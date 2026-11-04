#ifndef PLAYPLAY_BASICSOURCES_H
#define PLAYPLAY_BASICSOURCES_H

#include "source.h"
#include <QImage>
#include <QColor>
#include <QStringList>
#include <QMutex>
#include <atomic>
#include <memory>
#include <vector>

class DecoderPipeline;

/**
 * FileSource — media files, capture devices and network pulls through
 * DecoderPipeline. Program items are driven by the engine's item position;
 * overlays and live inputs run on their own timeline from the moment shown.
 *
 * spec: { type: file|capture|udp|stream, path|url, inPoint, outPoint, loop,
 *         inputFormat ("dshow"), inputOptions {...}, realtime (bool) }
 */
class FileSource : public LayerSource {
    Q_OBJECT
public:
    explicit FileSource(const QJsonObject& spec, QObject* parent = nullptr);
    /// Adopt an already-open pipeline (the engine's preloaded next item).
    FileSource(std::unique_ptr<DecoderPipeline> pipeline, const QJsonObject& spec, QObject* parent = nullptr);
    ~FileSource() override;

    QString type() const override { return m_type; }
    bool open(const OutputFormat& fmt) override;
    void close() override;
    bool isOpen() const override;
    FramePtr video(qint64 frame, double posSec) override;
    bool audio(const FramePtr& dst, qint64 frame, double posSec) override;
    bool hasAudio() const override;
    double durationSec() const override;
    bool atEnd() const override;
    QJsonObject status() const override;
    DecoderPipeline* pipeline() const { return m_dec.get(); }

private:
    QJsonObject m_spec;
    QString m_type = "file";
    std::unique_ptr<DecoderPipeline> m_dec;
    bool m_adopted = false;
};

/**
 * GeneratorSource — colour fields, SMPTE bars, slates with captions, tone.
 * spec: { type: color|black|bars|slate|tone, color "#rrggbb" or Qt "#aarrggbb", captions [..],
 *         toneHz (0 = silent), toneDbfs (-20), width/height (default raster) }
 */
class GeneratorSource : public LayerSource {
    Q_OBJECT
public:
    explicit GeneratorSource(const QJsonObject& spec, QObject* parent = nullptr);
    QString type() const override { return m_spec.value("type").toString("color"); }
    bool open(const OutputFormat& fmt) override;
    void close() override { m_frame.reset(); m_open = false; }
    bool isOpen() const override { return m_open; }
    FramePtr video(qint64 frame, double posSec) override;
    bool audio(const FramePtr& dst, qint64 frame, double posSec) override;
    bool hasAudio() const override { return m_toneHz > 0.0; }
    QSize nativeSize() const override { return m_size; }
    void update(const QJsonObject& params) override;

    static QImage renderBars(int w, int h, const QStringList& captions);

private:
    QJsonObject m_spec;
    OutputFormat m_fmt;
    QSize m_size;
    FramePtr m_frame;
    bool m_open = false;
    double m_toneHz = 0.0, m_toneAmp = 0.1;
    double m_phase = 0.0;
    void render();
};

/**
 * ImageSource — a still (PNG/JPG with alpha) or a numbered image sequence.
 * spec: { type: image|sequence, path (file or folder), fps (sequence), loop, fit: none|fit|stretch|fill }
 */
class ImageSource : public LayerSource {
    Q_OBJECT
public:
    explicit ImageSource(const QJsonObject& spec, QObject* parent = nullptr);
    QString type() const override { return m_sequence ? "sequence" : "image"; }
    bool open(const OutputFormat& fmt) override;
    void close() override;
    bool isOpen() const override { return m_open; }
    FramePtr video(qint64 frame, double posSec) override;
    QSize nativeSize() const override { return m_size; }
    double durationSec() const override;
    bool atEnd() const override;
    void update(const QJsonObject& params) override;

private:
    QJsonObject m_spec;
    OutputFormat m_fmt;
    bool m_open = false, m_sequence = false, m_loop = true;
    double m_fps = 25.0;
    QStringList m_files;
    QSize m_size;
    FramePtr m_still;
    std::vector<FramePtr> m_cache;      // decoded sequence frames (bounded)
    int m_lastIndex = -1;
    bool m_ended = false;
    FramePtr load(const QString& path);
};

/**
 * TemplateSource — broadcast graphics rendered with QPainter from a JSON
 * description (the CGElement model from the reference app, extended):
 *   kind: lower_third | bug | ticker | clock | countdown | text | box
 *   title, subtitle, text, fontFamily, fontSize, color, accentColor, bgColor,
 *   align, width/height (px; default per kind), speed (ticker px/s),
 *   format (clock, Qt date format), targetUtcMs | seconds (countdown),
 *   image (bug logo path), padding, radius
 * Static kinds render once and on update(). Animated kinds (PE-07 render cache, PLAYPLAY_TEMPLATE_CACHE=0 = per frame
 * as before): the ticker paints its background + label and one period of its crawl (background + one text copy) once
 * per spec and then only copies pixels per frame, every copy at the exact pixel QPainter would have put it, so the
 * picture is the per-frame render's bit for bit (or the source falls back to rendering every frame, see status().cache);
 * clock / countdown repaint only when their string changes.
 *
 * Caption kinds (see docs/cg-captions.md):
 *   karaoke  { words:[{word,start,end}] | segments:[{start,end,text,words:[..]}],
 *              mode: word|line, position: bottom|top|lower_third, align, maxLines (2),
 *              maxChars (48), fontSize, color, highlightColor, pastColor, bgColor, fill (true),
 *              inPointSec, startAtProgramSec, syncOffsetSec, timeBase: program|layer,
 *              leadSec (0.25), tailSec (1.2), lineGapSec (1.5), fadeSec (0.2) }
 *            Word times are file-relative seconds. The canvas is the full raster (position
 *            is honoured inside it, title-safe 5 % margins) but video() delivers only the
 *            caption box (contentRect) and reports isBlank() between lines. Lines break at
 *            maxChars AND at the measured pixel width of the title-safe box. Runs on PROGRAM
 *            time (wantsProgramTime) unless timeBase = "layer"; re-renders when the visible
 *            line / word / fill step / fade step changes (mode "line" or fill=false: less).
 *            Colours are Qt "#AARRGGBB" (alpha first), as QML emits them.
 *   factbox  { title, lines:[..] | "a\nb" | items:[{label,value}], width, accentColor, bgColor,
 *              color, fontSize, icon, image, footer, reveal: all|lines, revealIntervalSec,
 *              radius, shadow (true) }
 *            Panel-sized picture (nativeSize = panel incl. shadow margin) placed by the
 *            layer's anchor/x/y. Progressive reveal is driven by layer time. The image is
 *            decoded and scaled once (open(), or update() when image/width change), never per render.
 */
class TemplateSource : public LayerSource {
    Q_OBJECT
public:
    explicit TemplateSource(const QJsonObject& spec, QObject* parent = nullptr);
    ~TemplateSource() override;
    QString type() const override { return "template"; }
    bool open(const OutputFormat& fmt) override;
    void close() override;
    bool isOpen() const override { return m_open; }
    FramePtr video(qint64 frame, double posSec) override;
    QSize nativeSize() const override { return m_size; }
    QRect contentRect() const override;
    bool isBlank() const override;
    bool wantsProgramTime() const override;
    void update(const QJsonObject& params) override;
    QJsonObject status() const override;
    /// PE-08: with PLAYPLAY_PREMUL=1 (when the source is created) video() hands out the painter's premultiplied pixels
    /// as they are, instead of un-premultiplying every rendered picture for the compositor to premultiply again.
    bool deliversPremultiplied() const override { return m_premul; }

    /// The picture as QPainter painted it: Format_ARGB32_Premultiplied (premultiplied alpha) for every kind. Stateless:
    /// this is the per-frame path (and the reference the PE-07 cache is tested against).
    static QImage render(const QJsonObject& spec, const OutputFormat& fmt, double posSec, QSize* outSize = nullptr);

    // --- PE-07 render cache -------------------------------------------------------------------------------------
    /// Switch: PLAYPLAY_TEMPLATE_CACHE=0 (also off/false/no) renders ticker / clock / countdown every frame as before.
    /// Unset (the default) caches; the pictures are the same. Read when a source is created, fixed for its life.
    static bool renderCacheFromEnv();
    /// The string a clock / countdown shows at `posSec`: what render() draws (clock: the wall clock now).
    static QString clockText(const QJsonObject& spec, double posSec);
    /// The pixel column where QPainter puts a text item drawn at `x` when the font engine places glyphs on whole pixels:
    /// QFixed::fromReal truncates x * 64 toward zero, the raster engine then floors (100.99 -> 100, -49.0078 -> -49).
    static int textPixelX(double x);
    /// For tests and tools: the ticker's geometry {width, height, labelW, textW, gap, period}; the crawl repeats every
    /// `period` (= textW + gap) pixels and render() draws a copy at labelW + width - fmod(posSec * speed, period) - k * period.
    static QJsonObject tickerMetrics(const QJsonObject& spec, const OutputFormat& fmt);

    // --- karaoke layout (public so tests and tools can inspect it) -------------------------
    struct KWord { QString text; double start = 0.0, end = 0.0; int segment = -1; int line = -1; };
    struct KLine { int first = -1, last = -1; double start = 0.0, end = 0.0; };
    struct KaraokeLayout { std::vector<KWord> words; std::vector<KLine> lines; };
    /// Flatten words/segments into timed words and break them into caption lines: at segment
    /// boundaries, long silences, the maxChars budget and, when `fmt` is given, the measured pixel
    /// width of the title-safe caption box (needs a QGuiApplication for font metrics).
    static KaraokeLayout karaokeLayout(const QJsonObject& spec, const OutputFormat* fmt = nullptr);
    /// Map the time the compositor hands us to transcript (file) time.
    static double karaokeTime(const QJsonObject& spec, double posSec, bool programTime);
    /// Index of the caption line that should be on screen at `t`, -1 for none.
    static int karaokeLineAt(const KaraokeLayout& L, const QJsonObject& spec, double t);
    /// Full-raster picture (tests, previews): the caption box drawn where `position` puts it.
    static QImage renderKaraoke(const QJsonObject& spec, const OutputFormat& fmt, const KaraokeLayout& L, double t, double alpha, QSize* outSize);
    /// Only the caption box of line `li` (what video() delivers); `box` receives its place in the raster.
    static QImage renderKaraokeBox(const QJsonObject& spec, const OutputFormat& fmt, const KaraokeLayout& L, int li, double t, double alpha, QRect* box);
    /// `picture` = the pre-scaled image (TemplateSource decodes it once); null = load it from spec.image.
    static QImage renderFactbox(const QJsonObject& spec, const OutputFormat& fmt, int visibleRows, QSize* outSize, const QImage* picture = nullptr);
    /// spec.image decoded and scaled to the panel width (null image when there is none).
    static QImage factboxPicture(const QJsonObject& spec, const OutputFormat& fmt);
    static int factboxRowCount(const QJsonObject& spec);
    static int factboxVisibleRows(const QJsonObject& spec, double posSec);

private:
    // karaoke: what the last rendered caption picture depended on (compared as a struct: no bit packing)
    struct KaraokeKey {
        int line = -1, word = 0, step = 0, fade = 0;
        bool operator==(const KaraokeKey& o) const { return line == o.line && word == o.word && step == o.step && fade == o.fade; }
    };
    void loadFactboxPicture();
    /// A rendered picture as the frame video() hands out (premultiplied or straight, see deliversPremultiplied()).
    FramePtr toFrame(const QImage& img) const;
    // PE-07 ticker strip (TickerCache is defined in the .cpp)
    struct TickerCache;
    void buildTicker();
    FramePtr tickerFrame(double posSec);
    QImage tickerTile(int chunk) const;

    const bool m_premul = LayerSource::premultipliedFromEnv();   // PE-08, fixed for the source's life
    const bool m_cacheOn = renderCacheFromEnv();                  // PE-07, fixed for the source's life
    std::unique_ptr<TickerCache> m_ticker;                        // ticker: built on the first video() after open / update
    QString m_clockText;                                          // clock / countdown: the string of m_frame
    // What status() reports. status() runs on other threads than video() (the compositor's readers, PE-05), so these
    // are atomics written by video(), never read from the members video() works with.
    std::atomic<qint64> m_paints{0};                              // QPainter renders (whole pictures, strip base / tiles)
    std::atomic<int> m_cacheState{0};                             // CacheState (.cpp)
    std::atomic<int> m_tiles{0};                                  // ticker strip tiles held
    std::atomic<int> m_kWords{0}, m_kLines{0};                    // karaoke layout sizes
    QJsonObject m_spec;
    OutputFormat m_fmt;
    QSize m_size;                     // karaoke: the canvas (full raster); other kinds: the picture
    FramePtr m_frame;
    QRect m_content;                  // karaoke: where the delivered caption box sits in the canvas
    std::atomic<bool> m_blank{false}; // karaoke: no line on screen (atomic: isBlank() / status() read it too)
    bool m_open = false;
    bool m_animated = false;
    bool m_dirty = true;
    QImage m_logo;
    QImage m_picture;                 // factbox: image decoded + scaled once
    QString m_pictureKey;             // image path | panel width the picture was scaled for
    // caption kinds: cached layout + the state key of the last rendered picture
    KaraokeLayout m_karaoke;
    KaraokeKey m_kkey;
    bool m_kkeyValid = false;
    qint64 m_lastKey = -2;            // factbox: visible rows of the last render
    std::atomic<int> m_lastLine{-1};  // karaoke: line on screen (status() reads it)
    double m_lineChangeT = -1e9;
};

#endif // PLAYPLAY_BASICSOURCES_H
