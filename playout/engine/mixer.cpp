#include "mixer.h"
#include <QPainter>
#include <QFont>
#include <QDebug>
#include <cstring>

Mixer::Mixer(int width, int height, QObject* parent)
    : QObject(parent)
    , m_width(width)
    , m_height(height)
    , m_pool(width, height, kMixerPixelFormat)
{
    m_slate = generateColorBars(width, height, {"PLAY PLAY", "NO PROGRAM"});
}

Mixer::~Mixer() = default;

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------

void Mixer::setBaseFrame(const FramePtr& frame) {
    QMutexLocker l(&m_mutex);
    m_base = frame;
}

void Mixer::setOverlay(int layerId, const QImage& image, const QPoint& pos, float opacity, const QString& name) {
    QImage premul = image.format() == QImage::Format_ARGB32_Premultiplied
        ? image : image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QMutexLocker l(&m_mutex);
    Layer& layer = m_overlays[layerId];
    layer.layerId = layerId;
    layer.image = premul;
    layer.pos = pos;
    layer.opacity = qBound(0.0f, opacity, 1.0f);
    if (!name.isEmpty()) layer.name = name;
}

void Mixer::setOverlayVisible(int layerId, bool visible) {
    QMutexLocker l(&m_mutex);
    auto it = m_overlays.find(layerId);
    if (it != m_overlays.end()) it->visible = visible;
}

void Mixer::setOverlayOpacity(int layerId, float opacity) {
    QMutexLocker l(&m_mutex);
    auto it = m_overlays.find(layerId);
    if (it != m_overlays.end()) it->opacity = qBound(0.0f, opacity, 1.0f);
}

void Mixer::setOverlayPosition(int layerId, const QPoint& pos) {
    QMutexLocker l(&m_mutex);
    auto it = m_overlays.find(layerId);
    if (it != m_overlays.end()) it->pos = pos;
}

void Mixer::removeOverlay(int layerId) {
    QMutexLocker l(&m_mutex);
    m_overlays.remove(layerId);
}

void Mixer::clearOverlays() {
    QMutexLocker l(&m_mutex);
    m_overlays.clear();
}

QList<int> Mixer::overlayIds() const {
    QMutexLocker l(&m_mutex);
    return m_overlays.keys();
}

void Mixer::setSlate(const QImage& slate) {
    QMutexLocker l(&m_mutex);
    m_slate = slate.scaled(m_width, m_height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    m_slateFrame.reset();
}

void Mixer::showSlate(bool show) {
    QMutexLocker l(&m_mutex);
    m_showSlate = show;
}

bool Mixer::anyOverlayVisible() const {
    for (const Layer& layer : m_overlays) {
        if (layer.visible && layer.opacity > 0.0f && !layer.image.isNull()) return true;
    }
    return false;
}

FramePtr Mixer::blackFrame() {
    if (!m_black) {
        m_black = m_pool.get();
        if (m_black) {
            for (int y = 0; y < m_height; ++y) {
                uint32_t* row = reinterpret_cast<uint32_t*>(m_black->data[0] + y * m_black->linesize[0]);
                for (int x = 0; x < m_width; ++x) row[x] = 0xFF000000u;
            }
        }
    }
    return m_black;
}

// ---------------------------------------------------------------------------
// Composite
// ---------------------------------------------------------------------------

FramePtr Mixer::composite(qint64 frameNumber) {
    Q_UNUSED(frameNumber);
    QMutexLocker l(&m_mutex);

    // Pick the base picture.
    FramePtr base = m_base;
    if (!base) {
        if (m_showSlate && !m_slate.isNull()) {
            if (!m_slateFrame) m_slateFrame = imageToFrame(m_slate, &m_pool);
            base = m_slateFrame;
        } else {
            base = blackFrame();
        }
    }
    if (!base) return nullptr;

    // Fast path: nothing to draw on top.
    if (!anyOverlayVisible()) return base;

    // Copy base into a fresh pooled frame and paint overlays.
    FramePtr out = m_pool.get();
    if (!out) return base;
    const int rowBytes = m_width * 4;
    for (int y = 0; y < m_height; ++y) {
        memcpy(out->data[0] + y * out->linesize[0], base->data[0] + y * base->linesize[0], rowBytes);
    }

    QImage canvas(out->data[0], m_width, m_height, out->linesize[0], QImage::Format_ARGB32_Premultiplied);
    QPainter p(&canvas);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    for (const Layer& layer : m_overlays) {            // QMap iterates in key order = z-order
        if (!layer.visible || layer.opacity <= 0.0f || layer.image.isNull()) continue;
        p.setOpacity(layer.opacity);
        p.drawImage(layer.pos, layer.image);
    }
    p.end();
    out->pts = base->pts;
    return out;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {
/// Rows of a 32-bit BGRA-ordered QImage into a frame (from `pool` when given, else a fresh one).
FramePtr copyImageRows(const QImage& src, FramePool* pool) {
    FramePtr f = pool ? pool->get() : allocVideoFrame(src.width(), src.height(), kMixerPixelFormat);
    if (!f) return nullptr;
    const int rows = qMin(src.height(), f->height);
    const int rowBytes = qMin(src.width(), f->width) * 4;
    for (int y = 0; y < rows; ++y) {
        memcpy(f->data[0] + y * f->linesize[0], src.constScanLine(y), rowBytes);
    }
    return f;
}
} // namespace

FramePtr Mixer::imageToFrame(const QImage& img, FramePool* pool) {
    if (img.isNull()) return nullptr;
    QImage src = img.format() == QImage::Format_ARGB32 ? img : img.convertToFormat(QImage::Format_ARGB32);
    return copyImageRows(src, pool);
}

FramePtr Mixer::imageToFramePremultiplied(const QImage& img, FramePool* pool) {
    if (img.isNull()) return nullptr;
    // RGB32 is opaque with alpha forced to 0xFF: already valid premultiplied bytes, nothing to convert.
    const bool asIs = img.format() == QImage::Format_ARGB32_Premultiplied || img.format() == QImage::Format_RGB32;
    QImage src = asIs ? img : img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    return copyImageRows(src, pool);
}

QImage Mixer::generateColorBars(int width, int height, const QStringList& captions) {
    QImage img(width, height, QImage::Format_ARGB32);
    img.fill(Qt::black);
    QPainter p(&img);

    // Top 2/3: 75% SMPTE bars
    const QColor bars[7] = {
        QColor(191, 191, 191), QColor(191, 191, 0), QColor(0, 191, 191), QColor(0, 191, 0),
        QColor(191, 0, 191), QColor(191, 0, 0), QColor(0, 0, 191)
    };
    const int barH = height * 2 / 3;
    const double barW = width / 7.0;
    for (int i = 0; i < 7; ++i) {
        p.fillRect(QRectF(i * barW, 0, barW + 1, barH), bars[i]);
    }
    // Middle strip: reverse bars (blue, black, magenta, black, cyan, black, grey)
    const QColor rev[7] = {
        QColor(0, 0, 191), Qt::black, QColor(191, 0, 191), Qt::black,
        QColor(0, 191, 191), Qt::black, QColor(191, 191, 191)
    };
    const int stripY = barH;
    const int stripH = height / 12;
    for (int i = 0; i < 7; ++i) {
        p.fillRect(QRectF(i * barW, stripY, barW + 1, stripH), rev[i]);
    }
    // Bottom: -I, white, +Q, black, pluge
    const int botY = stripY + stripH;
    const int botH = height - botY;
    const double unit = width / 28.0;
    p.fillRect(QRectF(0, botY, unit * 5, botH), QColor(0, 33, 76));
    p.fillRect(QRectF(unit * 5, botY, unit * 5, botH), Qt::white);
    p.fillRect(QRectF(unit * 10, botY, unit * 5, botH), QColor(50, 0, 106));
    p.fillRect(QRectF(unit * 15, botY, unit * 13, botH), Qt::black);
    p.fillRect(QRectF(unit * 20, botY, unit, botH), QColor(9, 9, 9));
    p.fillRect(QRectF(unit * 22, botY, unit, botH), QColor(29, 29, 29));

    // Captions
    if (!captions.isEmpty()) {
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        QFont font("Segoe UI", height / 18, QFont::Bold);
        p.setFont(font);
        QRect box(width / 8, height / 3 - height / 10, width * 3 / 4, height / 5 + captions.size() * height / 14);
        p.fillRect(box, QColor(0, 0, 0, 190));
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, captions.join('\n'));
    }
    p.end();
    return img;
}
