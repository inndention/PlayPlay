#ifndef PLAYPLAY_COMPOSITOR_SOURCE_H
#define PLAYPLAY_COMPOSITOR_SOURCE_H

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QSize>
#include <QRect>
#include <memory>
#include "../mediaframe.h"
#include "../../core/framerate.h"

/**
 * OutputFormat — the channel raster every source renders into.
 */
struct OutputFormat {
    int width = 1920;
    int height = 1080;
    FrameRate rate = FrameRate::ntsc();
    int sampleRate = 48000;
    int channels = 2;
};

/**
 * LayerSource — anything that can feed a compositor layer.
 *
 * Contract (all calls on the clock thread, none may block on I/O):
 *   video(frame, posSec)  the BGRA picture to show at `posSec` seconds since the
 *                         layer was shown (program sources, and layers that return
 *                         wantsProgramTime(), get the item position instead). May
 *                         return the previous frame (repeat) or null (nothing to
 *                         draw: transparent). Straight alpha unless the source says
 *                         deliversPremultiplied().
 *   audio(dst)            fill `dst` (FLTP, nb_samples set by the caller) with this
 *                         source's audio for the frame. Return false for silence.
 *   nativeSize()          the source's own picture size, used by layers that place
 *                         the picture at its natural size (bugs, lower thirds).
 *
 * Sources that decode or receive on their own threads hand frames across with
 * FramePtr references; the compositor never copies pixels except when blending.
 */
class LayerSource : public QObject {
    Q_OBJECT
public:
    explicit LayerSource(QObject* parent = nullptr) : QObject(parent) {}
    ~LayerSource() override = default;

    virtual QString type() const = 0;
    virtual bool open(const OutputFormat& fmt) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    virtual FramePtr video(qint64 frame, double posSec) = 0;
    virtual bool audio(const FramePtr& dst, qint64 frame, double posSec) { Q_UNUSED(dst); Q_UNUSED(frame); Q_UNUSED(posSec); return false; }
    virtual bool hasAudio() const { return false; }

    /// Natural picture size (0x0 = fills the raster).
    virtual QSize nativeSize() const { return QSize(); }
    /// Layers whose picture is a function of the PROGRAM item's position (karaoke captions,
    /// transcript-synced graphics) return true: the compositor then passes the on-air item
    /// position to video()/audio() instead of seconds since the layer was shown.
    virtual bool wantsProgramTime() const { return false; }
    /// Finite sources report their length; 0 = endless.
    virtual double durationSec() const { return 0.0; }
    virtual bool atEnd() const { return false; }
    /// Runtime parameter update (text, colours, URL...).
    virtual void update(const QJsonObject& params) { Q_UNUSED(params); }
    virtual QJsonObject status() const { QJsonObject o; o["type"] = type(); o["open"] = isOpen(); return o; }
    /// Sources that draw into a sub-rectangle of a larger canvas (karaoke captions: one caption box
    /// inside a full-raster canvas) return that rectangle in canvas pixels, and nativeSize() must
    /// then be the canvas size. video() delivers only the sub-rectangle; the compositor places the
    /// canvas as it would a full-size picture and draws just the rectangle, so a caption costs
    /// the pixels of its box, not of the raster. Empty (default) = the picture IS the canvas.
    /// (Declared after status() so the vtable of older translation units keeps its layout.)
    virtual QRect contentRect() const { return QRect(); }
    /// True while the source has nothing to draw (a caption layer between two lines): the
    /// compositor then skips the layer instead of blending a transparent picture.
    virtual bool isBlank() const { return false; }
    /// PE-08: true when video() delivers PREMULTIPLIED BGRA (the byte layout of QImage::Format_ARGB32_Premultiplied)
    /// instead of straight alpha. The compositor then blends the picture as it is: the source skips the un-premultiply
    /// pass after painting, and neither QPainter nor the GPU shader premultiplies it again per draw. A source answers the
    /// same for its whole life (every picture it hands out, including repeats). Only sources that paint or decode RGBA
    /// themselves opt in (templates, HTML), and only when premultipliedFromEnv() is on when they are created.
    /// (Declared after isBlank() so the vtable of older translation units keeps its layout.)
    virtual bool deliversPremultiplied() const { return false; }
    /// PE-08 switch: PLAYPLAY_PREMUL=1 (also on/true/yes) makes templates and HTML pages deliver premultiplied pictures.
    /// Unset or 0 (the default) keeps straight alpha, the pre-PE-08 output bit for bit. Read when a source is created.
    static bool premultipliedFromEnv() {
        const QByteArray v = qgetenv("PLAYPLAY_PREMUL").trimmed().toLower();
        return v == "1" || v == "on" || v == "true" || v == "yes";
    }

    QString lastError() const { return m_error; }

    /**
     * Factory. `spec.type` selects the source:
     *   file | capture | udp | stream   (FileSource over DecoderPipeline)
     *   color | black | bars | slate | tone    (GeneratorSource)
     *   image | sequence                (ImageSource)
     *   template                        (TemplateSource: lower_third, bug, ticker, clock, countdown, text, box)
     *   html                            (HtmlSource, only when built with WebEngine)
     *   ndi                             (NdiSource)
     *   dektec_sdi_in | sdi_in          (DekTecSdiInSource)
     *   audio_in | line_in              (AudioInSource: sound card line input, audio only)
     */
    static std::unique_ptr<LayerSource> create(const QJsonObject& spec, QString* error = nullptr);

protected:
    void setError(const QString& e) { m_error = e; }
    QString m_error;
};

#endif // PLAYPLAY_COMPOSITOR_SOURCE_H
