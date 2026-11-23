// Screen capture on macOS: ScreenCaptureKit. The system's content picker
// (SCContentSharingPicker) chooses a display or window -- no Screen
// Recording permission needed for that -- or, for scripted tests, the main
// display is captured directly (needs the permission). Frames are 420v
// BT.709 CVPixelBuffers, scaled to fit on the GPU by ScreenCaptureKit and
// kept there (cv_image.h). An idle screen sends nothing: the last picture
// is repeated about once a second and on request, like the PipeWire source.
#include "core/clock.h"
#include "video/cv_image.h"
#include "video/video_source.h"

#import <CoreGraphics/CoreGraphics.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <QLoggingCategory>

#include <algorithm>
#include <atomic>
#include <mach/mach_time.h>
#include <mutex>

Q_LOGGING_CATEGORY(lcScreenMac, "p2p.screen")

namespace p2p::video {
class SckScreenSource;
}

@interface P2PScreenCapture : NSObject <SCStreamOutput, SCStreamDelegate, SCContentSharingPickerObserver> {
@public
    p2p::video::SckScreenSource* owner; // cleared (under ownerMutex) by stop()
    std::mutex ownerMutex;
}
@end

namespace p2p::video {

namespace {
constexpr int64_t kIdleRepeatUs = 1'000'000;

double hostNowSec() { return CMTimeGetSeconds(CMClockGetTime(CMClockGetHostTimeClock())); }

double machToSec(uint64_t t)
{
    static const mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t i;
        mach_timebase_info(&i);
        return i;
    }();
    return double(t) * tb.numer / tb.denom / 1e9;
}

QString qs(NSString* s) { return QString::fromNSString(s); }
} // namespace

class SckScreenSource : public VideoSource {
public:
    SckScreenSource(ScreenConfig cfg, std::function<void(QString)> onEnded)
        : cfg_(std::move(cfg)), onEnded_(std::move(onEnded))
    {
    }
    ~SckScreenSource() override { stop(); }

    bool start(Callback cb, QString* error) override
    {
        (void)error;
        if (running_) return true;
        cb_ = std::move(cb);
        running_ = true;
        ended_ = false;
        frames_ = dispatch_queue_create("p2p.screen.frames", DISPATCH_QUEUE_SERIAL);
        capture_ = [P2PScreenCapture new];
        capture_->owner = this;
        // Idle repeats and requestFrame() run on the frames queue, so the
        // callback never runs twice at once.
        timer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, frames_);
        dispatch_source_set_timer(timer_, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), 100 * NSEC_PER_MSEC,
                                  10 * NSEC_PER_MSEC);
        SckScreenSource* self = this;
        dispatch_source_set_event_handler(timer_, ^{ self->repeatIfIdle(false); });
        dispatch_resume(timer_);

        if (cfg_.mainDisplay) startMainDisplay();
        else startPicker();
        return true;
    }

    void stop() override
    {
        if (!running_) return;
        running_ = false;
        {
            std::lock_guard l(capture_->ownerMutex);
            capture_->owner = nullptr;
        }
        if (pickerActive_) {
            dispatch_block_t off = ^{
                SCContentSharingPicker* p = SCContentSharingPicker.sharedPicker;
                [p removeObserver:capture_];
                p.active = NO;
            };
            if ([NSThread isMainThread]) off();
            else dispatch_sync(dispatch_get_main_queue(), off);
            pickerActive_ = false;
        }
        SCStream* stream = nil;
        {
            std::lock_guard l(m_);
            stream = stream_;
            stream_ = nil;
        }
        if (stream) {
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            [stream stopCaptureWithCompletionHandler:^(NSError*) { dispatch_semaphore_signal(done); }];
            dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
        }
        dispatch_source_cancel(timer_);
        dispatch_sync(frames_, ^{}); // no callback after this
        timer_ = nil;
        frames_ = nil;
        capture_ = nil;
        last_.reset();
    }

    QString description() const override
    {
        std::lock_guard l(m_);
        if (width_ == 0)
            return cfg_.mainDisplay ? QStringLiteral("screen (main display, starting)")
                                    : QStringLiteral("screen (waiting for the picker)");
        return QStringLiteral("screen %1x%2 %3 (ScreenCaptureKit%4, up to %5 fps)")
            .arg(width_)
            .arg(height_)
            .arg(cfg_.mode == ScreenConfig::Mode::Text ? "text" : "motion")
            .arg(cfg_.mainDisplay ? QStringLiteral(", main display") : QString())
            .arg(cfg_.maxFps);
    }
    int fps() const override { return cfg_.maxFps; }

    void requestFrame() override
    {
        if (!running_) return;
        SckScreenSource* self = this;
        dispatch_async(frames_, ^{ self->repeatIfIdle(true); });
    }

    // ---- from P2PScreenCapture (owner checked under its mutex) ----
    void picked(SCContentFilter* filter) { open(filter); }
    void ended(const QString& why)
    {
        if (ended_.exchange(true)) return;
        qCInfo(lcScreenMac).noquote() << "screen share ended:" << why;
        if (onEnded_) onEnded_(why);
    }
    void frame(CMSampleBufferRef sb) // frames queue
    {
        CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
        if (!att || CFArrayGetCount(att) == 0) return;
        NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(att, 0);
        // Idle: nothing changed (no picture); Blank / Suspended / Stopped: none either.
        if (SCFrameStatus([info[SCStreamFrameInfoStatus] integerValue]) != SCFrameStatusComplete) return;
        CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb);
        if (!pb) return;
        const int64_t now = nowUs();
        auto f = std::make_shared<VideoFrame>();
        f->width = int(CVPixelBufferGetWidth(pb)) & ~1;
        f->height = int(CVPixelBufferGetHeight(pb)) & ~1;
        f->colour = Colour::Bt709Limited;
        f->gpu = wrapCvPixelBuffer(pb);
        f->captureUs = now;
        if (NSNumber* t = info[SCStreamFrameInfoDisplayTime]) {
            const double age = hostNowSec() - machToSec(t.unsignedLongLongValue);
            if (age >= 0 && age < 0.2) f->captureUs = now - int64_t(age * 1e6);
        }
        f->id = frameNo_++;
        {
            std::lock_guard l(m_);
            width_ = f->width;
            height_ = f->height;
        }
        last_ = f;
        lastDeliveredUs_ = now;
        if (running_) cb_(std::move(f));
    }

private:
    void repeatIfIdle(bool now) // frames queue
    {
        if (!running_ || !last_ || (!now && nowUs() - lastDeliveredUs_ < kIdleRepeatUs)) return;
        auto f = std::make_shared<VideoFrame>(*last_); // same CVPixelBuffer
        f->captureUs = nowUs();
        f->repeated = true;
        f->id = frameNo_++;
        lastDeliveredUs_ = f->captureUs;
        cb_(std::move(f));
    }

    void startPicker()
    {
        P2PScreenCapture* cap = capture_;
        pickerActive_ = true;
        dispatch_block_t show = ^{
            SCContentSharingPicker* p = SCContentSharingPicker.sharedPicker;
            SCContentSharingPickerConfiguration* pc = [SCContentSharingPickerConfiguration new];
            pc.allowedPickerModes = SCContentSharingPickerModeSingleDisplay | SCContentSharingPickerModeSingleWindow |
                                    SCContentSharingPickerModeSingleApplication;
            p.defaultConfiguration = pc;
            p.maximumStreamCount = @1;
            [p addObserver:cap];
            p.active = YES;
            [p present];
        };
        // Always queued: presented before the app has finished launching (a
        // source started before the event loop runs) the picker never shows.
        dispatch_async(dispatch_get_main_queue(), show);
        qCInfo(lcScreenMac) << "screen: waiting for the picker";
    }

    void startMainDisplay()
    {
        if (!CGPreflightScreenCaptureAccess()) {
            CGRequestScreenCaptureAccess(); // shows the system prompt once
            ended(QStringLiteral("Screen Recording access needed for --screen-source display: allow this app (or the "
                                 "terminal running it) in System Settings > Privacy & Security > Screen & System "
                                 "Audio Recording, then restart it"));
            return;
        }
        P2PScreenCapture* cap = capture_;
        [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent* content, NSError* err) {
            std::lock_guard l(cap->ownerMutex);
            SckScreenSource* self = cap->owner;
            if (!self) return;
            const CGDirectDisplayID main = CGMainDisplayID();
            SCDisplay* display = nil;
            for (SCDisplay* d in content.displays)
                if (d.displayID == main) display = d;
            if (!display) display = content.displays.firstObject;
            if (!display) {
                self->ended(QStringLiteral("no display to capture: %1")
                                .arg(err ? qs(err.localizedDescription) : QStringLiteral("none listed")));
                return;
            }
            self->open([[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]]);
        }];
    }

    // A filter from the picker or for the main display: start the stream, or
    // switch an open one to it (the user picked something else from the
    // menu bar's sharing menu).
    void open(SCContentFilter* filter)
    {
        const double pw = filter.contentRect.size.width * filter.pointPixelScale;
        const double ph = filter.contentRect.size.height * filter.pointPixelScale;
        const double scale = std::min({1.0, cfg_.maxWidth / std::max(1.0, pw), cfg_.maxHeight / std::max(1.0, ph)});
        SCStreamConfiguration* c = [SCStreamConfiguration new];
        c.width = std::max<size_t>(16, size_t(pw * scale) & ~size_t(1));
        c.height = std::max<size_t>(16, size_t(ph * scale) & ~size_t(1));
        c.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        c.colorMatrix = kCGDisplayStreamYCbCrMatrix_ITU_R_709_2;
        c.colorSpaceName = kCGColorSpaceITUR_709;
        c.minimumFrameInterval = CMTimeMake(1, std::max(1, cfg_.maxFps));
        c.queueDepth = 6; // we hold one (idle repeats) plus those in flight
        c.showsCursor = YES;
        c.scalesToFit = YES; // a resized window keeps filling the frame
        c.preservesAspectRatio = YES;
        qCInfo(lcScreenMac, "screen: capturing %.0fx%.0f px as %zux%zu at up to %d fps", pw, ph, c.width, c.height,
               cfg_.maxFps);

        SCStream* existing = nil;
        {
            std::lock_guard l(m_);
            existing = stream_;
        }
        if (existing) {
            [existing updateContentFilter:filter completionHandler:nil];
            [existing updateConfiguration:c completionHandler:nil];
            return;
        }
        SCStream* stream = [[SCStream alloc] initWithFilter:filter configuration:c delegate:capture_];
        NSError* err = nil;
        if (![stream addStreamOutput:capture_ type:SCStreamOutputTypeScreen sampleHandlerQueue:frames_ error:&err]) {
            ended(QStringLiteral("screen capture failed: %1").arg(qs(err.localizedDescription)));
            return;
        }
        {
            std::lock_guard l(m_);
            stream_ = stream;
        }
        P2PScreenCapture* cap = capture_;
        [stream startCaptureWithCompletionHandler:^(NSError* e) {
            if (!e) return;
            std::lock_guard l(cap->ownerMutex);
            if (cap->owner) cap->owner->ended(QStringLiteral("screen capture failed: %1").arg(qs(e.localizedDescription)));
        }];
    }

    ScreenConfig cfg_;
    std::function<void(QString)> onEnded_;
    Callback cb_;
    std::atomic<bool> running_{false}, ended_{false};
    bool pickerActive_ = false;
    P2PScreenCapture* capture_ = nil;
    dispatch_queue_t frames_ = nil;
    dispatch_source_t timer_ = nil;
    mutable std::mutex m_; // stream_, width_, height_
    SCStream* stream_ = nil;
    int width_ = 0, height_ = 0;
    // Frames queue only:
    std::shared_ptr<VideoFrame> last_;
    int64_t lastDeliveredUs_ = 0;
    uint32_t frameNo_ = 0;
};

std::unique_ptr<VideoSource> createScreenSource(const ScreenConfig& cfg, std::function<void(QString)> onEnded)
{
    return std::make_unique<SckScreenSource>(cfg, std::move(onEnded));
}

} // namespace p2p::video

@implementation P2PScreenCapture
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sb ofType:(SCStreamOutputType)type
{
    if (type != SCStreamOutputTypeScreen) return;
    // The frames queue; stop() drains it after clearing |owner|.
    p2p::video::SckScreenSource* o = nullptr;
    {
        std::lock_guard l(ownerMutex);
        o = owner;
    }
    if (o) o->frame(sb);
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error
{
    // The user stopped sharing (menu bar), the window closed, or a failure.
    std::lock_guard l(ownerMutex);
    if (owner) owner->ended(QStringLiteral("sharing stopped: %1").arg(QString::fromNSString(error.localizedDescription)));
}

- (void)contentSharingPicker:(SCContentSharingPicker*)picker
         didUpdateWithFilter:(SCContentFilter*)filter
                   forStream:(SCStream*)stream
{
    std::lock_guard l(ownerMutex);
    if (owner) owner->picked(filter);
}

- (void)contentSharingPicker:(SCContentSharingPicker*)picker didCancelForStream:(SCStream*)stream
{
    std::lock_guard l(ownerMutex);
    if (owner) owner->ended(QStringLiteral("cancelled in the picker"));
}

- (void)contentSharingPickerStartDidFailWithError:(NSError*)error
{
    std::lock_guard l(ownerMutex);
    if (owner) owner->ended(QStringLiteral("picker failed: %1").arg(QString::fromNSString(error.localizedDescription)));
}
@end
