#pragma once

#include "video/video_frame.h"

#include <QQuickRhiItem>
#include <QtQml/qqmlregistration.h>

namespace p2p::app {

// QML item that shows the newest frame from a FrameHub stream ("local" /
// "remote"). NV12 is uploaded as R8 + RG8 textures and converted to RGB in
// the fragment shader; nothing is converted on the CPU.
class VideoView : public QQuickRhiItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString stream READ stream WRITE setStream NOTIFY streamChanged)
    Q_PROPERTY(bool mirrored READ mirrored WRITE setMirrored NOTIFY mirroredChanged)
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY hasVideoChanged)
    Q_PROPERTY(QSize frameSize READ frameSize NOTIFY hasVideoChanged)

public:
    explicit VideoView(QQuickItem* parent = nullptr);

    QString stream() const { return stream_; }
    void setStream(const QString& s);
    bool mirrored() const { return mirrored_; }
    void setMirrored(bool m);
    bool hasVideo() const { return hasVideo_; }
    QSize frameSize() const { return frameSize_; }

signals:
    void streamChanged();
    void mirroredChanged();
    void hasVideoChanged();

protected:
    QQuickRhiItemRenderer* createRenderer() override;

private:
    friend class VideoRenderer;
    void onFrameAvailable(const QString& stream);

    QString stream_;
    bool mirrored_ = false;
    bool hasVideo_ = false;
    QSize frameSize_;
};

} // namespace p2p::app
