// Camera capture on macOS: AVFoundation, NV12 (420v) CVPixelBuffers kept on
// the GPU (cv_image.h): the preview shows them and the VideoToolbox encoder
// takes them without a copy.
#include "core/clock.h"
#include "video/cv_image.h"
#include "video/video_source.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>

#include <QLoggingCategory>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <tuple>

Q_LOGGING_CATEGORY(lcCamMac, "p2p.camera")

using p2p::video::VideoFrame;
using FrameCallback = std::function<void(std::shared_ptr<VideoFrame>)>;

// Sample buffers from AVFoundation's queue -> VideoFrames.
@interface P2PCameraDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
@public
    FrameCallback cb;
    std::atomic<bool> running;
    int width, height; // frames of any other size (before the format switch) are skipped
    uint32_t seq;
    uint64_t dropped;
    int timingN;
    double timingLag;
}
@end

@implementation P2PCameraDelegate
- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sb
           fromConnection:(AVCaptureConnection*)connection
{
    if (!running.load(std::memory_order_relaxed)) return;
    CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb);
    if (!pb || int(CVPixelBufferGetWidth(pb)) != width || int(CVPixelBufferGetHeight(pb)) != height) return;
    // Timestamps are on the host clock (mach_absolute_time, stops in sleep);
    // nowUs() keeps counting through sleep: carry over the age instead.
    const int64_t now = p2p::nowUs();
    const double age = CMTimeGetSeconds(CMClockGetTime(CMClockGetHostTimeClock())) -
                       CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sb));
    auto f = std::make_shared<VideoFrame>();
    f->width = width;
    f->height = height;
    f->colour = CVPixelBufferGetPixelFormatType(pb) == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                    ? p2p::video::Colour::Bt601Full
                    : p2p::video::Colour::Bt709Limited;
    f->gpu = p2p::video::wrapCvPixelBuffer(pb);
    f->captureUs = age >= 0 && age < 1.0 ? now - int64_t(age * 1e6) : now;
    f->id = seq++;
    timingLag += double(now - f->captureUs) / 1000.0;
    if (++timingN == 300) {
        qCInfo(lcCamMac, "camera timing over %d frames: timestamp->delivery %.1f ms, dropped %llu", timingN,
               timingLag / timingN, (unsigned long long)dropped);
        timingN = 0;
        timingLag = 0;
    }
    cb(std::move(f));
}

- (void)captureOutput:(AVCaptureOutput*)output
    didDropSampleBuffer:(CMSampleBufferRef)sb
         fromConnection:(AVCaptureConnection*)connection
{
    ++dropped; // late: the previous frame was still being encoded
}
@end

namespace p2p::video {

namespace {

QString qs(NSString* s) { return QString::fromNSString(s); }

AVCaptureDevice* findDevice(const QString& want, QString* error)
{
    AVCaptureDeviceDiscoverySession* ds = [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:@[ AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal,
                                           AVCaptureDeviceTypeContinuityCamera ]
                              mediaType:AVMediaTypeVideo
                               position:AVCaptureDevicePositionUnspecified];
    // The Linux default (/dev/video0), "default" or nothing: the system's
    // default camera. Otherwise a unique ID or part of the name.
    if (want.isEmpty() || want == QLatin1String("default") || want.startsWith(QLatin1String("/dev/"))) {
        AVCaptureDevice* d = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        if (!d && error) *error = QStringLiteral("no camera found");
        return d;
    }
    QStringList names;
    for (AVCaptureDevice* d in ds.devices) {
        if (qs(d.uniqueID) == want || qs(d.localizedName).contains(want, Qt::CaseInsensitive)) return d;
        names << QStringLiteral("\"%1\" (%2)").arg(qs(d.localizedName), qs(d.uniqueID));
    }
    if (error)
        *error = QStringLiteral("no camera matching \"%1\"; cameras: %2")
                     .arg(want, names.isEmpty() ? QStringLiteral("none") : names.join(QStringLiteral(", ")));
    return nil;
}

double maxFps(AVCaptureDeviceFormat* f)
{
    double m = 0;
    for (AVFrameRateRange* r in f.videoSupportedFrameRateRanges) m = std::max(m, r.maxFrameRate);
    return m;
}

// The requested size if the camera has it, else the smallest larger one,
// else the largest; among equals the one with the highest frame rate.
// Landscape formats only.
AVCaptureDeviceFormat* pickFormat(AVCaptureDevice* dev, int w, int h)
{
    AVCaptureDeviceFormat* best = nil;
    auto rank = [&](AVCaptureDeviceFormat* f) {
        const CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(f.formatDescription);
        const bool covers = d.width >= w && d.height >= h;
        // Covering formats first, smallest area; then the rest, largest area.
        const double area = double(d.width) * d.height;
        return std::make_tuple(covers ? 1 : 0, covers ? -area : area, maxFps(f));
    };
    for (AVCaptureDeviceFormat* f in dev.formats) {
        const CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(f.formatDescription);
        if (d.width < d.height || maxFps(f) < 1) continue;
        if (!best || rank(f) > rank(best)) best = f;
    }
    return best;
}

class AvfCamera : public VideoSource {
public:
    explicit AvfCamera(const CaptureConfig& cfg) : cfg_(cfg) {}
    ~AvfCamera() override { stop(); }

    bool start(Callback cb, QString* error) override
    {
        const AVAuthorizationStatus auth = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
        if (auth == AVAuthorizationStatusDenied || auth == AVAuthorizationStatusRestricted) {
            if (error)
                *error = QStringLiteral("camera access denied: allow this app (or the terminal running it) in "
                                        "System Settings > Privacy & Security > Camera");
            return false;
        }
        device_ = findDevice(cfg_.device, error);
        if (!device_) return false;
        format_ = pickFormat(device_, cfg_.width, cfg_.height);
        if (!format_) {
            if (error) *error = QStringLiteral("camera \"%1\" has no usable format").arg(qs(device_.localizedName));
            return false;
        }
        const CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(format_.formatDescription);
        width_ = dim.width & ~1;
        height_ = dim.height & ~1;
        fps_ = std::max(1, std::min(cfg_.fps, int(maxFps(format_))));

        delegate_ = [P2PCameraDelegate new];
        delegate_->cb = std::move(cb);
        delegate_->width = width_;
        delegate_->height = height_;
        delegate_->running = true;
        queue_ = dispatch_queue_create("p2p.camera", DISPATCH_QUEUE_SERIAL);
        // Asking for access, opening the device and starting the session take
        // from ~250 ms to as long as the permission prompt stays up: done on
        // the capture queue, frames follow when it is running.
        alive_ = std::make_shared<Alive>();
        alive_->cam = this;
        if (auth == AVAuthorizationStatusNotDetermined) {
            qCInfo(lcCamMac) << "asking for camera access";
            // The answer can come after stop() (or never): it only opens the
            // camera if this source is still there.
            std::shared_ptr<Alive> alive = alive_;
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                                     completionHandler:^(BOOL granted) {
                                         std::lock_guard l(alive->m);
                                         if (!granted)
                                             qCWarning(lcCamMac) << "camera access denied: no video will be sent";
                                         else if (AvfCamera* cam = alive->cam)
                                             dispatch_async(cam->queue_, ^{ cam->openOnQueue(); });
                                     }];
        } else {
            dispatch_async(queue_, ^{ this->openOnQueue(); });
        }
        qCInfo(lcCamMac).noquote() << "camera:" << description();
        return true;
    }

    void stop() override
    {
        if (!queue_) return;
        {
            std::lock_guard l(alive_->m);
            alive_->cam = nullptr;
        }
        delegate_->running = false;
        // After this no callback runs: the session is stopped on the same
        // queue and the delegate's queue drained.
        dispatch_sync(queue_, ^{
            if (session_) [session_ stopRunning];
        });
        if (output_) {
            [output_ setSampleBufferDelegate:nil queue:nil];
            dispatch_sync(outputQueue_, ^{});
        }
        session_ = nil;
        output_ = nil;
        queue_ = nil;
        outputQueue_ = nil;
        delegate_ = nil;
    }

    QString description() const override
    {
        return QStringLiteral("AVFoundation \"%1\" %2x%3 420v @ %4 fps")
            .arg(device_ ? qs(device_.localizedName) : QStringLiteral("?"))
            .arg(width_)
            .arg(height_)
            .arg(fps_);
    }
    int fps() const override { return fps_; }

private:
    void openOnQueue()
    {
        if (!delegate_->running) return; // stopped while waiting for access
        NSError* err = nil;
        AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device_ error:&err];
        if (!input) {
            qCWarning(lcCamMac).noquote() << "camera: cannot open" << qs(device_.localizedName) << ":"
                                          << qs(err.localizedDescription);
            return;
        }
        AVCaptureSession* session = [AVCaptureSession new];
        AVCaptureVideoDataOutput* output = [AVCaptureVideoDataOutput new];
        // Late frames are dropped (we keep latency, not every frame).
        output.alwaysDiscardsLateVideoFrames = YES;
        output.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)};
        outputQueue_ = dispatch_queue_create("p2p.camera.frames", DISPATCH_QUEUE_SERIAL);
        [output setSampleBufferDelegate:delegate_ queue:outputQueue_];
        [session beginConfiguration];
        if (![session canAddInput:input] || ![session canAddOutput:output]) {
            qCWarning(lcCamMac) << "camera: cannot set up a capture session";
            return;
        }
        [session addInput:input];
        [session addOutput:output];
        [session commitConfiguration];
        // startRunning applies the session preset (1080p here), so the format
        // goes on after it; the first frames at the preset's size are skipped.
        [session startRunning];
        if ([device_ lockForConfiguration:&err]) {
            device_.activeFormat = format_;
            device_.activeVideoMinFrameDuration = CMTimeMake(1, fps_);
            device_.activeVideoMaxFrameDuration = CMTimeMake(1, fps_);
            [device_ unlockForConfiguration];
        } else {
            qCWarning(lcCamMac).noquote() << "camera: cannot configure:" << qs(err.localizedDescription);
        }
        session_ = session;
        output_ = output;
    }

    // Lets a late answer to the access request see whether we still exist.
    struct Alive {
        std::mutex m;
        AvfCamera* cam = nullptr;
    };
    std::shared_ptr<Alive> alive_;

    CaptureConfig cfg_;
    AVCaptureDevice* device_ = nil;
    AVCaptureDeviceFormat* format_ = nil;
    AVCaptureSession* session_ = nil;
    AVCaptureVideoDataOutput* output_ = nil;
    P2PCameraDelegate* delegate_ = nil;
    dispatch_queue_t queue_ = nil;
    dispatch_queue_t outputQueue_ = nil;
    int width_ = 0, height_ = 0, fps_ = 0;
};

} // namespace

std::unique_ptr<VideoSource> createCameraSource(const CaptureConfig& cfg) { return std::make_unique<AvfCamera>(cfg); }

} // namespace p2p::video
