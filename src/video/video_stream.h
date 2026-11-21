#pragma once

#include "media/codec_caps.h"
#include "video/video_decoder.h"
#include "video/video_encoder.h"
#include "video/video_packetizer.h"
#include "video/video_source.h"

#include <QString>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace p2p::net {
class MediaTransport;
}

namespace p2p::video {

struct VideoStreamConfig {
    enum class Source { Camera, Pattern, None };
    Source source = Source::Camera;
    CaptureConfig capture;
    int bitrateKbps = 12000;
    bool fallbackToPattern = true; // camera unavailable (e.g. held by another instance)
};

// Feedback payloads on the video stream ids.
enum class VideoFeedback : uint8_t { PictureLoss = 1 };

// Capture -> encode -> packetize -> transport. Runs on the source's thread.
class VideoSendStream {
public:
    struct Stats {
        uint64_t captured = 0, encoded = 0, keyframes = 0, pliReceived = 0, bytes = 0, encodeErrors = 0;
        double encodeAvgMs = 0, encodeMaxMs = 0;
        double kbps = 0, fps = 0;
        int width = 0, height = 0;
        QString encoder, source;
    };

    VideoSendStream(VideoStreamConfig cfg, media::StreamSelection sel, net::MediaTransport& t, uint8_t streamId,
                    QString previewName);
    ~VideoSendStream();

    void attach();                 // before transport.start()
    bool start(QString* error);
    void stop();
    Stats stats() const;

private:
    void onFrame(std::shared_ptr<VideoFrame> f);

    VideoStreamConfig cfg_;
    media::StreamSelection sel_;
    net::MediaTransport& transport_;
    uint8_t streamId_;
    QString previewName_;
    std::unique_ptr<VideoSource> source_;
    std::unique_ptr<VideoEncoder> encoder_;
    bool encoderFailed_ = false;
    uint32_t frameId_ = 0;
    std::atomic<bool> forceKeyframe_{true};
    bool sendingPattern_ = false;

    mutable std::mutex statsMutex_;
    Stats st_;
    double encodeSumMs_ = 0;
    mutable uint64_t rateBytes_ = 0, rateFrames_ = 0;
    mutable int64_t rateAtUs_ = 0;
    mutable double kbps_ = 0, fps_ = 0;
};

// Transport -> frame assembly -> decode thread -> FrameHub.
class VideoReceiveStream {
public:
    struct Stats {
        FrameAssembler::Stats assembler;
        uint64_t decoded = 0, decodeErrors = 0, pliSent = 0, patternOk = 0, patternBad = 0;
        double decodeAvgMs = 0;
        double latencyAvgMs = -1, latencyP95Ms = -1; // capture -> decoded (excl. display)
        double kbps = 0, fps = 0;
        int width = 0, height = 0;
        QString decoder;
    };

    VideoReceiveStream(media::StreamSelection sel, net::MediaTransport& t, uint8_t streamId, QString hubName);
    ~VideoReceiveStream();

    void attach();
    bool start(QString* error);
    void stop();
    void setClockOffsetUs(int64_t us) { clockOffsetUs_ = us; }
    Stats stats() const;

private:
    void onFragment(const uint8_t* data, size_t len, int64_t arrivalUs);
    void decodeLoop();
    void requestKeyframe(int64_t nowUs);

    media::StreamSelection sel_;
    net::MediaTransport& transport_;
    uint8_t streamId_;
    QString hubName_;
    VideoDecoder decoder_;

    std::mutex m_;
    std::condition_variable cv_;
    FrameAssembler assembler_;
    uint64_t rxBytes_ = 0;
    bool wake_ = false;

    std::atomic<bool> running_{false};
    std::thread thread_;
    std::atomic<int64_t> clockOffsetUs_{0};
    int64_t lastPliUs_ = 0;
    uint32_t lastPattern_ = 0;
    bool havePattern_ = false;

    mutable std::mutex statsMutex_;
    Stats st_;
    double decodeSumMs_ = 0;
    std::deque<double> latencies_;
    mutable uint64_t rateBytes_ = 0, rateFrames_ = 0;
    mutable int64_t rateAtUs_ = 0;
    mutable double kbps_ = 0, fps_ = 0;
    std::atomic<uint64_t> bytesIn_{0};
};

} // namespace p2p::video
