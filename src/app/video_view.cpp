#include "app/video_view.h"

#include <QFile>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

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
};
} // namespace

class VideoRenderer : public QQuickRhiItemRenderer {
public:
    void initialize(QRhiCommandBuffer*) override;
    void synchronize(QQuickRhiItem* item) override;
    void render(QRhiCommandBuffer* cb) override;

private:
    void ensureTextures(int w, int h);

    QRhi* rhi_ = nullptr;
    std::unique_ptr<QRhiBuffer> vbuf_, ubuf_;
    std::unique_ptr<QRhiSampler> sampler_;
    std::unique_ptr<QRhiTexture> yTex_, uvTex_;
    std::unique_ptr<QRhiShaderResourceBindings> srb_;
    std::unique_ptr<QRhiGraphicsPipeline> ps_;
    QRhiRenderPassDescriptor* rpDesc_ = nullptr;
    bool vbufUploaded_ = false;

    QString stream_;
    bool mirrored_ = false;
    uint64_t generation_ = 0;
    video::VideoFramePtr frame_;   // frame to show
    bool frameDirty_ = false;      // needs uploading
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

    srb_.reset(rhi_->newShaderResourceBindings());
    srb_->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage |
                                                        QRhiShaderResourceBinding::FragmentStage,
                                                 ubuf_.get()),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, yTex_.get(),
                                                  sampler_.get()),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, uvTex_.get(),
                                                  sampler_.get()),
    });
    srb_->create();
    ps_.reset();
}

void VideoRenderer::synchronize(QQuickRhiItem* item)
{
    auto* view = static_cast<VideoView*>(item);
    stream_ = view->stream();
    mirrored_ = view->mirrored();
    uint64_t gen = 0;
    auto f = video::FrameHub::instance().latest(stream_, &gen);
    if (gen != generation_) {
        generation_ = gen;
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

    ensureTextures(frame_->width, frame_->height);
    if (frameDirty_) {
        QRhiTextureSubresourceUploadDescription yDesc(frame_->y(), quint32(frame_->width * frame_->height));
        yDesc.setDataStride(quint32(frame_->width));
        QRhiTextureSubresourceUploadDescription uvDesc(frame_->uv(), quint32(frame_->width * frame_->height / 2));
        uvDesc.setDataStride(quint32(frame_->width));
        u->uploadTexture(yTex_.get(), QRhiTextureUploadDescription({0, 0, yDesc}));
        u->uploadTexture(uvTex_.get(), QRhiTextureUploadDescription({0, 0, uvDesc}));
        frameDirty_ = false;
    }

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
