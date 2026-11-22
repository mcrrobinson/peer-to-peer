#pragma once

#include "audio/audio_pipeline.h"
#include "net/crypto.h"
#include "media/codec_caps.h"
#include "net/media_transport.h"
#include "video/video_stream.h"

#include <QHostAddress>
#include <QString>

#include <memory>
#include <optional>

namespace p2p::app {

// Media plane for one call: the encrypted UDP transport, the audio
// pipeline, and the camera video send/receive streams.
class MediaSession {
public:
    struct Config {
        audio::AudioPipeline::Config audio;
        video::VideoStreamConfig video;
        double simulatedLoss = 0.0; // fraction of outgoing packets to drop (testing)
        double simulatedBurst = 1.0; // mean loss burst length in packets (testing)
        net::MediaTransport::LinkSim linkSim; // simulated bottleneck on outgoing media (testing)
    };

    explicit MediaSession(Config cfg);
    ~MediaSession();

    bool open(QString* error);                 // binds the UDP socket
    uint16_t localPort() const { return transport_.localPort(); }

    // |sendCamera| / |recvCamera|: negotiated codecs for each direction
    // (nullopt = no video that way).
    bool start(const crypto::SessionKeys& keys, const QHostAddress& remote, uint16_t remotePort,
               const std::optional<media::StreamSelection>& sendCamera,
               const std::optional<media::StreamSelection>& recvCamera, QString* error);
    void stop();

    void setClockOffsetUs(int64_t us);
    void setRttUs(int64_t us);
    void setMuted(bool m);
    // Live AEC / NS / AGC switching; also remembered for a later start().
    void setAudioProcessing(bool echoCancel, bool noiseSuppress, bool autoGain, int noiseLevel);

    QString statsText() const;
    audio::AudioPipeline::Stats audioStats() const;
    net::MediaTransport::Stats transportStats() const { return transport_.stats(); }
    std::optional<video::VideoSendStream::Stats> videoSendStats() const;
    std::optional<video::VideoReceiveStream::Stats> videoRecvStats() const;

private:
    Config cfg_;
    net::MediaTransport transport_;
    std::unique_ptr<audio::AudioPipeline> audio_;
    std::unique_ptr<video::VideoSendStream> videoSend_;
    std::unique_ptr<video::VideoReceiveStream> videoRecv_;
    bool started_ = false;
};

} // namespace p2p::app
