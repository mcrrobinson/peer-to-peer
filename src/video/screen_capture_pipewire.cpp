// Screen capture on Linux: xdg-desktop-portal ScreenCast (D-Bus) hands us a
// PipeWire remote and a node; a PipeWire stream delivers BGRx/RGBx frames in
// shared memory, which are converted to NV12 (BT.709 limited) with swscale.
//
// Threads: the portal conversation runs on the Qt thread that called
// start(); PipeWire callbacks (convert) run on the pw_thread_loop; the
// frame callback (encode) runs on our worker thread, which takes only the
// newest frame and also repeats the last one when the screen is idle.
//
// DMA-BUF import (zero-copy into the encoder) is phase 9 work; for now we
// don't offer DMA-BUF modifiers, so the compositor sends shared memory.
#include "core/clock.h"
#include "video/video_source.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QFile>
#include <QLoggingCategory>
#include <QPointer>
#include <QRandomGenerator>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/debug/types.h>
#include <spa/param/video/type-info.h>

extern "C" {
#include <libswscale/swscale.h>
}

#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

Q_LOGGING_CATEGORY(lcScreen, "p2p.screen")

namespace p2p::video {

namespace {
constexpr char kPortalService[] = "org.freedesktop.portal.Desktop";
constexpr char kPortalPath[] = "/org/freedesktop/portal/desktop";
constexpr char kScreenCastIface[] = "org.freedesktop.portal.ScreenCast";
constexpr char kRequestIface[] = "org.freedesktop.portal.Request";
constexpr char kSessionIface[] = "org.freedesktop.portal.Session";
constexpr int64_t kIdleRepeatUs = 1'000'000;

void pipewireInitOnce()
{
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

AVPixelFormat avFormat(uint32_t spa)
{
    switch (spa) {
    case SPA_VIDEO_FORMAT_BGRx: return AV_PIX_FMT_BGR0;
    case SPA_VIDEO_FORMAT_BGRA: return AV_PIX_FMT_BGRA;
    case SPA_VIDEO_FORMAT_RGBx: return AV_PIX_FMT_RGB0;
    case SPA_VIDEO_FORMAT_RGBA: return AV_PIX_FMT_RGBA;
    default: return AV_PIX_FMT_NONE;
    }
}
} // namespace

// ------------------------------------------------------------------ portal

// One ScreenCast session: CreateSession -> SelectSources -> Start ->
// OpenPipeWireRemote. Each step answers through a Request object's
// Response signal.
class ScreenCastPortal : public QObject {
    Q_OBJECT
public:
    using Ready = std::function<void(int fd, uint32_t node, int w, int h)>;
    using Failed = std::function<void(QString)>;

    ScreenCastPortal(ScreenConfig cfg, Ready ready, Failed failed)
        : cfg_(std::move(cfg)), ready_(std::move(ready)), failed_(std::move(failed)),
          bus_(QDBusConnection::sessionBus())
    {
    }
    ~ScreenCastPortal() override { close(); }

    void begin()
    {
        if (!bus_.isConnected()) return fail(QStringLiteral("no D-Bus session bus"));
        const QString token = newToken();
        call(QStringLiteral("CreateSession"),
             {QVariantMap{{"handle_token", token}, {"session_handle_token", newToken()}}}, token,
             &ScreenCastPortal::onCreated);
    }

    void close()
    {
        if (session_.isEmpty()) return;
        QDBusMessage m = QDBusMessage::createMethodCall(kPortalService, session_, kSessionIface, "Close");
        bus_.call(m, QDBus::NoBlock);
        bus_.disconnect(kPortalService, session_, kSessionIface, "Closed", this, SLOT(onSessionClosed()));
        session_.clear();
    }

private slots:
    void onResponse(uint response, const QVariantMap& results)
    {
        auto step = pending_;
        pending_ = nullptr;
        if (requestPath_.size())
            bus_.disconnect(kPortalService, requestPath_, kRequestIface, "Response", this,
                            SLOT(onResponse(uint, QVariantMap)));
        if (response != 0) {
            fail(response == 1 ? QStringLiteral("screen sharing cancelled")
                               : QStringLiteral("screen sharing refused by the portal (%1)").arg(response));
            return;
        }
        if (step) (this->*step)(results);
    }
    void onSessionClosed()
    {
        session_.clear();
        fail(QStringLiteral("screen sharing stopped from the desktop"));
    }

private:
    using Step = void (ScreenCastPortal::*)(const QVariantMap&);

    QString newToken() { return QStringLiteral("p2pcall%1").arg(QRandomGenerator::global()->generate()); }

    // Subscribes to the predicted Request path before calling, so the
    // Response can't race us.
    void call(const QString& method, QVariantList args, const QString& token, Step next)
    {
        QString sender = bus_.baseService().mid(1);
        sender.replace('.', '_');
        requestPath_ = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
        bus_.connect(kPortalService, requestPath_, kRequestIface, "Response", this,
                     SLOT(onResponse(uint, QVariantMap)));
        pending_ = next;
        QDBusMessage m = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kScreenCastIface, method);
        m.setArguments(args);
        auto* w = new QDBusPendingCallWatcher(bus_.asyncCall(m), this);
        connect(w, &QDBusPendingCallWatcher::finished, this, [this, method](QDBusPendingCallWatcher* w) {
            w->deleteLater();
            if (w->isError()) fail(QStringLiteral("portal %1 failed: %2").arg(method, w->error().message()));
        });
    }

    void onCreated(const QVariantMap& r)
    {
        session_ = r.value("session_handle").toString();
        if (session_.isEmpty()) return fail(QStringLiteral("portal gave no session"));
        bus_.connect(kPortalService, session_, kSessionIface, "Closed", this, SLOT(onSessionClosed()));

        QDBusInterface sc(kPortalService, kPortalPath, kScreenCastIface, bus_);
        const uint cursorModes = sc.property("AvailableCursorModes").toUInt();
        const QString token = newToken();
        QVariantMap opt{{"handle_token", token},
                        {"types", uint(1 | 2)}, // monitor | window
                        {"multiple", false},
                        {"cursor_mode", uint((cursorModes & 2) ? 2 : 1)}}; // embedded, else hidden
        if (!cfg_.restoreTokenFile.isEmpty()) {
            opt.insert("persist_mode", uint(2)); // until revoked
            QFile f(cfg_.restoreTokenFile);
            if (f.open(QIODevice::ReadOnly)) {
                const QString t = QString::fromUtf8(f.readAll()).trimmed();
                if (!t.isEmpty()) opt.insert("restore_token", t);
            }
        }
        call(QStringLiteral("SelectSources"), {QVariant::fromValue(QDBusObjectPath(session_)), opt}, token,
             &ScreenCastPortal::onSelected);
    }

    void onSelected(const QVariantMap&)
    {
        const QString token = newToken();
        call(QStringLiteral("Start"), {QVariant::fromValue(QDBusObjectPath(session_)), QString(),
                                       QVariantMap{{"handle_token", token}}},
             token, &ScreenCastPortal::onStarted);
    }

    void onStarted(const QVariantMap& r)
    {
        if (!cfg_.restoreTokenFile.isEmpty() && r.contains("restore_token")) {
            QFile f(cfg_.restoreTokenFile);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(r.value("restore_token").toString().toUtf8());
        }
        // streams: a(ua{sv}) -> first node id (and its size, when given).
        uint32_t node = 0;
        int w = 0, h = 0;
        const QDBusArgument arg = r.value("streams").value<QDBusArgument>();
        arg.beginArray();
        if (!arg.atEnd()) {
            arg.beginStructure();
            QVariantMap props;
            arg >> node >> props;
            arg.endStructure();
            const QDBusArgument size = props.value("size").value<QDBusArgument>();
            if (size.currentType() == QDBusArgument::StructureType) {
                size.beginStructure();
                size >> w >> h;
                size.endStructure();
            }
        }
        while (!arg.atEnd()) { // only one source is shared; read past the rest
            uint32_t other = 0;
            QVariantMap props;
            arg.beginStructure();
            arg >> other >> props;
            arg.endStructure();
        }
        arg.endArray();
        if (node == 0) return fail(QStringLiteral("portal returned no stream"));

        QDBusMessage m = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kScreenCastIface,
                                                        "OpenPipeWireRemote");
        m.setArguments({QVariant::fromValue(QDBusObjectPath(session_)), QVariantMap{}});
        auto* watcher = new QDBusPendingCallWatcher(bus_.asyncCall(m), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, node, w, h](QDBusPendingCallWatcher* wt) {
            wt->deleteLater();
            QDBusPendingReply<QDBusUnixFileDescriptor> reply = *wt;
            if (reply.isError()) return fail(QStringLiteral("OpenPipeWireRemote: %1").arg(reply.error().message()));
            const int fd = ::dup(reply.value().fileDescriptor());
            if (fd < 0) return fail(QStringLiteral("bad PipeWire fd"));
            qCInfo(lcScreen) << "portal: sharing node" << node << w << "x" << h;
            ready_(fd, node, w, h);
        });
    }

    void fail(const QString& why)
    {
        qCWarning(lcScreen).noquote() << why;
        close();
        if (failed_) failed_(why);
    }

    ScreenConfig cfg_;
    Ready ready_;
    Failed failed_;
    QDBusConnection bus_;
    QString session_, requestPath_;
    Step pending_ = nullptr;
};

// ------------------------------------------------------------------ source

class PipeWireScreenSource : public VideoSource {
public:
    PipeWireScreenSource(ScreenConfig cfg, std::function<void(QString)> onEnded)
        : cfg_(std::move(cfg)), onEnded_(std::move(onEnded))
    {
    }
    ~PipeWireScreenSource() override { stop(); }

    bool start(Callback cb, QString* error) override
    {
        if (running_) return true;
        pipewireInitOnce();
        cb_ = std::move(cb);
        running_ = true;
        ended_ = false;
        worker_ = std::thread([this] { workerLoop(); });
        portal_ = new ScreenCastPortal(
            cfg_, [this](int fd, uint32_t node, int, int) { openStream(fd, node); },
            [this](const QString& why) { end(why); });
        portal_->begin();
        (void)error;
        return true;
    }

    void stop() override
    {
        if (!running_) return;
        running_ = false;
        closeStream();
        if (portal_) {
            portal_->close();
            delete portal_.data();
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        sws_freeContext(sws_);
        sws_ = nullptr;
        std::lock_guard l(m_);
        latest_.reset();
        last_.reset();
    }

    QString description() const override
    {
        std::lock_guard l(m_);
        if (width_ == 0) return QStringLiteral("screen (waiting for the portal)");
        return QStringLiteral("screen %1x%2 %3 (PipeWire, up to %4 fps)")
            .arg(width_)
            .arg(height_)
            .arg(cfg_.mode == ScreenConfig::Mode::Text ? "text" : "motion")
            .arg(cfg_.maxFps);
    }
    int fps() const override { return cfg_.maxFps; }

    void requestFrame() override
    {
        std::lock_guard l(m_);
        repeatNow_ = true;
        cv_.notify_one();
    }

private:
    void end(const QString& why)
    {
        if (ended_.exchange(true)) return;
        if (onEnded_) onEnded_(why);
    }

    // --------------------------------------------------------- PipeWire
    void openStream(int fd, uint32_t node)
    {
        loop_ = pw_thread_loop_new("p2p-screen", nullptr);
        if (!loop_ || pw_thread_loop_start(loop_) < 0) {
            ::close(fd);
            return end(QStringLiteral("PipeWire loop failed"));
        }
        pw_thread_loop_lock(loop_);
        context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
        core_ = context_ ? pw_context_connect_fd(context_, fd, nullptr, 0) : nullptr;
        if (!core_) {
            pw_thread_loop_unlock(loop_);
            return end(QStringLiteral("could not connect to PipeWire"));
        }
        stream_ = pw_stream_new(core_, "p2pcall-screen",
                                pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                                  PW_KEY_MEDIA_ROLE, "Screen", nullptr));
        static const pw_stream_events events = [] {
            pw_stream_events e{};
            e.version = PW_VERSION_STREAM_EVENTS;
            e.state_changed = [](void* d, pw_stream_state, pw_stream_state s, const char* err) {
                static_cast<PipeWireScreenSource*>(d)->onState(s, err);
            };
            e.param_changed = [](void* d, uint32_t id, const spa_pod* p) {
                static_cast<PipeWireScreenSource*>(d)->onParam(id, p);
            };
            e.process = [](void* d) { static_cast<PipeWireScreenSource*>(d)->onProcess(); };
            return e;
        }();
        pw_stream_add_listener(stream_, &listener_, &events, this);

        uint8_t buf[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        spa_rectangle defSize{1920, 1080}, minSize{1, 1}, maxSize{8192, 8192};
        spa_fraction defRate{uint32_t(cfg_.maxFps), 1}, minRate{0, 1}, maxRate{uint32_t(cfg_.maxFps), 1};
        const spa_pod* params[1];
        params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
            SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
                                   SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
            SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&defSize, &minSize, &maxSize),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&defRate, &minRate, &maxRate)));
        const int r = pw_stream_connect(stream_, PW_DIRECTION_INPUT, node,
                                        pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                                        params, 1);
        pw_thread_loop_unlock(loop_);
        if (r < 0) end(QStringLiteral("PipeWire stream connect failed"));
    }

    void closeStream()
    {
        if (!loop_) return;
        pw_thread_loop_lock(loop_);
        if (stream_) {
            spa_hook_remove(&listener_);
            pw_stream_disconnect(stream_);
            pw_stream_destroy(stream_);
            stream_ = nullptr;
        }
        if (core_) pw_core_disconnect(core_);
        core_ = nullptr;
        pw_thread_loop_unlock(loop_);
        pw_thread_loop_stop(loop_);
        if (context_) pw_context_destroy(context_);
        context_ = nullptr;
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }

    void onState(pw_stream_state s, const char* err)
    {
        if (s == PW_STREAM_STATE_ERROR) end(QStringLiteral("PipeWire stream error: %1").arg(err ? err : "?"));
        else if (s == PW_STREAM_STATE_UNCONNECTED && running_) end(QStringLiteral("PipeWire stream closed"));
    }

    void onParam(uint32_t id, const spa_pod* param)
    {
        if (id != SPA_PARAM_Format || !param) return;
        spa_video_info_raw info{};
        uint32_t mediaType = 0, mediaSubtype = 0;
        if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0 || mediaType != SPA_MEDIA_TYPE_video ||
            mediaSubtype != SPA_MEDIA_SUBTYPE_raw || spa_format_video_raw_parse(param, &info) < 0)
            return;
        format_ = info.format;
        srcW_ = int(info.size.width);
        srcH_ = int(info.size.height);
        // Shared memory only (see the top of the file), plus the header meta
        // for the corrupted-frame flag.
        uint8_t buf[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        const spa_pod* params[2];
        params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
            SPA_POD_CHOICE_RANGE_Int(8, 2, 16), SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd))));
        params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
            SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header))));
        pw_stream_update_params(stream_, params, 2);
        qCInfo(lcScreen) << "PipeWire format" << spa_debug_type_find_short_name(spa_type_video_format, format_)
                         << srcW_ << "x" << srcH_;
    }

    void onProcess()
    {
        pw_buffer* b = nullptr;
        while (pw_buffer* n = pw_stream_dequeue_buffer(stream_)) { // newest only
            if (b) pw_stream_queue_buffer(stream_, b);
            b = n;
        }
        if (!b) return;
        convert(b->buffer);
        pw_stream_queue_buffer(stream_, b);
    }

    void convert(spa_buffer* sb)
    {
        const int64_t now = nowUs();
        if (now - lastConvertUs_ < 1'000'000 / std::max(1, cfg_.maxFps) - 2'000) return; // rate cap
        auto* h = static_cast<spa_meta_header*>(spa_buffer_find_meta_data(sb, SPA_META_Header, sizeof(spa_meta_header)));
        if (h && (h->flags & SPA_META_HEADER_FLAG_CORRUPTED)) return;
        const spa_data& d = sb->datas[0];
        if (!d.data || !d.chunk || d.chunk->size == 0 || srcW_ <= 0 || srcH_ <= 0) return; // e.g. cursor-only update
        const AVPixelFormat fmt = avFormat(format_);
        if (fmt == AV_PIX_FMT_NONE) return;

        // Even size, scaled down to fit the encoder's limit.
        const double scale = std::min({1.0, double(cfg_.maxWidth) / srcW_, double(cfg_.maxHeight) / srcH_});
        const int w = std::max(16, int(srcW_ * scale) & ~1), hgt = std::max(16, int(srcH_ * scale) & ~1);
        sws_ = sws_getCachedContext(sws_, srcW_, srcH_, fmt, w, hgt, AV_PIX_FMT_NV12,
                                    scale < 1.0 ? SWS_BICUBIC : SWS_POINT, nullptr, nullptr, nullptr);
        if (!sws_) return;
        if (sws_ != lastSws_) {
            // Full-range RGB in, BT.709 limited range out.
            sws_setColorspaceDetails(sws_, sws_getCoefficients(SWS_CS_ITU709), 1, sws_getCoefficients(SWS_CS_ITU709), 0,
                                     0, 1 << 16, 1 << 16);
            lastSws_ = sws_;
        }
        auto f = std::make_shared<VideoFrame>();
        f->allocate(w, hgt);
        f->captureUs = now;
        f->id = frameNo_++;
        const uint8_t* src[1] = {static_cast<const uint8_t*>(d.data) + d.chunk->offset};
        const int srcStride[1] = {d.chunk->stride ? int(d.chunk->stride) : srcW_ * 4};
        uint8_t* dst[2] = {f->y(), f->uv()};
        const int dstStride[2] = {w, w};
        sws_scale(sws_, src, srcStride, 0, srcH_, dst, dstStride);
        lastConvertUs_ = now;
        {
            std::lock_guard l(m_);
            latest_ = std::move(f);
            width_ = w;
            height_ = hgt;
        }
        cv_.notify_one();
    }

    // ----------------------------------------------------------- worker
    void workerLoop()
    {
        while (running_) {
            std::shared_ptr<VideoFrame> f;
            {
                std::unique_lock l(m_);
                cv_.wait_for(l, std::chrono::milliseconds(100), [&] { return latest_ || repeatNow_ || !running_; });
                if (!running_) break;
                if (latest_) {
                    f = std::move(latest_);
                    last_ = f;
                    repeatNow_ = false;
                    lastDeliveredUs_ = nowUs();
                } else if (last_ && (repeatNow_ || nowUs() - lastDeliveredUs_ >= kIdleRepeatUs)) {
                    // Idle screen: show the same picture again (lets the encoder
                    // refine it and answers keyframe requests).
                    f = std::make_shared<VideoFrame>(*last_);
                    f->captureUs = nowUs();
                    f->id = frameNo_++;
                    repeatNow_ = false;
                    lastDeliveredUs_ = f->captureUs;
                }
            }
            if (f && cb_) cb_(std::move(f));
        }
    }

    ScreenConfig cfg_;
    std::function<void(QString)> onEnded_;
    Callback cb_;
    std::atomic<bool> running_{false}, ended_{false};
    QPointer<ScreenCastPortal> portal_;

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;
    spa_hook listener_{};
    uint32_t format_ = 0;
    int srcW_ = 0, srcH_ = 0;
    SwsContext* sws_ = nullptr;
    SwsContext* lastSws_ = nullptr;
    int64_t lastConvertUs_ = 0;
    std::atomic<uint32_t> frameNo_{0};

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::shared_ptr<VideoFrame> latest_, last_;
    bool repeatNow_ = false;
    int64_t lastDeliveredUs_ = 0;
    int width_ = 0, height_ = 0;
    std::thread worker_;
};

std::unique_ptr<VideoSource> createScreenSource(const ScreenConfig& cfg, std::function<void(QString)> onEnded)
{
    return std::make_unique<PipeWireScreenSource>(cfg, std::move(onEnded));
}

} // namespace p2p::video

#include "screen_capture_pipewire.moc"
