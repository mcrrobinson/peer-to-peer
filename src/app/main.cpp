#include "app/call_controller.h"
#include "net/crypto.h"
#include "video/codec_probe.h"

#include <QCommandLineParser>
#include <QDir>
#include <QStandardPaths>
#include <QCoreApplication>
#include "video/video_decoder.h"
#include "video/video_encoder.h"

#include <QGuiApplication>
#include <QHostInfo>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <memory>

using p2p::app::CallController;

namespace {
constexpr quint16 kBasePort = 47100;

bool wantsHeadless(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (qstrcmp(argv[i], "--headless") == 0) return true;
    return false;
}
} // namespace

int main(int argc, char** argv)
{
    // Headless mode (scripted tests / CI) must not require a display.
    const bool headless = wantsHeadless(argc, argv);
    std::unique_ptr<QCoreApplication> app;
    if (headless)
        app = std::make_unique<QCoreApplication>(argc, argv);
    else
        app = std::make_unique<QGuiApplication>(argc, argv);
    QCoreApplication::setApplicationName("p2pcall");
    QCoreApplication::setApplicationVersion("0.1.0");

    QCommandLineParser cli;
    cli.setApplicationDescription("Low-latency peer-to-peer video calls");
    cli.addHelpOption();
    cli.addVersionOption();
    QCommandLineOption instanceOpt("instance", "Instance number (selects default port 47100+N).", "N", "0");
    QCommandLineOption nameOpt("name", "Display name.", "name");
    QCommandLineOption portOpt("port", "Control port (0 = ephemeral).", "port");
    QCommandLineOption connectOpt("connect", "Call host:port on startup.", "host:port");
    QCommandLineOption autoAcceptOpt("auto-accept", "Accept incoming calls automatically.");
    QCommandLineOption noDiscoveryOpt("no-discovery", "Disable LAN discovery beacons.");
    QCommandLineOption headlessOpt("headless", "Run without UI (for scripted tests).");
    QCommandLineOption exitAfterOpt("exit-after-call", "Hang up N seconds after the call connects, then exit.", "seconds");
    QCommandLineOption preferAv1Opt("prefer-av1", "Prefer AV1 for: camera, screen or both.", "which");
    QCommandLineOption audioSourceOpt("audio-source", "Audio to send: mic, tone, silence or echo (loop back what is received).",
                                      "src", "mic");
    QCommandLineOption audioIoOpt("audio-io", "Audio I/O: device or null (simulated clock, no playback).", "io", "device");
    QCommandLineOption noApmOpt("no-audio-processing", "Disable all mic processing (AEC, NS, AGC).");
    QCommandLineOption aecOpt("aec", "Echo cancellation: on or off.", "on|off", "off");
    QCommandLineOption nsOpt("ns", "Noise suppression: off, low, moderate, high.", "level", "off");
    QCommandLineOption agcOpt("agc", "Automatic gain control: on or off.", "on|off", "off");
    QCommandLineOption audioBitrateOpt("audio-bitrate", "Opus bitrate in kbps.", "kbps", "48");
    QCommandLineOption audioLowDelayOpt("audio-lowdelay", "Opus restricted-low-delay (CELT only, no LBRR).");
    QCommandLineOption periodOpt("audio-period", "Audio device period in ms.", "ms", "10");
    QCommandLineOption simLossOpt("sim-loss", "Drop this percentage of outgoing media packets (testing).", "pct", "0");
    QCommandLineOption noMediaOpt("no-media", "Signalling only, no media plane.");
    cli.addOptions({instanceOpt, nameOpt, portOpt, connectOpt, autoAcceptOpt, noDiscoveryOpt, headlessOpt,
                    exitAfterOpt, preferAv1Opt, audioSourceOpt, audioIoOpt, noApmOpt, audioBitrateOpt,
                    audioLowDelayOpt, periodOpt, simLossOpt, noMediaOpt, aecOpt, nsOpt, agcOpt});
    QCommandLineOption videoSourceOpt("video-source", "Video to send: camera, pattern, flash (for tools/latency_probe) or none.", "src", "camera");
#ifdef Q_OS_MACOS
    QCommandLineOption cameraOpt("camera", "Camera: default, or a unique ID or part of the name.", "device", "default");
#else
    QCommandLineOption cameraOpt("camera", "V4L2 camera device.", "device", "/dev/video0");
#endif
    QCommandLineOption videoSizeOpt("video-size", "Capture size WxH.", "WxH", "1920x1080");
    QCommandLineOption videoFpsOpt("video-fps", "Capture frame rate.", "fps", "60");
    QCommandLineOption videoBitrateOpt("video-bitrate", "Video bitrate in kbps (the maximum with --cc on).", "kbps",
                                       "12000");
    QCommandLineOption cameraFormatOpt("camera-format", "Camera format: auto, mjpeg or nv12.", "fmt", "auto");
    QCommandLineOption decodeOutputOpt("decode-output", "Hardware-decoded video: gpu (shown without a copy) or cpu (read back first).", "where", "gpu");
    QCommandLineOption encoderOpt("encoder", "Hardware HEVC encoder: native (own VA-API / VideoToolbox encoder) or ffmpeg (FFmpeg's hevc_vaapi / hevc_videotoolbox).", "which", "native");
    QCommandLineOption cameraDecodeOpt("camera-decode", "Camera MJPEG decode: auto (hardware if available) or software.", "how", "auto");
    QCommandLineOption audioFecOpt("audio-fec", "Opus loss protection: lbrr (in-band FEC) or dred.", "mode",
                                   "lbrr");
    QCommandLineOption fecOpt("fec", "Video FEC: hybrid (default: per frame when its rounding fits the spare budget, else across frames), auto (per frame), window (across frames), off, or a fixed parity percentage.", "mode", "hybrid");
    QCommandLineOption noNackOpt("no-nack", "Disable video retransmission requests.");
    QCommandLineOption simBurstOpt("sim-burst", "Mean length of simulated loss bursts, in packets (testing).", "n",
                                   "1");
    QCommandLineOption ccOpt("cc", "Congestion control: on (video follows the link) or off (fixed bitrate).",
                             "on|off", "on");
    QCommandLineOption minBitrateOpt("min-video-bitrate", "Lowest video bitrate congestion control may pick, kbps.",
                                     "kbps", "300");
    QCommandLineOption simRateOpt("sim-rate", "Simulated bottleneck rate on outgoing media, kbps (testing).", "kbps",
                                  "0");
    QCommandLineOption simDelayOpt("sim-delay", "Simulated one-way delay on outgoing media, ms (testing).", "ms", "0");
    QCommandLineOption simQueueOpt("sim-queue", "Simulated bottleneck buffer, ms of drain time (testing).", "ms",
                                   "100");
    QCommandLineOption simCeOpt("sim-ce", "Simulated L4S marking: CE above this queue delay, ms (0 = off).", "ms",
                                "0");
    QCommandLineOption flashLogOpt("flash-log", "With --video-source flash: append switch times here.", "file",
                                   "flash-log.txt");
    QCommandLineOption shareScreenOpt("share-screen", "Start sharing the screen when the call connects: text or motion.",
                                      "mode");
    QCommandLineOption screenSourceOpt("screen-source",
                                       "Screen to share: portal (pick a screen/window), pattern (test) or none.", "src",
                                       "portal");
    QCommandLineOption screenBitrateOpt("screen-bitrate", "Screen share bitrate in kbps (the maximum with --cc on).",
                                        "kbps", "10000");
    cli.addOptions({videoSourceOpt, cameraOpt, videoSizeOpt, videoFpsOpt, videoBitrateOpt, cameraFormatOpt, cameraDecodeOpt, decodeOutputOpt, encoderOpt, fecOpt,
                    noNackOpt, simBurstOpt, audioFecOpt, ccOpt, minBitrateOpt, simRateOpt, simDelayOpt, simQueueOpt,
                    simCeOpt, flashLogOpt, shareScreenOpt, screenSourceOpt, screenBitrateOpt});
    cli.process(*app);

    if (!p2p::crypto::init()) {
        std::fprintf(stderr, "libsodium init failed\n");
        return 1;
    }

    const int instance = cli.value(instanceOpt).toInt();
    CallController::Options opts;
    opts.name = cli.isSet(nameOpt) ? cli.value(nameOpt)
                                   : QStringLiteral("%1-%2").arg(QHostInfo::localHostName()).arg(instance);
    opts.port = cli.isSet(portOpt) ? quint16(cli.value(portOpt).toUInt()) : quint16(kBasePort + instance);
    opts.autoAccept = cli.isSet(autoAcceptOpt);
    opts.discovery = !cli.isSet(noDiscoveryOpt);
    opts.caps = p2p::video::probeLocalCaps();

    using Source = p2p::audio::AudioPipeline::Source;
    const QString src = cli.value(audioSourceOpt);
    auto& audio = opts.mediaConfig.audio;
    if (src == "mic") audio.source = Source::Mic;
    else if (src == "tone") audio.source = Source::Tone;
    else if (src == "silence") audio.source = Source::Silence;
    else if (src == "echo") audio.source = Source::Echo;
    else {
        std::fprintf(stderr, "unknown --audio-source %s\n", qPrintable(src));
        return 1;
    }
    audio.useDevice = cli.value(audioIoOpt) != "null";
    const bool apm = !cli.isSet(noApmOpt);
    audio.echoCancel = apm && cli.value(aecOpt) == "on";
    audio.autoGain = apm && cli.value(agcOpt) == "on";
    const QString ns = cli.value(nsOpt);
    audio.noiseSuppress = apm && ns != "off";
    audio.noiseLevel = ns == "low" ? 0 : ns == "high" ? 2 : 1;
    audio.opus.bitrate = std::clamp(cli.value(audioBitrateOpt).toInt(), 6, 510) * 1000;
    audio.opus.restrictedLowDelay = cli.isSet(audioLowDelayOpt);
    audio.opus.inbandFec = cli.value(audioFecOpt) != "dred";
    audio.periodMs = std::clamp(cli.value(periodOpt).toInt(), 2, 40);
    opts.mediaConfig.simulatedLoss = std::clamp(cli.value(simLossOpt).toDouble(), 0.0, 100.0) / 100.0;
    opts.media = !cli.isSet(noMediaOpt);

    using VSource = p2p::video::VideoStreamConfig::Source;
    auto& video = opts.mediaConfig.video;
    const QString vsrc = cli.value(videoSourceOpt);
    if (vsrc == "camera") video.source = VSource::Camera;
    else if (vsrc == "pattern") video.source = VSource::Pattern;
    else if (vsrc == "flash") video.source = VSource::Flash;
    else if (vsrc == "none") video.source = VSource::None;
    else {
        std::fprintf(stderr, "unknown --video-source %s\n", qPrintable(vsrc));
        return 1;
    }
    const QStringList wh = cli.value(videoSizeOpt).split('x');
    if (wh.size() != 2 || wh[0].toInt() < 64 || wh[1].toInt() < 64) {
        std::fprintf(stderr, "--video-size must look like 1920x1080\n");
        return 1;
    }
    video.capture.device = cli.value(cameraOpt);
    video.capture.width = wh[0].toInt() & ~1;
    video.capture.height = wh[1].toInt() & ~1;
    video.capture.fps = std::clamp(cli.value(videoFpsOpt).toInt(), 1, 240);
    const QString cfmt = cli.value(cameraFormatOpt);
    video.capture.format = cfmt == "mjpeg" ? p2p::video::CaptureConfig::Format::Mjpeg
                         : cfmt == "nv12"  ? p2p::video::CaptureConfig::Format::Nv12
                                           : p2p::video::CaptureConfig::Format::Auto;
    video.capture.hwJpegDecode = cli.value(cameraDecodeOpt) != "software";
    p2p::video::VideoDecoder::setGpuFrames(cli.value(decodeOutputOpt) != "cpu");
    p2p::video::VideoEncoder::setNativeHevc(cli.value(encoderOpt) == "native");
    video.bitrateKbps = std::clamp(cli.value(videoBitrateOpt).toInt(), 100, 200000);
    const QString fec = cli.value(fecOpt);
    if (fec == "auto") video.fec = p2p::video::VideoStreamConfig::Fec::Auto;
    else if (fec == "off") video.fec = p2p::video::VideoStreamConfig::Fec::Off;
    else if (fec == "window") video.fec = p2p::video::VideoStreamConfig::Fec::Window;
    else if (fec == "hybrid") video.fec = p2p::video::VideoStreamConfig::Fec::Hybrid;
    else {
        bool ok = false;
        const double pct = fec.toDouble(&ok);
        if (!ok || pct <= 0 || pct > 100) {
            std::fprintf(stderr, "--fec must be auto, window, hybrid, off or a percentage\n");
            return 1;
        }
        video.fec = p2p::video::VideoStreamConfig::Fec::Fixed;
        video.fecFixedRate = pct / 100.0;
    }
    video.nack = !cli.isSet(noNackOpt);
    opts.mediaConfig.simulatedBurst = std::max(1.0, cli.value(simBurstOpt).toDouble());
    video.congestionControl = cli.value(ccOpt) != "off";
    video.minBitrateKbps = std::clamp(cli.value(minBitrateOpt).toInt(), 100, video.bitrateKbps);
    auto& sim = opts.mediaConfig.linkSim;
    sim.rateBps = std::max<int64_t>(0, cli.value(simRateOpt).toLongLong()) * 1000;
    sim.delayUs = int64_t(std::max(0.0, cli.value(simDelayOpt).toDouble()) * 1000);
    sim.queueUs = int64_t(std::max(1.0, cli.value(simQueueOpt).toDouble()) * 1000);
    sim.ceThresholdUs = int64_t(std::max(0.0, cli.value(simCeOpt).toDouble()) * 1000);
    video.flashLog = cli.value(flashLogOpt);

    // Screen share: same transport / recovery settings as the camera.
    auto& screen = opts.mediaConfig.screen;
    screen = video;
    screen.onSourceEnded = nullptr;
    screen.fallbackToPattern = false;
    screen.bitrateKbps = std::clamp(cli.value(screenBitrateOpt).toInt(), 300, 200000);
    const QString ssrc = cli.value(screenSourceOpt);
    if (ssrc == "portal") screen.source = VSource::Screen;
    else if (ssrc == "pattern") screen.source = VSource::Pattern; // capture.* gives its size and rate
    else if (ssrc == "none") screen.source = VSource::None;
    else {
        std::fprintf(stderr, "unknown --screen-source %s\n", qPrintable(ssrc));
        return 1;
    }
    // The portal's restore token lets later shares skip the picker.
    const QString cfgDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(cfgDir);
    screen.screen.restoreTokenFile = cfgDir + QStringLiteral("/screencast-token-%1").arg(instance);
    const QString shareMode = cli.value(shareScreenOpt);
    if (cli.isSet(shareScreenOpt) && shareMode != "text" && shareMode != "motion") {
        std::fprintf(stderr, "--share-screen must be text or motion\n");
        return 1;
    }

    CallController controller(opts);
    const QString av1 = cli.value(preferAv1Opt);
    controller.setPreferAv1Camera(av1 == "camera" || av1 == "both");
    controller.setPreferAv1Screen(av1 == "screen" || av1 == "both");
    if (!controller.start()) {
        std::fprintf(stderr, "%s\n", qPrintable(controller.lastError()));
        return 1;
    }

    // Machine-readable lines for scripted tests.
    QObject::connect(&controller, &CallController::callEstablished, &controller, [&] {
        std::printf("CALL_ESTABLISHED sas=\"%s\" send=\"%s\" recv=\"%s\"\n", qPrintable(controller.sas()),
                    qPrintable(controller.sendSummary()), qPrintable(controller.recvSummary()));
        std::fflush(stdout);
    });
    QObject::connect(&controller, &CallController::statsChanged, &controller, [&] {
        static int n = 0;
        if (headless && controller.rttMs() >= 0 && (++n % 5) == 1) {
            std::printf("STATS rtt_ms=%.3f offset_ms=%.3f\n", controller.rttMs(), controller.clockOffsetMs());
            std::fflush(stdout);
        }
    });

    if (headless) {
        QObject::connect(&controller, &CallController::mediaStatsChanged, &controller, [&] {
            static int n = 0;
            if (controller.mediaStats().isEmpty() || (++n % 2) != 0) return; // once per second
            std::printf("MEDIA %s\n", qPrintable(QString(controller.mediaStats()).replace('\n', QStringLiteral("\n      "))));
            std::fflush(stdout);
        });
    }

    if (cli.isSet(shareScreenOpt)) {
        QObject::connect(&controller, &CallController::callEstablished, &controller,
                         [&controller, shareMode] { controller.startScreenShare(shareMode); });
    }

    if (cli.isSet(exitAfterOpt)) {
        const int secs = cli.value(exitAfterOpt).toInt();
        QObject::connect(&controller, &CallController::callEstablished, &controller, [&controller, secs] {
            QTimer::singleShot(secs * 1000, &controller, [&controller] { controller.hangup(); });
        });
        QObject::connect(&controller, &CallController::callEnded, &controller, [&](const QString& reason) {
            std::printf("CALL_ENDED reason=\"%s\"\n", qPrintable(reason));
            std::fflush(stdout);
            const bool ok = reason == "local hangup" || reason == "remote hangup";
            QCoreApplication::exit(ok ? 0 : 2);
        });
    }

    if (cli.isSet(connectOpt)) {
        const QString target = cli.value(connectOpt);
        QTimer::singleShot(0, &controller, [&controller, target] { controller.callAddress(target); });
    }

    std::unique_ptr<QQmlApplicationEngine> engine;
    if (!headless) {
        engine = std::make_unique<QQmlApplicationEngine>();
        engine->rootContext()->setContextProperty("call", &controller);
        QObject::connect(engine.get(), &QQmlApplicationEngine::objectCreationFailed, app.get(),
                         [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
        engine->loadFromModule("P2PCall", "Main");
    }

    return app->exec();
}
