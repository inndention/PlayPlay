#ifndef MIXER_H
#define MIXER_H

#include <QObject>
#include <QImage>
#include <QMap>
#include <QMutex>
#include <QPoint>
#include "mediaframe.h"

/**
 * Mixer — per-channel layer compositor.
 *
 * Layers (bottom to top):
 *   base    : decoded program video (BGRA FramePtr), or the slate when there is
 *             no video, or black.
 *   overlays: CG layers (lower thirds, bugs, tickers) as premultiplied ARGB
 *             QImages with position, opacity and z-order (= layerId).
 *
 * Fast paths:
 *   - no visible overlay  -> the base frame is returned by reference (no copy).
 *   - overlays present    -> base is copied into a pooled frame and overlays
 *                            are drawn with QPainter (SIMD raster backend).
 *
 * All methods are thread-safe; composite() is called on the frame-clock thread,
 * overlay updates come from the IPC/CG threads.
 */
class Mixer : public QObject {
    Q_OBJECT

public:
    struct Layer {
        int layerId = 0;
        QString name;
        QImage image;          // Format_ARGB32_Premultiplied
        QPoint pos;
        float opacity = 1.0f;
        bool visible = true;
    };

    explicit Mixer(int width, int height, QObject* parent = nullptr);
    ~Mixer() override;

    int width() const { return m_width; }
    int height() const { return m_height; }

    /// Program video for the next composite (null = no video -> slate/black).
    void setBaseFrame(const FramePtr& frame);

    // Overlay management
    void setOverlay(int layerId, const QImage& image, const QPoint& pos = QPoint(),
                    float opacity = 1.0f, const QString& name = QString());
    void setOverlayVisible(int layerId, bool visible);
    void setOverlayOpacity(int layerId, float opacity);
    void setOverlayPosition(int layerId, const QPoint& pos);
    void removeOverlay(int layerId);
    void clearOverlays();
    QList<int> overlayIds() const;

    // Slate shown when there is no base frame and showSlate is on; otherwise black.
    void setSlate(const QImage& slate);
    void showSlate(bool show);
    bool slateVisible() const { QMutexLocker l(&m_mutex); return m_showSlate; }

    /// Produce the output picture for `frameNumber`.
    FramePtr composite(qint64 frameNumber);

    /// SMPTE-style colour bars with optional caption lines, sized to the mixer.
    static QImage generateColorBars(int width, int height, const QStringList& captions = {});
    /// Convert an ARGB QImage into a BGRA FramePtr (copy), straight alpha (a premultiplied image is un-premultiplied).
    static FramePtr imageToFrame(const QImage& img, FramePool* pool = nullptr);
    /// PE-08: the same copy, but the frame keeps PREMULTIPLIED alpha (QImage::Format_ARGB32_Premultiplied bytes). An image
    /// QPainter painted (premultiplied already) is copied as is, with no un-premultiply pass; an opaque RGB32 image too.
    static FramePtr imageToFramePremultiplied(const QImage& img, FramePool* pool = nullptr);

private:
    int m_width;
    int m_height;
    FramePool m_pool;
    mutable QMutex m_mutex;

    FramePtr m_base;
    FramePtr m_black;         // cached
    FramePtr m_slateFrame;    // cached conversion of m_slate
    QImage m_slate;
    bool m_showSlate = true;
    QMap<int, Layer> m_overlays;   // ordered by layerId = z-order

    bool anyOverlayVisible() const;
    FramePtr blackFrame();
};

#endif // MIXER_H
