#include "app/video_view.h"

#include "core/clock.h"

#include <QQuickWindow>

#include <QFile>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <rhi/qshader.h>

#include <cstring>
#include <memory>

namespace p2p::app {

namespace {
QShader loadShader(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
}

// Full-screen quad as a triangle strip: x, y, u, v.
const float kQuad[] = {
    -1.f, -1.f, 0.f, 1.f, //
    1.f,  -1.f, 1.f, 1.f, //
    -1.f, 1.f,  0.f, 0.f, //
    1.f,  1.f,  1.f, 0.f, //
};

struct Uniforms {
    float mvp[16];
    float params[4]; // x: mirror
    float range[4];  // YUV -> RGB: x luma offset, y luma scale, z chroma scale
    float matrix[4]; // x Cr->R, y Cb->G, z Cr->G, w Cb->B
};

// Showing GPU frames without a copy (OpenGL on EGL): dma-buf -> EGLImage ->
// GL texture. The few EGL names needed (EGL_EXT_image_dma_buf_import,
// EGL_EXT_image_dma_buf_import_modifiers, GL_OES_EGL_image), from the specs.
using EGLint = int32_t;
using EGLenum = unsigned int;
using EGLBoolean = unsigned int;
using EGLImage = void*;
constexpr EGLint kEglNone = 0x3038, kEglWidth = 0x3057, kEglHeight = 0x3056;
constexpr EGLenum kEglLinuxDmaBuf = 0x3270;
constexpr EGLint kEglDrmFourcc = 0x3271, kEglPlane0Fd = 0x3272, kEglPlane0Offset = 0x3273,
                 kEglPlane0Pitch = 0x3274, kEglPlane0ModLo = 0x3443, kEglPlane0ModHi = 0x3444;
constexpr uint64_t kDrmModInvalid = 0x00ffffffffffffffULL;
using CreateImageFn = EGLImage (*)(EGLDisplay, EGLContext, EGLenum, void*, const EGLint*);
using DestroyImageFn = EGLBoolean (*)(EGLDisplay, EGLImage);
using ImageTargetTextureFn = void (*)(GLenum, void*);

class DmaBufImporter {
public:
    // From the renderer's GL context (current); false if not EGL / no extensions.
    bool init(QOpenGLContext* ctx)
    {
        auto* egl = ctx ? ctx->nativeInterface<QNativeInterface::QEGLContext>() : nullptr;
        if (!egl) return false;
        display_ = egl->display();
        create_ = reinterpret_cast<CreateImageFn>(ctx->getProcAddress("eglCreateImageKHR"));
        destroy_ = reinterpret_cast<DestroyImageFn>(ctx->getProcAddress("eglDestroyImageKHR"));
        target_ = reinterpret_cast<ImageTargetTextureFn>(ctx->getProcAddress("glEGLImageTargetTexture2DOES"));
        gl_ = ctx->functions();
        getError_ = reinterpret_cast<EGLint (*)()>(ctx->getProcAddress("eglGetError"));
        return display_ && create_ && destroy_ && target_ && gl_;
    }
    // (Re)points tex[0] / tex[1] at the frame's Y / UV planes.
    bool import(const video::DmaBufNv12& d, GLuint tex[2])
    {
        if (!tex[0]) gl_->glGenTextures(2, tex);
        const video::DmaBufPlane* planes[2] = {&d.y, &d.uv};
        for (int i = 0; i < 2; ++i) {
            const auto& p = *planes[i];
            const int div = i ? 2 : 1;
            std::vector<EGLint> a = {kEglWidth,        d.width / div,    kEglHeight,      d.height / div,
                                     kEglDrmFourcc,    EGLint(p.drmFormat), kEglPlane0Fd,  p.fd,
                                     kEglPlane0Offset, EGLint(p.offset), kEglPlane0Pitch, EGLint(p.pitch)};
            if (p.modifier != kDrmModInvalid) {
                a.insert(a.end(), {kEglPlane0ModLo, EGLint(p.modifier & 0xffffffff), kEglPlane0ModHi,
                                   EGLint(p.modifier >> 32)});
            }
            a.push_back(kEglNone);
            EGLImage img = create_(display_, nullptr, kEglLinuxDmaBuf, nullptr, a.data());
            if (!img) {
                error_ = QStringLiteral("eglCreateImage plane %1 (fourcc %2, modifier %3): EGL error 0x%4")
                             .arg(i)
                             .arg(p.drmFormat, 0, 16)
                             .arg(p.modifier, 0, 16)
                             .arg(getError_ ? getError_() : 0, 0, 16);
                return false;
            }
            while (gl_->glGetError() != GL_NO_ERROR) {} // not ours
            gl_->glBindTexture(GL_TEXTURE_2D, tex[i]);
            target_(GL_TEXTURE_2D, img);
            const GLenum err = gl_->glGetError();
            const bool ok = err == GL_NO_ERROR;
            if (!ok) error_ = QStringLiteral("glEGLImageTargetTexture2DOES plane %1: GL error 0x%2").arg(i).arg(err, 0, 16);
            gl_->glBindTexture(GL_TEXTURE_2D, 0);
            destroy_(display_, img); // the texture keeps the buffer
            if (!ok) return false;
        }
        return true;
    }
    QString error() const { return error_; }
    void release(GLuint tex[2])
    {
        if (gl_ && tex[0]) gl_->glDeleteTextures(2, tex);
        tex[0] = tex[1] = 0;
    }

private:
    EGLDisplay display_ = nullptr;
    CreateImageFn create_ = nullptr;
    DestroyImageFn destroy_ = nullptr;
    ImageTargetTextureFn target_ = nullptr;
    QOpenGLFunctions* gl_ = nullptr;
    EGLint (*getError_)() = nullptr;
    QString error_;
};

void setColour(Uniforms& u, video::Colour c)
{
    if (c == video::Colour::Bt601Full) {
        const float r[4] = {0.f, 1.f, 1.f, 0.f}, m[4] = {1.402f, 0.344136f, 0.714136f, 1.772f};
        std::memcpy(u.range, r, sizeof r);
        std::memcpy(u.matrix, m, sizeof m);
    } else { // BT.709 limited
        const float r[4] = {16.f / 255.f, 255.f / 219.f, 255.f / 224.f, 0.f}, m[4] = {1.5748f, 0.1873f, 0.4681f, 1.8556f};
        std::memcpy(u.range, r, sizeof r);
        std::memcpy(u.matrix, m, sizeof m);
    }
}
} // namespace

class VideoRenderer : public QQuickRhiItemRenderer {
public:
    ~VideoRenderer() override
    {
        QObject::disconnect(swapConn_);
        if (importOk_ && rhi_ && rhi_->makeThreadLocalNativeContextCurrent()) importer_.release(glTex_);
    }
    void initialize(QRhiCommandBuffer*) override;
    void synchronize(QQuickRhiItem* item) override;
    void render(QRhiCommandBuffer* cb) override;

private:
    void ensureTextures(int w, int h);
    bool importFrame(); // frame_ straight from its dma-bufs; false: upload instead
    void bindTextures(QRhiTexture* y, QRhiTexture* uv);

    QRhi* rhi_ = nullptr;
    std::unique_ptr<QRhiBuffer> vbuf_, ubuf_;
    std::unique_ptr<QRhiSampler> sampler_;
    std::unique_ptr<QRhiTexture> yTex_, uvTex_;    // uploaded frames
    std::unique_ptr<QRhiTexture> extY_, extUV_;    // imported (zero-copy) frames, wrapping glTex_
    GLuint glTex_[2] = {0, 0};
    DmaBufImporter importer_;
    bool importTried_ = false, importOk_ = false;
    bool showingImported_ = false;
    QRhiTexture* boundY_ = nullptr;
    video::VideoFramePtr prevFrame_; // its buffers may still be read by the GPU
    std::unique_ptr<QRhiShaderResourceBindings> srb_;
    std::unique_ptr<QRhiGraphicsPipeline> ps_;
    QRhiRenderPassDescriptor* rpDesc_ = nullptr;
    bool vbufUploaded_ = false;

    QString stream_;
    bool mirrored_ = false;
    uint64_t generation_ = 0;
    video::VideoFramePtr frame_;   // frame to show
    bool frameDirty_ = false;      // needs uploading
    // Latency breakdown: the generation drawn in this frame, reported to
    // the hub once the window's swap returns (render thread).
    QMetaObject::Connection swapConn_;
    uint64_t drawnGeneration_ = 0;
};

// ---------------------------------------------------------------- renderer

void VideoRenderer::initialize(QRhiCommandBuffer*)
{
    if (rhi_ != rhi()) {
        rhi_ = rhi();
        ps_.reset();
        srb_.reset();
        yTex_.reset();
        uvTex_.reset();
        extY_.reset();
        extUV_.reset();
        glTex_[0] = glTex_[1] = 0; // went with the old context
        importTried_ = importOk_ = showingImported_ = false;
        boundY_ = nullptr;
        vbufUploaded_ = false;
        vbuf_.reset(rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuad)));
        vbuf_->create();
        ubuf_.reset(rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Uniforms)));
        ubuf_->create();
        sampler_.reset(rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                        QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        sampler_->create();
        frameDirty_ = frame_ != nullptr;
    }
    if (rpDesc_ != renderTarget()->renderPassDescriptor()) {
        rpDesc_ = renderTarget()->renderPassDescriptor();
        ps_.reset(); // recreated lazily against the new render pass
    }
}

void VideoRenderer::ensureTextures(int w, int h)
{
    if (yTex_ && yTex_->pixelSize() == QSize(w, h)) return;
    yTex_.reset(rhi_->newTexture(QRhiTexture::R8, QSize(w, h)));
    uvTex_.reset(rhi_->newTexture(QRhiTexture::RG8, QSize(w / 2, h / 2)));
    yTex_->create();
    uvTex_->create();
    boundY_ = nullptr;
}

bool VideoRenderer::importFrame()
{
    if (!importTried_) {
        importTried_ = true;
        if (rhi_->backend() == QRhi::OpenGLES2) {
            auto* h = static_cast<const QRhiGles2NativeHandles*>(rhi_->nativeHandles());
            importOk_ = h && rhi_->makeThreadLocalNativeContextCurrent() && importer_.init(h->context);
        }
        qInfo("video view: %s", importOk_ ? "GPU frames shown without a copy (dma-buf -> EGLImage)"
                                          : "GPU frames are read back and uploaded (no EGL dma-buf import)");
    }
    if (!importOk_ || !frame_->gpu) return false;
    const video::DmaBufNv12* d = frame_->gpu->dmaBuf();
    if (!d || !rhi_->makeThreadLocalNativeContextCurrent() || !importer_.import(*d, glTex_)) {
        static bool warned = false;
        if (!warned)
            qWarning("video view: dma-buf import failed (%s); uploading instead",
                     d ? qPrintable(importer_.error()) : "export");
        warned = true;
        return false;
    }
    const QSize size(frame_->width, frame_->height);
    if (!extY_ || extY_->pixelSize() != size) {
        extY_.reset(rhi_->newTexture(QRhiTexture::R8, size));
        extUV_.reset(rhi_->newTexture(QRhiTexture::RG8, size / 2));
        if (!extY_->createFrom({glTex_[0], 0}) || !extUV_->createFrom({glTex_[1], 0})) {
            extY_.reset();
            extUV_.reset();
            return false;
        }
        boundY_ = nullptr;
    }
    return true;
}

void VideoRenderer::bindTextures(QRhiTexture* y, QRhiTexture* uv)
{
    if (boundY_ == y && srb_) return;
    boundY_ = y;
    srb_.reset(rhi_->newShaderResourceBindings());
    srb_->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage |
                                                        QRhiShaderResourceBinding::FragmentStage,
                                                 ubuf_.get()),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, y, sampler_.get()),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, uv, sampler_.get()),
    });
    srb_->create();
    ps_.reset();
}

void VideoRenderer::synchronize(QQuickRhiItem* item)
{
    auto* view = static_cast<VideoView*>(item);
    if (!swapConn_ && item->window()) {
        swapConn_ = QObject::connect(item->window(), &QQuickWindow::frameSwapped, [this] {
            if (drawnGeneration_ == 0) return;
            video::FrameHub::instance().noteShown(stream_, drawnGeneration_, nowUs());
            drawnGeneration_ = 0;
        });
    }
    stream_ = view->stream();
    mirrored_ = view->mirrored();
    uint64_t gen = 0;
    auto f = video::FrameHub::instance().latest(stream_, &gen);
    if (gen != generation_) {
        generation_ = gen;
        if (frameDirty_ == false && frame_) prevFrame_ = frame_; // shown last; GPU may still read it
        frame_ = std::move(f);
        frameDirty_ = frame_ != nullptr;
    }
}

void VideoRenderer::render(QRhiCommandBuffer* cb)
{
    QRhiResourceUpdateBatch* u = rhi_->nextResourceUpdateBatch();
    if (!vbufUploaded_) {
        u->uploadStaticBuffer(vbuf_.get(), kQuad);
        vbufUploaded_ = true;
    }

    const QSize out = renderTarget()->pixelSize();
    if (!frame_) {
        cb->beginPass(renderTarget(), QColor(0, 0, 0), {1.0f, 0}, u);
        cb->endPass();
        return;
    }

    if (frameDirty_) {
        // A GPU frame is shown straight from its buffers; otherwise (or if
        // that fails) uploaded, downloading a GPU frame first.
        showingImported_ = importFrame();
        if (showingImported_) {
            frameDirty_ = false;
            drawnGeneration_ = generation_;
        } else if (!frame_->ensureCpu()) {
            frameDirty_ = false;
        }
    }
    if (!showingImported_) ensureTextures(frame_->width, frame_->height);
    if (frameDirty_) {
        QRhiTextureSubresourceUploadDescription yDesc(frame_->y(), quint32(frame_->width * frame_->height));
        yDesc.setDataStride(quint32(frame_->width));
        QRhiTextureSubresourceUploadDescription uvDesc(frame_->uv(), quint32(frame_->width * frame_->height / 2));
        uvDesc.setDataStride(quint32(frame_->width));
        u->uploadTexture(yTex_.get(), QRhiTextureUploadDescription({0, 0, yDesc}));
        u->uploadTexture(uvTex_.get(), QRhiTextureUploadDescription({0, 0, uvDesc}));
        frameDirty_ = false;
        drawnGeneration_ = generation_;
    }
    if (showingImported_) bindTextures(extY_.get(), extUV_.get());
    else bindTextures(yTex_.get(), uvTex_.get());

    if (!ps_) {
        ps_.reset(rhi_->newGraphicsPipeline());
        ps_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        ps_->setShaderStages({
            {QRhiShaderStage::Vertex, loadShader(QStringLiteral(":/shaders/nv12.vert.qsb"))},
            {QRhiShaderStage::Fragment, loadShader(QStringLiteral(":/shaders/nv12.frag.qsb"))},
        });
        QRhiVertexInputLayout layout;
        layout.setBindings({{4 * sizeof(float)}});
        layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float4, 0}});
        ps_->setVertexInputLayout(layout);
        ps_->setShaderResourceBindings(srb_.get());
        ps_->setRenderPassDescriptor(rpDesc_);
        ps_->create();
    }

    // Aspect-fit the frame inside the item.
    const float itemAspect = float(out.width()) / float(std::max(1, out.height()));
    const float frameAspect = float(frame_->width) / float(std::max(1, frame_->height));
    float sx = 1.f, sy = 1.f;
    if (frameAspect > itemAspect) sy = itemAspect / frameAspect;
    else sx = frameAspect / itemAspect;
    QMatrix4x4 mvp = rhi_->clipSpaceCorrMatrix();
    mvp.scale(sx, sy);
    Uniforms uni{};
    std::memcpy(uni.mvp, mvp.constData(), sizeof(uni.mvp));
    uni.params[0] = mirrored_ ? 1.f : 0.f;
    setColour(uni, frame_->colour);
    u->updateDynamicBuffer(ubuf_.get(), 0, sizeof(uni), &uni);

    cb->beginPass(renderTarget(), QColor(0, 0, 0), {1.0f, 0}, u);
    cb->setGraphicsPipeline(ps_.get());
    cb->setViewport({0, 0, float(out.width()), float(out.height())});
    cb->setShaderResources();
    const QRhiCommandBuffer::VertexInput vin(vbuf_.get(), 0);
    cb->setVertexInput(0, 1, &vin);
    cb->draw(4);
    cb->endPass();
}

// ---------------------------------------------------------------- item

VideoView::VideoView(QQuickItem* parent) : QQuickRhiItem(parent)
{
    connect(&video::FrameHub::instance(), &video::FrameHub::frameAvailable, this, &VideoView::onFrameAvailable,
            Qt::QueuedConnection);
}

QQuickRhiItemRenderer* VideoView::createRenderer()
{
    return new VideoRenderer;
}

void VideoView::setStream(const QString& s)
{
    if (stream_ == s) return;
    stream_ = s;
    emit streamChanged();
    update();
}

void VideoView::setMirrored(bool m)
{
    if (mirrored_ == m) return;
    mirrored_ = m;
    emit mirroredChanged();
    update();
}

void VideoView::onFrameAvailable(const QString& stream)
{
    if (stream != stream_) return;
    auto f = video::FrameHub::instance().latest(stream_);
    const bool has = f != nullptr;
    const QSize size = has ? QSize(f->width, f->height) : QSize();
    if (has != hasVideo_ || size != frameSize_) {
        hasVideo_ = has;
        frameSize_ = size;
        emit hasVideoChanged();
    }
    update();
}

} // namespace p2p::app
