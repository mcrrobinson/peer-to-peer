#pragma once

#include "media/codec_caps.h"
#include "video/video_decoder.h"
#include "video/video_encoder.h"
#include "video/rate_ladder.h"
#include "video/video_packetizer.h"
#include "video/video_source.h"

#include <QString>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

struct SwsContext;

namespace p2p::net {
class MediaTransport;
}

namespace p2p::video {

struct VideoStreamConfig {
    enum class Source { Camera, Pattern, Flash, Screen, None };
    enum class Fec { Off, Auto, Fixed };
    Source source = Source::Camera;
    CaptureConfig capture;             // camera, and the pattern's size / rate
    ScreenConfig screen;               // Source::Screen
    QString flashLog;                  // Source::Flash: where its switch times go
    std::function<void(QString)> onSourceEnded; // the screen source stopped by itself
    int bitrateKbps = 12000;       // the maximum with congestion control, else fixed
    int minBitrateKbps = 300;
    bool congestionControl = true; // follow the transport's target via the rate ladder
    bool fallbackToPattern = true; // camera unavailable (e.g. held by another instance)
    Fec fec = Fec::Auto;
    double fecFixedRate = 0.2;     // parity / data for Fec::Fixed
    bool nack = true;
};

// Feedback payloads on the video stream ids (first byte = kind):
//   PictureLoss: no body.
//   Nack:        repeated { u32 frame id, u16 fragment index (0xffff = whole frame) }.
// (Kind 3, a video-only loss report, was replaced by transport-wide
// congestion feedback.)
// Recover: u32 id of the last frame the receiver decoded; it lost what came
// after. The sender answers with a frame predicting from it (or a keyframe).
enum class VideoFeedback : uint8_t { PictureLoss = 1, Nack = 2, Recover = 3 };

// FEC parity share for the sender's loss estimate: off when the path is
// clean, otherwise enough to cover roughly twice the loss rate; keyframes
// get more because losing one costs a full refresh.
double autoFecRate(double loss, bool keyframe);

// Capture -> [scale] -> encode -> packetize -> pacer/transport. Runs on the
// source's thread. With congestion control the encoder follows a rate
// ladder (re-opened on each rung change) and frames are skipped while the
// pacer is backed up by more than 1.5 frame intervals.
class VideoSendStream {
public:
    struct Stats {
        uint64_t captured = 0, encoded = 0, keyframes = 0, pliReceived = 0, bytes = 0, encodeErrors = 0;
        uint64_t recoverReceived = 0, recoveries = 0; // requests; frames sent that predict from the receiver's last
        uint64_t nackItems = 0, retransmitted = 0, retransmitMisses = 0, parityFragments = 0;
        double encodeAvgMs = 0, encodeMaxMs = 0;
        double kbps = 0, fps = 0;
        double reportedLoss = 0, fecRate = 0; // loss: the congestion controller's estimate
        int width = 0, height = 0;            // encoded size
        uint64_t framesSkipped = 0, rungSwitches = 0;
        int rungKbps = 0, rungIndex = 0, rungCount = 0;
        double budgetKbps = 0, reopenMaxMs = 0;
        QString encoder, source;
        // Latency breakdown, avg/p95 ms: source (capture -> handed to us:
        // camera driver, MJPEG decode, screen convert), prep (scaling, rate
        // control), encode (until the bitstream is out).
        QString stages;
    };

    VideoSendStream(VideoStreamConfig cfg, media::StreamSelection sel, net::MediaTransport& t, uint8_t streamId,
                    QString previewName);
    ~VideoSendStream();

    void attach();                 // before transport.start()
    bool start(QString* error);    // may be called again after stop()
    void stop();
    bool running() const { return running_; }
    void setScreenConfig(const ScreenConfig& s) { cfg_.screen = s; } // while stopped
    // Share of the controller's video budget this stream may use (camera
    // and screen split it while both run).
    void setBudgetShare(double s) { share_ = s; }
    Stats stats() const;

private:
    struct SentFrame {
        uint32_t id = 0;
        int64_t sentUs = 0;
        size_t bytes = 0;
        std::vector<std::vector<uint8_t>> fragments; // data fragments, full payloads
    };
    void onFrame(std::shared_ptr<VideoFrame> f);
    void onFeedback(const uint8_t* d, size_t n);
    void resend(const std::vector<uint8_t>& payload);
    bool resendFits(size_t bytes) const;
    bool openEncoder(const Rung& r, int fps);
    const VideoFrame* scaled(const VideoFrame& f, int w, int h);

    VideoStreamConfig cfg_;
    media::StreamSelection sel_;
    net::MediaTransport& transport_;
    uint8_t streamId_;
    QString previewName_;
    std::unique_ptr<VideoSource> source_;
    std::mutex sourceMutex_; // source_ vs. keyframe requests from the receive thread
    std::atomic<bool> running_{false};
    std::atomic<double> share_{1.0};
    int ladderSrcW_ = 0, ladderSrcH_ = 0;
    std::unique_ptr<VideoEncoder> encoder_;
    bool encoderFailed_ = false;
    int encW_ = 0, encH_ = 0, encKbps_ = 0; // what the open encoder makes
    bool liveRate_ = false; // the encoder retargets without re-opening: same-size rung switches keep it
    Colour encoderColour_ = Colour::Bt709Limited;
    std::unique_ptr<RungSelector> ladder_;
    SwsContext* sws_ = nullptr;
    VideoFrame scaledFrame_;
    double lastFecRate_ = 0;
    double fecCap_ = 1.0; // auto FEC may only use the budget left above the current rung
    uint32_t frameId_ = 0;
    std::atomic<bool> forceKeyframe_{true};
    std::atomic<int64_t> recoverFrom_{-1}; // frame id from a Recover request, -1 = none
    std::atomic<int64_t> skipQueueUs_{50'000}; // pacer backlog above which new frames are skipped
    bool sendingPattern_ = false;
    std::atomic<double> lossEstimate_{0.0};

    std::mutex historyMutex_;
    std::deque<SentFrame> history_;
    size_t historyBytes_ = 0;

    mutable std::mutex statsMutex_;
    Stats st_;
    double encodeSumMs_ = 0;
    StageStats stages_{{"source", "prep", "encode"}};
    mutable uint64_t rateBytes_ = 0, rateFrames_ = 0;
    mutable int64_t rateAtUs_ = 0;
    mutable double kbps_ = 0, fps_ = 0;
};

// Transport -> frame assembly (FEC, NACK) -> decode thread -> FrameHub.
class VideoReceiveStream {
public:
    struct Stats {
        FrameAssembler::Stats assembler;
        uint64_t decoded = 0, decodeErrors = 0, pliSent = 0, recoverSent = 0, patternOk = 0, patternBad = 0;
        uint64_t recoveredDecoded = 0; // frames that needed FEC or a resend
        double decodeAvgMs = 0;
        double latencyAvgMs = -1, latencyP95Ms = -1; // capture -> decoded (excl. display)
        // Latency breakdown, avg/p95 ms: encode (sender capture -> encoded),
        // send (-> first packet here: pacer + network), spread (first -> last
        // packet or recovery), wait (complete -> decode start), decode.
        QString stages;
        FrameHub::DisplayStats display; // decoded -> on screen (GUI only)
        double kbps = 0, fps = 0;
        double pathLoss = 0; // last measured, before recovery
        int width = 0, height = 0;
        QString decoder;
    };

    VideoReceiveStream(media::StreamSelection sel, net::MediaTransport& t, uint8_t streamId, QString hubName,
                       bool nack = true);
    ~VideoReceiveStream();

    void attach();
    bool start(QString* error);
    void stop();
    void setClockOffsetUs(int64_t us) { clockOffsetUs_ = us; }
    void setRttUs(int64_t us) { rttUs_ = us; }
    // The sender restarted its source (screen share stopped): its test
    // pattern numbering starts again.
    void resetPatternCheck() { resetPattern_ = true; }
    Stats stats() const;

private:
    void onFragment(const uint8_t* data, size_t len, int64_t arrivalUs);
    void decodeLoop();
    void requestKeyframe(int64_t nowUs);
    // After a loss: ask the sender to predict from |good| (the last frame
    // decoded); a keyframe if there is none or that keeps failing.
    void requestRecovery(int64_t nowUs, std::optional<uint32_t> good, int64_t rttUs);
    void sendNacks(const std::vector<NackItem>& items);
    void updatePathLoss(int64_t nowUs);

    media::StreamSelection sel_;
    net::MediaTransport& transport_;
    uint8_t streamId_;
    QString hubName_;
    bool nack_;
    VideoDecoder decoder_;

    std::mutex m_;
    std::condition_variable cv_;
    FrameAssembler assembler_;
    bool wake_ = false;

    std::atomic<bool> running_{false};
    std::thread thread_;
    std::atomic<int64_t> clockOffsetUs_{0};
    std::atomic<int64_t> rttUs_{1000};
    int64_t lastPliUs_ = 0;
    int recoverTries_ = 0; // requests for the current loss
    int64_t firstFragmentUs_ = 0; // decode thread's first sight of the stream
    uint32_t lastPattern_ = 0;
    bool havePattern_ = false;
    std::atomic<bool> resetPattern_{false};
    int64_t lastReportUs_ = 0;
    uint64_t reportExpected_ = 0, reportReceived_ = 0;

    mutable std::mutex statsMutex_;
    Stats st_;
    double decodeSumMs_ = 0;
    StageStats stages_{{"encode", "send", "spread", "wait", "decode"}};
    std::deque<double> latencies_;
    mutable uint64_t rateBytes_ = 0, rateFrames_ = 0;
    mutable int64_t rateAtUs_ = 0;
    mutable double kbps_ = 0, fps_ = 0;
    std::atomic<uint64_t> bytesIn_{0};
};

} // namespace p2p::video
