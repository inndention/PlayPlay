#include "basicsources.h"
#include "../decoderpipeline.h"
#include "../mixer.h"
#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QFontMetrics>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QJsonArray>
#include <QUrl>
#include <QPen>
#include <QDebug>
#include <QTextLayout>
#include <QGlyphRun>
#include <cmath>
#include <cstring>

// ---------------------------------------------------------------------------
// FileSource
// ---------------------------------------------------------------------------

FileSource::FileSource(const QJsonObject& spec, QObject* parent) : LayerSource(parent), m_spec(spec) {
    const QString t = spec.value("type").toString("file");
    m_type = (t == "capture" || t == "udp" || t == "stream") ? t : "file";
}

FileSource::FileSource(std::unique_ptr<DecoderPipeline> pipeline, const QJsonObject& spec, QObject* parent)
    : LayerSource(parent), m_spec(spec), m_dec(std::move(pipeline)), m_adopted(true) {}

FileSource::~FileSource() { close(); }

bool FileSource::open(const OutputFormat& fmt) {
    if (m_adopted && m_dec) return m_dec->isOpen();
    DecoderPipeline::Config dc;
    dc.outWidth = fmt.width; dc.outHeight = fmt.height; dc.outRate = fmt.rate;
    dc.sampleRate = fmt.sampleRate; dc.channels = fmt.channels;
    dc.hwAccel = m_spec.value("hwDecode").toBool(true);
    dc.loop = m_spec.value("loop").toBool(false);
    dc.realtime = m_spec.value("realtime").toBool(m_type != "file");
    dc.inputFormat = m_spec.value("inputFormat").toString(m_type == "capture" ? "dshow" : QString());
    dc.inputOptions = m_spec.value("inputOptions").toObject();
    if (m_type == "capture" && !dc.inputOptions.contains("rtbufsize")) dc.inputOptions["rtbufsize"] = "100M";
    const QString path = m_spec.value("path").toString(m_spec.value("url").toString());
    if (path.isEmpty()) { setError("no path/url"); return false; }
    m_dec = std::make_unique<DecoderPipeline>(dc);
    if (!m_dec->open(path, m_spec.value("inPoint").toDouble(0.0), m_spec.value("outPoint").toDouble(0.0))) {
        setError(m_dec->lastError());
        m_dec.reset();
        return false;
    }
    return true;
}

void FileSource::close() {
    if (m_dec) { m_dec->close(); m_dec.reset(); }
}

bool FileSource::isOpen() const { return m_dec && m_dec->isOpen(); }
FramePtr FileSource::video(qint64, double posSec) { return m_dec ? m_dec->videoFrameAt(posSec) : nullptr; }
bool FileSource::audio(const FramePtr& dst, qint64, double) { return m_dec ? m_dec->pullAudio(dst) : false; }
bool FileSource::hasAudio() const { return m_dec && m_dec->hasAudio(); }
double FileSource::durationSec() const { return m_dec ? m_dec->durationSec() : 0.0; }
bool FileSource::atEnd() const { return m_dec ? m_dec->atEnd() : true; }

QJsonObject FileSource::status() const {
    QJsonObject o = LayerSource::status();
    if (m_dec) {
        const auto st = m_dec->stats();
        o["path"] = m_dec->path(); o["queue"] = st.queueDepth; o["dropped"] = double(st.droppedVideo);
        o["repeated"] = double(st.repeatedVideo); o["audioUnderruns"] = double(st.audioUnderruns); o["hw"] = st.hwDevice;
    }
    return o;
}

// ---------------------------------------------------------------------------
// GeneratorSource
// ---------------------------------------------------------------------------

GeneratorSource::GeneratorSource(const QJsonObject& spec, QObject* parent) : LayerSource(parent), m_spec(spec) {}

bool GeneratorSource::open(const OutputFormat& fmt) {
    m_fmt = fmt;
    m_size = QSize(m_spec.value("width").toInt(fmt.width), m_spec.value("height").toInt(fmt.height));
    m_toneHz = m_spec.value("toneHz").toDouble(type() == "tone" ? 1000.0 : 0.0);
    m_toneAmp = std::pow(10.0, m_spec.value("toneDbfs").toDouble(-20.0) / 20.0);
    render();
    m_open = (m_frame != nullptr);
    return m_open;
}

QImage GeneratorSource::renderBars(int w, int h, const QStringList& captions) {
    return Mixer::generateColorBars(w, h, captions);
}

void GeneratorSource::render() {
    const QString t = type();
    QImage img;
    if (t == "bars" || t == "slate") {
        QStringList caps; for (const auto& v : m_spec.value("captions").toArray()) caps << v.toString();
        img = renderBars(m_size.width(), m_size.height(), caps);
    } else {
        img = QImage(m_size, QImage::Format_ARGB32);
        QColor c(m_spec.value("color").toString(t == "black" ? "#000000" : "#000000"));
        if (!c.isValid()) c = Qt::black;
        img.fill(c);
    }
    m_frame = Mixer::imageToFrame(img);
}

void GeneratorSource::update(const QJsonObject& params) {
    for (auto it = params.begin(); it != params.end(); ++it) m_spec[it.key()] = it.value();
    if (params.contains("toneHz")) m_toneHz = params["toneHz"].toDouble();
    if (params.contains("toneDbfs")) m_toneAmp = std::pow(10.0, params["toneDbfs"].toDouble() / 20.0);
    render();
}

FramePtr GeneratorSource::video(qint64, double) { return m_frame; }

bool GeneratorSource::audio(const FramePtr& dst, qint64, double) {
    if (m_toneHz <= 0.0 || !dst) return false;
    const double step = 2.0 * M_PI * m_toneHz / m_fmt.sampleRate;
    for (int i = 0; i < dst->nb_samples; ++i) {
        const float v = float(std::sin(m_phase) * m_toneAmp);
        m_phase += step; if (m_phase > 2.0 * M_PI) m_phase -= 2.0 * M_PI;
        for (int c = 0; c < dst->ch_layout.nb_channels; ++c) reinterpret_cast<float*>(dst->data[c])[i] = v;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ImageSource
// ---------------------------------------------------------------------------

ImageSource::ImageSource(const QJsonObject& spec, QObject* parent) : LayerSource(parent), m_spec(spec) {}

FramePtr ImageSource::load(const QString& path) {
    QImage img(path);
    if (img.isNull()) return nullptr;
    const QString fit = m_spec.value("fit").toString("none");
    if (fit != "none") {
        const Qt::AspectRatioMode mode = fit == "stretch" ? Qt::IgnoreAspectRatio : (fit == "fill" ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio);
        img = img.scaled(m_fmt.width, m_fmt.height, mode, Qt::SmoothTransformation);
        if (fit == "fit" || fit == "fill") {
            QImage canvas(m_fmt.width, m_fmt.height, QImage::Format_ARGB32);
            canvas.fill(Qt::transparent);
            QPainter p(&canvas);
            p.drawImage((m_fmt.width - img.width()) / 2, (m_fmt.height - img.height()) / 2, img);
            img = canvas;
        }
    }
    if (img.format() != QImage::Format_ARGB32) img = img.convertToFormat(QImage::Format_ARGB32);
    m_size = img.size();
    return Mixer::imageToFrame(img);
}

bool ImageSource::open(const OutputFormat& fmt) {
    m_fmt = fmt;
    const QString path = m_spec.value("path").toString();
    m_loop = m_spec.value("loop").toBool(true);
    m_fps = m_spec.value("fps").toDouble(fmt.rate.fps());
    QFileInfo fi(path);
    if (fi.isDir()) {
        m_sequence = true;
        QDir d(path);
        for (const QString& f : d.entryList({"*.png", "*.PNG", "*.jpg", "*.jpeg", "*.tga", "*.bmp"}, QDir::Files, QDir::Name)) m_files << d.filePath(f);
        if (m_files.isEmpty()) { setError("no images in " + path); return false; }
        m_cache.assign(size_t(m_files.size()), nullptr);
        m_still = load(m_files.first());
        if (!m_still) { setError("cannot read " + m_files.first()); return false; }
        m_cache[0] = m_still;
    } else {
        m_still = load(path);
        if (!m_still) { setError("cannot read image " + path); return false; }
    }
    m_open = true;
    return true;
}

void ImageSource::close() { m_still.reset(); m_cache.clear(); m_files.clear(); m_open = false; }

FramePtr ImageSource::video(qint64, double posSec) {
    if (!m_sequence) return m_still;
    qint64 idx = qint64(posSec * m_fps);
    if (idx >= m_files.size()) {
        if (m_loop) idx %= m_files.size(); else { m_ended = true; idx = m_files.size() - 1; }
    }
    if (idx == m_lastIndex && m_cache[size_t(idx)]) return m_cache[size_t(idx)];
    if (!m_cache[size_t(idx)]) m_cache[size_t(idx)] = load(m_files[int(idx)]);
    // Keep memory bounded: forget frames far behind the current index (unless looping a short sequence).
    if (m_files.size() > 120) { const qint64 old = idx - 30; if (old >= 0) m_cache[size_t(old)].reset(); }
    m_lastIndex = int(idx);
    return m_cache[size_t(idx)];
}

double ImageSource::durationSec() const { return (m_sequence && !m_loop) ? m_files.size() / m_fps : 0.0; }
bool ImageSource::atEnd() const { return m_ended; }
void ImageSource::update(const QJsonObject& params) { for (auto it = params.begin(); it != params.end(); ++it) m_spec[it.key()] = it.value(); if (params.contains("path")) { close(); open(m_fmt); } }

// ---------------------------------------------------------------------------
// TemplateSource
// ---------------------------------------------------------------------------

// Karaoke sliding fill: the current word is re-rendered at most this many times (steps of 1/12 of the word).
static constexpr int kKaraokeFillSteps = 12;

namespace {
QColor colorOr(const QJsonObject& o, const char* key, const QColor& def) {
    const QString s = o.value(key).toString();
    QColor c(s);
    return (s.isEmpty() || !c.isValid()) ? def : c;
}
QFont fontFor(const QJsonObject& o, int defSize, int weight = QFont::Bold) {
    QFont f(o.value("fontFamily").toString("Segoe UI"), o.value("fontSize").toInt(defSize), weight);
    return f;
}
void roundedRect(QPainter& p, const QRectF& r, double radius, const QColor& fill) {
    QPainterPath path; path.addRoundedRect(r, radius, radius); p.fillPath(path, fill);
}

// --- Ticker geometry: one definition for render() (per frame) and the PE-07 strip, so both put the same copies of the
// crawl at the same x, computed from the same doubles.
struct TickerGeom {
    int w = 0, h = 0, labelW = 0, textW = 1, gap = 0;
    double speed = 120.0;
    QString label, text;
    QFont labelFont, font;
    QColor bg, accent, fg, labelColor;
};
TickerGeom tickerGeom(const QJsonObject& spec, const OutputFormat& fmt) {
    TickerGeom g;
    g.accent = colorOr(spec, "accentColor", QColor(0, 212, 255));
    g.bg = colorOr(spec, "bgColor", QColor(5, 5, 10, 220));
    g.fg = colorOr(spec, "color", Qt::white);
    g.labelColor = colorOr(spec, "labelColor", QColor(5, 5, 10));
    const int pad = spec.value("padding").toInt(18);
    g.w = spec.value("width").toInt(fmt.width);
    g.h = spec.value("height").toInt(int(fmt.height * 0.06));
    g.label = spec.value("label").toString();
    if (!g.label.isEmpty()) { g.labelFont = fontFor(spec, int(g.h * 0.5)); g.labelW = QFontMetrics(g.labelFont).horizontalAdvance(g.label) + pad * 2; }
    g.text = spec.value("text").toString();
    if (spec.contains("items")) { QStringList items; for (const auto& v : spec["items"].toArray()) items << v.toString(); g.text = items.join(QStringLiteral("   •   ")); }
    g.font = fontFor(spec, int(g.h * 0.5), QFont::DemiBold);
    g.textW = qMax(1, QFontMetrics(g.font).horizontalAdvance(g.text));
    g.speed = spec.value("speed").toDouble(120.0);   // px/s
    g.gap = g.w / 3;
    return g;
}
// Background and label: everything in the ticker picture but the crawl.
void paintTickerBase(QPainter& p, const TickerGeom& g) {
    p.fillRect(QRect(0, 0, g.w, g.h), g.bg);
    if (!g.label.isEmpty()) {
        p.fillRect(QRect(0, 0, g.labelW, g.h), g.accent);
        p.setFont(g.labelFont); p.setPen(g.labelColor);
        p.drawText(QRect(0, 0, g.labelW, g.h), Qt::AlignCenter, g.label);
    }
}
// One copy of the crawl text with its left edge at x (font and pen already set).
void drawTickerText(QPainter& p, const TickerGeom& g, double x) {
    p.drawText(QRectF(x, 0, g.textW + 10, g.h), Qt::AlignVCenter | Qt::AlignLeft, g.text);
}
// The x of every copy of the crawl render() draws at posSec, right to left (the copies repeat every textW + gap).
template <typename Fn> void forEachTickerCopy(const TickerGeom& g, double posSec, Fn&& fn) {
    const double offset = std::fmod(posSec * g.speed, double(g.textW + g.gap));
    for (double x = g.labelW + g.w - offset; x > g.labelW - g.textW; x -= (g.textW + g.gap)) {
        fn(x);
        if (x < g.labelW) break;
    }
}

// Clock / countdown picture for a given string (render() and the PE-07 cache paint it the same way).
QImage clockPicture(const QJsonObject& spec, const OutputFormat& fmt, const QString& text) {
    const QColor bg = colorOr(spec, "bgColor", QColor(5, 5, 10, 220));
    const QColor fg = colorOr(spec, "color", Qt::white);
    const int pad = spec.value("padding").toInt(18);
    const double radius = spec.value("radius").toDouble(6.0);
    const int h = spec.value("height").toInt(int(fmt.height * 0.07));
    QFont f(spec.value("fontFamily").toString("Consolas"), spec.value("fontSize").toInt(int(h * 0.55)), QFont::Bold);
    QFontMetrics fm(f);
    const int w = spec.value("width").toInt(fm.horizontalAdvance(text) + pad * 2);
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
    QPainter p(&img); p.setRenderHint(QPainter::Antialiasing); p.setRenderHint(QPainter::TextAntialiasing);
    roundedRect(p, QRectF(0, 0, w, h), radius, bg);
    p.setFont(f); p.setPen(fg); p.drawText(QRect(0, 0, w, h), Qt::AlignCenter, text);
    p.end();
    return img;
}

// --- PE-07 ticker strip ---------------------------------------------------------------------------------------------
// A crawl whose period fits kTickerTileMax is one tile; a longer one is cut into kTickerChunk-wide chunks, painted when
// they scroll in and dropped (least recently used first) beyond kTickerTilesKept.
constexpr int kTickerTileMax = 8192;
constexpr int kTickerChunk = 4096;
constexpr int kTickerTilesKept = 6;

// What status().cache reports (TemplateSource::m_cacheState).
enum CacheState { CachePending = 0, CacheOff, CacheStrip, CacheString,
                  CachePerFrameEmpty, CachePerFrameNarrow, CachePerFrameClip, CachePerFrameLines, CachePerFrameSubpixel };
QString cacheStateName(int s) {
    switch (s) {
    case CacheOff: return QStringLiteral("off");
    case CacheStrip: return QStringLiteral("strip");
    case CacheString: return QStringLiteral("string");
    case CachePerFrameEmpty: return QStringLiteral("per-frame: no crawl area");
    case CachePerFrameNarrow: return QStringLiteral("per-frame: gap narrower than the glyphs");
    case CachePerFrameClip: return QStringLiteral("per-frame: text taller than the ticker");
    case CachePerFrameLines: return QStringLiteral("per-frame: multi-line text");
    case CachePerFrameSubpixel: return QStringLiteral("per-frame: sub-pixel text");
    default: return QStringLiteral("pending");
    }
}

// The strip is exact only when the font engine draws a copy at x as the copy at textPixelX(x), moved by whole pixels:
// shaped glyph positions on whole pixels (hinted advances), no sub-pixel glyph positioning, and QFixed's truncation
// for an x just below a whole pixel. Measured true for DirectWrite (windows QPA) and FreeType (offscreen) on this box
// (test_template_cache glyphSnapping); checked again here per text and font, so another engine falls back instead.
bool textOnWholePixels(const TickerGeom& g) {
    QImage dev(1, 1, QImage::Format_ARGB32_Premultiplied);      // QPainter draws on a QImage: use its DPI, not the screen's
    QTextLayout layout(g.text, g.font, &dev);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    layout.endLayout();
    if (line.isValid())
        for (const QGlyphRun& run : layout.glyphRuns())
            for (const QPointF& pt : run.positions()) if (pt.x() != std::floor(pt.x())) return false;
    const QString sample = g.text.simplified().left(16) + QStringLiteral("Ag");   // "Ag": ink even for a blank text
    const int sw = QFontMetrics(g.font, &dev).horizontalAdvance(sample) + 24;
    auto draw = [&](double x) {
        QImage img(sw, g.h, QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
        QPainter p(&img); p.setRenderHint(QPainter::TextAntialiasing); p.setFont(g.font); p.setPen(Qt::white);
        p.drawText(QRectF(x, 0, sw, g.h), Qt::AlignVCenter | Qt::AlignLeft, sample);
        p.end();
        return img;
    };
    const QImage at8 = draw(8.0);
    for (double x : {8.25, 8.5, 8.75, 8.984375}) if (draw(x) != at8) return false;   // fractions floor to 8
    return draw(-0.0078125) == draw(0.0);                                           // -1/128 truncates to 0, not -1
}

// The columns a copy of the crawl text really covers, relative to its origin, measured by drawing its two ends (the
// same drawText call as the tile). QFontMetrics::boundingRect() is not good enough: with DirectWrite it misses the
// antialiased edge column, and Qt's box engine (no fonts, offscreen) reports x = 100000.
struct Ink { int left = 0, right = 0; };
Ink measureInk(const TickerGeom& g) {
    const int M = qMax(32, 2 * g.h);                              // ink further than 2 line heights from an end: none
    auto scan = [&](double x, bool first) {                       // first / last ink column of the text drawn at x
        QImage img(2 * M, g.h, QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
        QPainter p(&img); p.setRenderHint(QPainter::TextAntialiasing); p.setFont(g.font); p.setPen(Qt::white);
        drawTickerText(p, g, x);
        p.end();
        int found = -1;
        for (int y = 0; y < g.h; ++y) {
            const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
            if (first) { for (int c = 0; c < (found < 0 ? 2 * M : found); ++c) if (qAlpha(row[c])) { found = c; break; } }
            else for (int c = 2 * M - 1; c > found; --c) if (qAlpha(row[c])) { found = c; break; }
        }
        return found;
    };
    const int l = scan(M, true), r = scan(M - g.textW, false);    // origin at M / advance end at M
    Ink ink;
    ink.left = l < 0 ? M : l - M;                                 // nothing near the start: the ink begins further in
    ink.right = r < 0 ? g.textW - M : r - M + g.textW;
    return ink;
}
} // namespace

struct TemplateSource::TickerCache {
    TickerGeom g;
    bool strip = false;
    int period = 0;                // textW + gap: the crawl repeats every `period` pixels
    int half = 0;                  // a copy's left edge sits at tile column `half` (gap / 2): one tile = one copy's zone
    int chunkW = 0;                // tile columns per chunk (the whole period when it fits kTickerTileMax)
    QImage base;                   // background + label, as delivered (premultiplied or straight)
    std::vector<QImage> tiles;     // chunk j = tile columns [j * chunkW, min(period, (j + 1) * chunkW)), painted on first use
    std::vector<qint64> tileUsed;  // serial of the last picture that used the chunk
    std::unique_ptr<FramePool> pool;
    std::vector<int> lastX, xs;    // pixel x of every copy: in the last delivered picture / in this one
    struct Piece { int dst, chunk, src, len; };
    std::vector<Piece> pieces;     // raster column, chunk, chunk column, width
    qint64 serial = 0;
};

TemplateSource::TemplateSource(const QJsonObject& spec, QObject* parent) : LayerSource(parent), m_spec(spec) {}
TemplateSource::~TemplateSource() = default;

bool TemplateSource::renderCacheFromEnv() {
    const QByteArray v = qgetenv("PLAYPLAY_TEMPLATE_CACHE").trimmed().toLower();
    return !(v == "0" || v == "off" || v == "false" || v == "no");
}

QJsonObject TemplateSource::tickerMetrics(const QJsonObject& spec, const OutputFormat& fmt) {
    const TickerGeom g = tickerGeom(spec, fmt);
    return QJsonObject{{"width", g.w}, {"height", g.h}, {"labelW", g.labelW}, {"textW", g.textW}, {"gap", g.gap}, {"period", g.textW + g.gap}};
}

int TemplateSource::textPixelX(double x) {
    const qint64 fx = qint64(x * 64.0);                         // QFixed::fromReal: 1/64 px, truncated toward zero
    return int(fx >= 0 ? fx / 64 : -((-fx + 63) / 64));         // the raster engine then floors to the pixel
}

QString TemplateSource::clockText(const QJsonObject& spec, double posSec) {
    if (spec.value("kind").toString() == "clock") return QDateTime::currentDateTime().toString(spec.value("format").toString("hh:mm:ss"));
    qint64 remaining = 0;
    if (spec.contains("targetUtcMs")) remaining = qint64(spec["targetUtcMs"].toDouble()) - QDateTime::currentMSecsSinceEpoch();
    else remaining = qint64((spec.value("seconds").toDouble(60.0) - posSec) * 1000.0);
    if (remaining < 0) remaining = 0;
    const qint64 s = remaining / 1000;
    QString text = s >= 3600 ? QString("%1:%2:%3").arg(s / 3600).arg((s / 60) % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'))
                             : QString("%1:%2").arg((s / 60) % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
    if (!spec.value("label").toString().isEmpty()) text = spec["label"].toString() + "  " + text;
    return text;
}

void TemplateSource::close() {
    m_frame.reset(); m_picture = QImage(); m_open = false;
    m_ticker.reset(); m_clockText.clear(); m_tiles = 0;
}

bool TemplateSource::open(const OutputFormat& fmt) {
    m_fmt = fmt;
    const QString kind = m_spec.value("kind").toString("lower_third");
    // Caption kinds are "animated" (their picture depends on time) but video() short-circuits
    // on an unchanged state key, so they cost a render only when a word/line/row changes.
    m_animated = (kind == "ticker" || kind == "clock" || kind == "countdown" || kind == "karaoke" ||
                  (kind == "factbox" && m_spec.value("reveal").toString("all") == "lines"));
    const QString logo = m_spec.value("image").toString();
    if (!logo.isEmpty() && kind != "factbox") m_logo = QImage(logo);
    m_dirty = true;
    m_lastKey = -2; m_lastLine = -1; m_lineChangeT = -1e9; m_kkeyValid = false; m_blank = false;
    // open() runs on the loader thread (never the clock thread): the caption kinds do their disk
    // access and their layout here, so video() only paints.
    if (kind == "factbox") loadFactboxPicture();
    if (kind == "karaoke") { m_karaoke = karaokeLayout(m_spec, &m_fmt); m_dirty = false; m_kWords = int(m_karaoke.words.size()); m_kLines = int(m_karaoke.lines.size()); }
    m_ticker.reset(); m_clockText.clear(); m_tiles = 0;
    m_cacheState = !m_cacheOn ? CacheOff : (kind == "clock" || kind == "countdown") ? CacheString : CachePending;
    m_open = true;
    return true;
}

void TemplateSource::update(const QJsonObject& params) {
    for (auto it = params.begin(); it != params.end(); ++it) m_spec[it.key()] = it.value();
    if (params.contains("image") && m_spec.value("kind").toString() != "factbox") m_logo = QImage(params["image"].toString());
    const QString kind = m_spec.value("kind").toString("lower_third");
    if (kind == "factbox") {
        m_animated = m_spec.value("reveal").toString("all") == "lines";
        // Decode again only when the picture or the panel width changed (reveal steps and text edits reuse it).
        if (params.contains("image") || params.contains("width")) loadFactboxPicture();
    }
    // PE-07: the ticker strip is rebuilt by the next video() (m_dirty); a kind that is no ticker any more drops it.
    if (kind != "ticker") { m_ticker.reset(); m_tiles = 0; }
    m_cacheState = !m_cacheOn ? CacheOff : (kind == "clock" || kind == "countdown") ? CacheString : CachePending;
    m_dirty = true;
}

void TemplateSource::loadFactboxPicture() {
    const QString key = m_spec.value("image").toString() + "|" + QString::number(m_spec.value("width").toInt(520)) + "|" + QString::number(m_fmt.width) + "x" + QString::number(m_fmt.height);
    if (key == m_pictureKey && (!m_picture.isNull() || m_spec.value("image").toString().isEmpty())) return;
    m_picture = factboxPicture(m_spec, m_fmt);
    m_pictureKey = key;
}

QRect TemplateSource::contentRect() const { return m_spec.value("kind").toString() == "karaoke" ? m_content : QRect(); }
bool TemplateSource::isBlank() const { return m_blank; }

// Every kind is painted into a Format_ARGB32_Premultiplied QImage. Straight (the default) converts it to straight ARGB32
// (a pass over every pixel, and QPainter or the GPU shader premultiplies it again when the layer is drawn); PE-08
// premultiplied copies the painter's pixels unchanged.
FramePtr TemplateSource::toFrame(const QImage& img) const {
    return m_premul ? Mixer::imageToFramePremultiplied(img) : Mixer::imageToFrame(img);
}

QJsonObject TemplateSource::status() const {
    // Runs on the compositor's reader threads while video() runs on the clock thread: only m_spec (written by update(),
    // which the compositor never runs during status()) and the atomics below, never the members video() works with.
    const QString kind = m_spec.value("kind").toString();
    QJsonObject o = LayerSource::status(); o["kind"] = kind;
    if (m_premul) o["premultiplied"] = true;           // only when on: the default status JSON is unchanged
    if (kind == "karaoke") { o["words"] = m_kWords.load(); o["lines"] = m_kLines.load(); o["line"] = m_lastLine.load(); o["programTime"] = wantsProgramTime(); o["blank"] = m_blank.load(); }
    if (kind == "ticker" || kind == "clock" || kind == "countdown") {
        // PE-07: strip | string | off (PLAYPLAY_TEMPLATE_CACHE=0) | per-frame: <why the strip would not be exact>
        const int state = m_cacheState.load();
        o["cache"] = cacheStateName(state);
        o["paints"] = double(m_paints.load());
        if (state == CacheStrip) o["tiles"] = m_tiles.load();
    }
    return o;
}

bool TemplateSource::wantsProgramTime() const {
    return m_spec.value("kind").toString() == "karaoke" && m_spec.value("timeBase").toString("program") == "program";
}

FramePtr TemplateSource::video(qint64, double posSec) {
    if (!m_open) return nullptr;
    const QString kind = m_spec.value("kind").toString("lower_third");

    if (kind == "karaoke") {
        // Only reached again after update() (open() did the layout): re-break the lines.
        if (m_dirty) { m_karaoke = karaokeLayout(m_spec, &m_fmt); m_kkeyValid = false; m_lastLine = -1; m_dirty = false; m_kWords = int(m_karaoke.words.size()); m_kLines = int(m_karaoke.lines.size()); }
        const double t = karaokeTime(m_spec, posSec, wantsProgramTime());
        const int li = karaokeLineAt(m_karaoke, m_spec, t);
        m_size = QSize(m_fmt.width, m_fmt.height);        // the canvas: position is honoured inside it
        if (li < 0) {
            // Nothing on screen: say so (the compositor skips the layer) instead of delivering a transparent raster.
            m_blank = true; m_kkeyValid = false; m_lastLine = -1;
            return nullptr;
        }
        if (li != m_lastLine) { m_lastLine = li; m_lineChangeT = t; }
        // State key: what the picture depends on. Line and fade step always; the current word only in
        // word mode (colours per word), the fill step only while the sliding fill is on.
        KaraokeKey key; key.line = li;
        if (m_spec.value("mode").toString("word") == "word") {
            const KLine& L = m_karaoke.lines[size_t(li)];
            int wi = -1, fillStep = 0;
            for (int i = L.first; i <= L.last; ++i) { const KWord& w = m_karaoke.words[size_t(i)]; if (t >= w.start && t < w.end) { wi = i; fillStep = int(qBound(0.0, (t - w.start) / qMax(0.01, w.end - w.start), 0.999) * kKaraokeFillSteps); break; } if (t >= w.end) wi = -2 - i; }
            key.word = wi;
            key.step = m_spec.value("fill").toBool(true) ? fillStep : 0;
        }
        const double fadeSec = m_spec.value("fadeSec").toDouble(0.2);
        const double fade = fadeSec > 0.0 ? qBound(0.0, (t - m_lineChangeT) / fadeSec, 1.0) : 1.0;
        key.fade = fade >= 1.0 ? 8 : int(fade * 8);
        m_blank = false;
        if (m_kkeyValid && key == m_kkey && m_frame) return m_frame;
        QRect box; QImage img = renderKaraokeBox(m_spec, m_fmt, m_karaoke, li, t, 0.25 + 0.75 * fade, &box);
        if (img.isNull()) { m_blank = true; m_kkeyValid = false; return nullptr; }     // box entirely outside the raster
        m_content = box; m_frame = toFrame(img); m_kkey = key; m_kkeyValid = true;
        return m_frame;
    }
    if (kind == "factbox") {
        const int rows = factboxVisibleRows(m_spec, posSec);
        if (!m_dirty && rows == int(m_lastKey) && m_frame) return m_frame;
        QSize sz; QImage img = renderFactbox(m_spec, m_fmt, rows, &sz, &m_picture);
        m_size = sz; m_frame = toFrame(img); m_lastKey = rows; m_dirty = false;
        return m_frame;
    }

    // PE-07 render cache (PLAYPLAY_TEMPLATE_CACHE=0: the per-frame render below, as before).
    if (m_cacheOn && kind == "ticker") {
        if (m_dirty || !m_ticker) { buildTicker(); m_dirty = false; }
        if (m_ticker->strip) { m_frame = tickerFrame(posSec); return m_frame; }
        // a guard said the strip would not be exact for this text / font: paint every frame below
    } else if (m_cacheOn && (kind == "clock" || kind == "countdown")) {
        // The picture is a function of its string: paint only when the string changes (once a second).
        const QString text = clockText(m_spec, posSec);
        if (m_dirty || !m_frame || text != m_clockText) {
            const QImage img = clockPicture(m_spec, m_fmt, text);
            m_size = img.size(); m_frame = toFrame(img); m_clockText = text; m_dirty = false;
            ++m_paints;
        }
        return m_frame;
    }

    if (m_dirty || m_animated) {
        QSize sz;
        QImage img = render(m_spec, m_fmt, posSec, &sz);
        m_size = sz;
        m_frame = toFrame(img);
        m_dirty = false;
        ++m_paints;
    }
    return m_frame;
}

// ---------------------------------------------------------------------------
// TemplateSource — PE-07 ticker strip
// ---------------------------------------------------------------------------
// render() paints the background, the label and every copy of the crawl text (clipped to the crawl area) each frame:
// ~2 ms of QPainter on the clock thread at 1080p, most of it shaping and drawing the same text again. Here the
// background + label (the base) and one period of the crawl (a tile: background + one copy of the text) are painted
// once per spec; a frame is the base with, for every copy render() would draw, the copy's slice of the tile at the
// pixel QPainter would have put it (textPixelX of the same x). Copies sit `period` apart (one pixel less when
// QFixed's truncation moves a copy left of zero) and each one's glyphs stay inside its own tile, so neighbouring
// slices meet on background and the frame equals render()'s picture bit for bit. Converted to straight alpha (when
// not PE-08 premultiplied) once per tile instead of once per frame; the conversion works pixel by pixel.

void TemplateSource::buildTicker() {
    auto c = std::make_unique<TickerCache>();
    c->g = tickerGeom(m_spec, m_fmt);
    const TickerGeom& g = c->g;
    m_size = QSize(g.w, g.h);
    c->period = g.textW + g.gap;
    c->half = g.gap / 2;
    QImage dev(1, 1, QImage::Format_ARGB32_Premultiplied);       // metrics of the device the text is drawn on
    int state = CacheStrip;
    if (g.w <= 0 || g.h <= 0 || g.labelW >= g.w) state = CachePerFrameEmpty;
    else if (g.text.contains(QLatin1Char('\n')) || g.text.contains(QChar::LineSeparator) || g.text.contains(QChar::ParagraphSeparator)) state = CachePerFrameLines;
    else {
        const Ink ink = measureInk(g);
        // Every copy's glyphs stay inside its own tile with two columns of background to spare: two neighbouring copies
        // may sit one pixel closer than a period (textPixelX of a negative x), never further apart.
        if (c->half + ink.left < 2 || c->half + ink.right > c->period - 3) state = CachePerFrameNarrow;
        // A line taller than the ticker makes drawText clip to its rect, whose edges may round differently from the
        // glyphs: only when no ink comes within a pixel of the rect's ends (measured with that clip) is it the same.
        else if (QFontMetricsF(g.font, &dev).height() > g.h && (ink.left < 1 || ink.right > g.textW + 8)) state = CachePerFrameClip;
        else if (!textOnWholePixels(g)) state = CachePerFrameSubpixel;
    }
    if (state == CacheStrip) {
        c->strip = true;
        c->chunkW = c->period <= kTickerTileMax ? c->period : kTickerChunk;
        const size_t chunks = size_t((c->period + c->chunkW - 1) / c->chunkW);
        c->tiles.assign(chunks, QImage());
        c->tileUsed.assign(chunks, -1);
        QImage base(g.w, g.h, QImage::Format_ARGB32_Premultiplied); base.fill(Qt::transparent);
        QPainter p(&base); p.setRenderHint(QPainter::TextAntialiasing);
        paintTickerBase(p, g);
        p.end();
        c->base = m_premul ? base : base.convertToFormat(QImage::Format_ARGB32);
        c->pool = std::make_unique<FramePool>(g.w, g.h, kMixerPixelFormat);
        ++m_paints;
    }
    m_ticker = std::move(c);
    m_tiles = 0;
    m_cacheState = state;
}

QImage TemplateSource::tickerTile(int chunk) const {
    const TickerCache& c = *m_ticker;
    const TickerGeom& g = c.g;
    const int x0 = chunk * c.chunkW, cw = qMin(c.chunkW, c.period - x0);
    QImage img(cw, g.h, QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
    QPainter p(&img); p.setRenderHint(QPainter::TextAntialiasing);
    p.fillRect(QRect(0, 0, cw, g.h), g.bg);
    p.setFont(g.font); p.setPen(g.fg);
    drawTickerText(p, g, double(c.half - x0));                   // the copy at tile column `half`, moved by whole pixels
    p.end();
    return m_premul ? img : img.convertToFormat(QImage::Format_ARGB32);
}

FramePtr TemplateSource::tickerFrame(double posSec) {
    TickerCache& c = *m_ticker;
    const TickerGeom& g = c.g;
    c.xs.clear();
    forEachTickerCopy(g, posSec, [&](double x) { c.xs.push_back(textPixelX(x)); });
    if (m_frame && c.xs == c.lastX) return m_frame;               // not moved by a whole pixel: the same picture
    ++c.serial;
    // The crawl area [labelW, w) is covered by the copies' zones: copy X shows tile column t at raster column X - half + t.
    c.pieces.clear();
    for (const int X : c.xs) {
        const int z0 = X - c.half;
        for (int a = qMax(g.labelW, z0), b = qMin(g.w, z0 + c.period); a < b;) {
            const int t = a - z0, j = t / c.chunkW;
            const int len = qMin(b - a, (j + 1) * c.chunkW - t);
            c.pieces.push_back({a, j, t - j * c.chunkW, len});
            a += len;
        }
    }
    for (const TickerCache::Piece& pc : c.pieces) {
        QImage& tile = c.tiles[size_t(pc.chunk)];
        if (tile.isNull()) { tile = tickerTile(pc.chunk); ++m_paints; }
        c.tileUsed[size_t(pc.chunk)] = c.serial;
    }
    int held = 0;
    for (const QImage& t : c.tiles) held += t.isNull() ? 0 : 1;
    while (held > kTickerTilesKept) {                            // a long crawl: drop the chunk used least recently
        int drop = -1;
        for (size_t j = 0; j < c.tiles.size(); ++j)
            if (!c.tiles[j].isNull() && c.tileUsed[j] != c.serial && (drop < 0 || c.tileUsed[j] < c.tileUsed[size_t(drop)])) drop = int(j);
        if (drop < 0) break;
        c.tiles[size_t(drop)] = QImage(); --held;
    }
    m_tiles = held;
    FramePtr f = c.pool->get();
    if (!f) return m_frame;                                      // out of memory: repeat the last picture
    const size_t rowBytes = size_t(g.w) * 4;
    for (int y = 0; y < g.h; ++y) {
        uint8_t* dst = f->data[0] + size_t(y) * size_t(f->linesize[0]);
        memcpy(dst, c.base.constScanLine(y), rowBytes);          // label, and background where no copy's zone reaches
        for (const TickerCache::Piece& pc : c.pieces)
            memcpy(dst + size_t(pc.dst) * 4, c.tiles[size_t(pc.chunk)].constScanLine(y) + size_t(pc.src) * 4, size_t(pc.len) * 4);
    }
    c.lastX.swap(c.xs);
    return f;
}

QImage TemplateSource::render(const QJsonObject& spec, const OutputFormat& fmt, double posSec, QSize* outSize) {
    const QString kind = spec.value("kind").toString("lower_third");
    const QColor accent = colorOr(spec, "accentColor", QColor(0, 212, 255));
    const QColor bg = colorOr(spec, "bgColor", QColor(5, 5, 10, 220));
    const QColor fg = colorOr(spec, "color", Qt::white);
    const int pad = spec.value("padding").toInt(18);
    const double radius = spec.value("radius").toDouble(6.0);
    QImage img;

    if (kind == "karaoke") {
        // Stateless entry (tests, previews): layout from the spec, program time = posSec.
        const KaraokeLayout L = karaokeLayout(spec, &fmt);
        return renderKaraoke(spec, fmt, L, karaokeTime(spec, posSec, spec.value("timeBase").toString("program") == "program"), 1.0, outSize);
    }
    if (kind == "factbox") return renderFactbox(spec, fmt, factboxVisibleRows(spec, posSec), outSize);

    if (kind == "lower_third") {
        const int w = spec.value("width").toInt(int(fmt.width * 0.5));
        const int h = spec.value("height").toInt(int(fmt.height * 0.11));
        img = QImage(w, h, QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
        QPainter p(&img); p.setRenderHint(QPainter::Antialiasing); p.setRenderHint(QPainter::TextAntialiasing);
        roundedRect(p, QRectF(0, 0, w, h), radius, bg);
        p.fillRect(QRectF(0, 0, qMax(6, h / 12), h), accent);
        const QString title = spec.value("title").toString(spec.value("text").toString());
        const QString subtitle = spec.value("subtitle").toString();
        p.setPen(fg);
        p.setFont(fontFor(spec, h * (subtitle.isEmpty() ? 0.42 : 0.36)));
        const int left = h / 6 + pad;
        if (subtitle.isEmpty()) p.drawText(QRect(left, 0, w - left - pad, h), Qt::AlignVCenter | Qt::AlignLeft, title);
        else {
            p.drawText(QRect(left, int(h * 0.08), w - left - pad, int(h * 0.5)), Qt::AlignVCenter | Qt::AlignLeft, title);
            QFont f2 = fontFor(spec, int(h * 0.26), QFont::Normal); p.setFont(f2); p.setPen(colorOr(spec, "subtitleColor", QColor(200, 200, 220)));
            p.drawText(QRect(left, int(h * 0.56), w - left - pad, int(h * 0.38)), Qt::AlignVCenter | Qt::AlignLeft, subtitle);
        }
    } else if (kind == "bug") {
        const QString logo = spec.value("image").toString();
        QImage li = logo.isEmpty() ? QImage() : QImage(logo);
        const int h = spec.value("height").toInt(int(fmt.height * 0.08));
        if (!li.isNull()) {
            li = li.scaledToHeight(h, Qt::SmoothTransformation);
            img = li.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            const double op = spec.value("opacity").toDouble(1.0);
            if (op < 1.0) { QImage o(img.size(), QImage::Format_ARGB32_Premultiplied); o.fill(Qt::transparent); QPainter p(&o); p.setOpacity(op); p.drawImage(0, 0, img); img = o; }
        } else {
            const QString text = spec.value("text").toString(spec.value("title").toString("PLAY PLAY"));
            QFont f = fontFor(spec, int(h * 0.55));
            QFontMetrics fm(f);
            const int w = fm.horizontalAdvance(text) + pad * 2;
            img = QImage(w, h, QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
            QPainter p(&img); p.setRenderHint(QPainter::Antialiasing); p.setRenderHint(QPainter::TextAntialiasing);
            roundedRect(p, QRectF(0, 0, w, h), radius, bg);
            p.setFont(f); p.setPen(fg); p.drawText(QRect(0, 0, w, h), Qt::AlignCenter, text);
        }
    } else if (kind == "ticker") {
        // Per frame: background, label, then every copy of the crawl clipped to the crawl area (the PE-07 strip in
        // video() delivers the same picture from pixels painted once).
        const TickerGeom g = tickerGeom(spec, fmt);
        img = QImage(g.w, g.h, QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
        QPainter p(&img); p.setRenderHint(QPainter::TextAntialiasing);
        paintTickerBase(p, g);
        p.setClipRect(QRect(g.labelW, 0, g.w - g.labelW, g.h));
        p.setFont(g.font); p.setPen(g.fg);
        forEachTickerCopy(g, posSec, [&](double x) { drawTickerText(p, g, x); });
    } else if (kind == "clock" || kind == "countdown") {
        img = clockPicture(spec, fmt, clockText(spec, posSec));
    } else if (kind == "box") {
        const int w = spec.value("width").toInt(fmt.width / 4), h = spec.value("height").toInt(fmt.height / 4);
        img = QImage(w, h, QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
        QPainter p(&img); p.setRenderHint(QPainter::Antialiasing);
        roundedRect(p, QRectF(0, 0, w, h), radius, colorOr(spec, "color", bg));
    } else { // text
        const QString text = spec.value("text").toString(spec.value("title").toString());
        QFont f = fontFor(spec, spec.value("fontSize").toInt(int(fmt.height * 0.05)));
        QFontMetrics fm(f);
        const QRect br = fm.boundingRect(QRect(0, 0, spec.value("width").toInt(fmt.width - pad * 2), fmt.height), Qt::TextWordWrap | Qt::AlignLeft, text);
        const int w = spec.value("width").toInt(br.width() + pad * 2), h = spec.value("height").toInt(br.height() + pad * 2);
        img = QImage(w, h, QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
        QPainter p(&img); p.setRenderHint(QPainter::TextAntialiasing);
        if (spec.value("bgColor").isString()) roundedRect(p, QRectF(0, 0, w, h), radius, bg);
        p.setFont(f); p.setPen(fg);
        const QString align = spec.value("align").toString("left");
        p.drawText(QRect(pad, pad, w - pad * 2, h - pad * 2), Qt::TextWordWrap | (align == "center" ? Qt::AlignHCenter : align == "right" ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter, text);
    }
    if (outSize) *outSize = img.size();
    return img;
}

// ---------------------------------------------------------------------------
// TemplateSource — karaoke captions
// ---------------------------------------------------------------------------

namespace {
// Caption font and box geometry shared by the line breaker and the renderer (sizes are 1080p values
// scaled by raster width / 1920; the box is capped at the title-safe width, 5 % margins).
QFont karaokeFont(const QJsonObject& spec, const OutputFormat& fmt) {
    QFont f(spec.value("fontFamily").toString("Segoe UI"), -1, QFont::Bold);
    f.setPixelSize(qMax(10, int(spec.value("fontSize").toInt(46) * (fmt.width / 1920.0))));
    return f;
}
int karaokePad(const QJsonObject& spec, const OutputFormat& fmt) { return int(spec.value("padding").toInt(22) * (fmt.width / 1920.0)); }
int karaokeSafeX(const OutputFormat& fmt) { return int(fmt.width * 0.05); }
} // namespace

TemplateSource::KaraokeLayout TemplateSource::karaokeLayout(const QJsonObject& spec, const OutputFormat* fmt) {
    KaraokeLayout L;
    const int maxChars = qMax(8, spec.value("maxChars").toInt(48));
    const double maxGap = spec.value("lineGapSec").toDouble(1.5);
    const bool breakOnSegments = spec.value("breakOnSegments").toBool(true);
    auto addWord = [&](const QJsonObject& w, int seg) {
        const QString txt = w.value("word").toString(w.value("text").toString()).trimmed();
        if (txt.isEmpty()) return;
        KWord k; k.text = txt; k.start = w.value("start").toDouble(); k.end = qMax(k.start + 0.05, w.value("end").toDouble(k.start + 0.3)); k.segment = seg;
        L.words.push_back(k);
    };
    const QJsonArray segs = spec.value("segments").toArray();
    if (!segs.isEmpty()) {
        int si = 0;
        for (const QJsonValue& sv : segs) {
            const QJsonObject s = sv.toObject();
            const QJsonArray ws = s.value("words").toArray();
            if (ws.isEmpty()) {
                // No word timing: spread the segment text evenly over its duration (by character count).
                const QStringList toks = s.value("text").toString().simplified().split(' ', Qt::SkipEmptyParts);
                const double s0 = s.value("start").toDouble(), s1 = qMax(s0 + 0.2, s.value("end").toDouble());
                int total = 0; for (const QString& t : toks) total += t.size() + 1;
                double at = s0;
                for (const QString& t : toks) { KWord k; k.text = t; k.start = at; at += (s1 - s0) * double(t.size() + 1) / qMax(1, total); k.end = at; k.segment = si; L.words.push_back(k); }
            } else {
                for (const QJsonValue& wv : ws) addWord(wv.toObject(), si);
            }
            ++si;
        }
    } else {
        for (const QJsonValue& wv : spec.value("words").toArray()) addWord(wv.toObject(), -1);
    }
    // Lines: break at segment boundaries, long silences, the character budget and (with a raster)
    // the measured width of the caption box, so a large font never runs past the title-safe margins.
    std::unique_ptr<QFontMetrics> fm; int gapPx = 0, innerPx = 0, linePx = 0;
    if (fmt) { fm = std::make_unique<QFontMetrics>(karaokeFont(spec, *fmt)); gapPx = fm->horizontalAdvance(' '); innerPx = fmt->width - karaokeSafeX(*fmt) * 2 - karaokePad(spec, *fmt) * 2; }
    int chars = 0; KLine cur;
    for (size_t i = 0; i < L.words.size(); ++i) {
        const KWord& w = L.words[i];
        const int wordPx = fm ? fm->horizontalAdvance(w.text) : 0;
        bool brk = cur.first < 0;
        if (!brk) {
            const KWord& prev = L.words[size_t(cur.last)];
            brk = (breakOnSegments && w.segment >= 0 && w.segment != prev.segment) || (w.start - prev.end > maxGap) || (chars + 1 + w.text.size() > maxChars)
                  || (fm && linePx + gapPx + wordPx > innerPx);
        }
        if (brk) { if (cur.first >= 0) L.lines.push_back(cur); cur = KLine{int(i), int(i), w.start, w.end}; chars = w.text.size(); linePx = wordPx; }
        else { cur.last = int(i); cur.end = qMax(cur.end, w.end); chars += 1 + w.text.size(); linePx += gapPx + wordPx; }
    }
    if (cur.first >= 0) L.lines.push_back(cur);
    for (size_t li = 0; li < L.lines.size(); ++li) for (int i = L.lines[li].first; i <= L.lines[li].last; ++i) L.words[size_t(i)].line = int(li);
    return L;
}

double TemplateSource::karaokeTime(const QJsonObject& spec, double posSec, bool programTime) {
    // Transcript words are file-relative; the engine's item position starts at the item's in point.
    //   program time base: t = programPos + inPointSec + syncOffsetSec
    //   layer time base:   t = layerPos + startAtProgramSec + inPointSec + syncOffsetSec
    double t = posSec + spec.value("inPointSec").toDouble(0.0) + spec.value("syncOffsetSec").toDouble(0.0);
    if (!programTime) t += spec.value("startAtProgramSec").toDouble(spec.value("startOffset").toDouble(0.0));
    return t;
}

int TemplateSource::karaokeLineAt(const KaraokeLayout& L, const QJsonObject& spec, double t) {
    if (L.lines.empty()) return -1;
    const double lead = spec.value("leadSec").toDouble(0.25), tail = spec.value("tailSec").toDouble(1.2);
    if (t < L.lines.front().start - lead) return -1;
    // A line becomes the live one `lead` before its first word, but never before the previous line has
    // finished: in contiguous speech the pre-roll must not cut the last word of the line being spoken
    // (and never later than the new line's own first word, for overlapping word times).
    int li = 0;
    for (size_t i = 1; i < L.lines.size(); ++i) {
        const double switchAt = qMin(L.lines[i].start, qMax(L.lines[i].start - lead, L.lines[i - 1].end));
        if (switchAt <= t) li = int(i); else break;
    }
    // The live line stays up for `tail` after its last word; then nothing until the next line's pre-roll.
    return t <= L.lines[size_t(li)].end + tail ? li : -1;
}

QImage TemplateSource::renderKaraoke(const QJsonObject& spec, const OutputFormat& fmt, const KaraokeLayout& L, double t, double alpha, QSize* outSize) {
    // Full raster for tests and previews: the caption box composited where `position` puts it.
    QImage img(fmt.width, fmt.height, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    if (outSize) *outSize = img.size();
    QRect box;
    const QImage b = renderKaraokeBox(spec, fmt, L, karaokeLineAt(L, spec, t), t, alpha, &box);
    if (b.isNull()) return img;
    QPainter p(&img);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(box.topLeft(), b);
    p.end();
    return img;
}

QImage TemplateSource::renderKaraokeBox(const QJsonObject& spec, const OutputFormat& fmt, const KaraokeLayout& L, int li, double t, double alpha, QRect* boxOut) {
    if (boxOut) *boxOut = QRect();
    if (li < 0 || li >= int(L.lines.size())) return QImage();
    const double sc = fmt.width / 1920.0;
    const QString mode = spec.value("mode").toString("word");
    const bool fill = spec.value("fill").toBool(true);
    const int maxLines = qBound(1, spec.value("maxLines").toInt(2), 4);
    const QColor fg = colorOr(spec, "color", QColor(235, 235, 245));
    const QColor hi = colorOr(spec, "highlightColor", QColor(255, 214, 10));
    const QColor past = colorOr(spec, "pastColor", QColor(255, 255, 255));
    const QColor bg = colorOr(spec, "bgColor", QColor(5, 5, 10, 190));
    const QColor nextColor = colorOr(spec, "nextColor", QColor(fg.red(), fg.green(), fg.blue(), 150));
    const QFont f = karaokeFont(spec, fmt);
    const QFontMetrics fm(f);
    const int lineH = int(fm.height() * 1.18);
    const int pad = karaokePad(spec, fmt);
    const int gapW = fm.horizontalAdvance(' ');
    const double radius = spec.value("radius").toDouble(10.0) * sc;

    // Lines on screen: the current one plus up to maxLines-1 following lines (preview).
    std::vector<int> shown; for (int i = li; i < int(L.lines.size()) && int(shown.size()) < maxLines; ++i) shown.push_back(i);
    int maxW = 0;
    std::vector<int> widths;
    for (int idx : shown) { const KLine& ln = L.lines[size_t(idx)]; int w = 0; for (int i = ln.first; i <= ln.last; ++i) { w += fm.horizontalAdvance(L.words[size_t(i)].text); if (i < ln.last) w += gapW; } widths.push_back(w); maxW = qMax(maxW, w); }
    const int safeX = karaokeSafeX(fmt), safeY = int(fmt.height * 0.05);
    const int boxW = qMin(fmt.width - safeX * 2, maxW + pad * 2);
    const int boxH = int(shown.size()) * lineH + pad * 2 - int(lineH - fm.height());
    const QString align = spec.value("align").toString("center");
    const int boxX = align == "left" ? safeX : align == "right" ? fmt.width - safeX - boxW : (fmt.width - boxW) / 2;
    const QString position = spec.value("position").toString("bottom");
    int boxY = fmt.height - safeY - boxH;                                              // bottom
    if (position == "top") boxY = safeY;
    else if (position == "lower_third") boxY = fmt.height - safeY - boxH - int(fmt.height * 0.07);
    else if (position == "center") boxY = (fmt.height - boxH) / 2;
    if (spec.contains("y")) boxY = int(spec.value("y").toDouble() * sc);

    // Only the box is rendered (and handed to the compositor): the part of it that lies inside the raster.
    const QRect boxRect(boxX, boxY, boxW, boxH);
    const QRect visible = boxRect.intersected(QRect(0, 0, fmt.width, fmt.height));
    if (visible.isEmpty()) return QImage();
    if (boxOut) *boxOut = visible;
    QImage img(visible.size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing); p.setRenderHint(QPainter::TextAntialiasing);
    p.translate(-visible.topLeft());                      // the code below works in raster coordinates
    p.setClipRect(boxRect);                               // a word wider than the box is clipped, never drawn past the margins
    p.setOpacity(qBound(0.0, alpha, 1.0));
    if (bg.alpha() > 0) roundedRect(p, QRectF(boxX, boxY, boxW, boxH), radius, bg);
    p.setFont(f);
    const bool shadow = spec.value("textShadow").toBool(true);
    int y = boxY + pad;
    for (size_t s = 0; s < shown.size(); ++s) {
        const KLine& ln = L.lines[size_t(shown[s])];
        const bool isCurrent = (s == 0);
        int x = align == "left" ? boxX + pad : align == "right" ? boxX + boxW - pad - widths[s] : boxX + (boxW - widths[s]) / 2;
        const QRect lineRect(0, y, 0, fm.height());
        for (int i = ln.first; i <= ln.last; ++i) {
            const KWord& w = L.words[size_t(i)];
            const int ww = fm.horizontalAdvance(w.text);
            const QRect r(x, y, ww + 2, fm.height());
            QColor c = isCurrent ? fg : nextColor;
            double prog = -1.0;      // >= 0: current word, fraction spoken
            if (isCurrent && mode == "word") {
                if (t >= w.end) c = past;
                else if (t >= w.start) { prog = qBound(0.0, (t - w.start) / qMax(0.01, w.end - w.start), 1.0); c = fill ? fg : hi; }
            } else if (isCurrent && mode == "line") {
                c = hi;
            }
            if (shadow) { p.setPen(QColor(0, 0, 0, 160)); p.drawText(r.translated(int(2 * sc), int(2 * sc)), Qt::AlignLeft | Qt::AlignVCenter, w.text); }
            p.setPen(c); p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, w.text);
            if (prog >= 0.0 && fill) {
                // Sliding fill: the highlight colour sweeps left-to-right through the current word.
                p.save(); p.setClipRect(QRect(x, y, int(ww * prog) + 1, fm.height()), Qt::IntersectClip);
                p.setPen(hi); p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, w.text);
                p.restore();
            }
            x += ww + gapW;
        }
        Q_UNUSED(lineRect);
        y += lineH;
    }
    p.end();
    return img;
}

// ---------------------------------------------------------------------------
// TemplateSource — fact box
// ---------------------------------------------------------------------------

namespace {
// The bullet rows of a fact box: `lines` as an array (strings or {text}) or as one newline-separated
// string. renderFactbox and factboxRowCount both read it here, so the reveal count always matches
// what is drawn (a string used to count as 0 rows and the panel body stayed empty).
QStringList factboxLinesOf(const QJsonObject& spec) {
    QStringList lines;
    const QJsonValue lv = spec.value("lines");
    if (lv.isString()) { for (const QString& l : lv.toString().split('\n', Qt::SkipEmptyParts)) { const QString s = l.trimmed(); if (!s.isEmpty()) lines << s; } }
    else for (const QJsonValue& v : lv.toArray()) lines << (v.isString() ? v.toString() : v.toObject().value("text").toString());
    return lines;
}
} // namespace

int TemplateSource::factboxRowCount(const QJsonObject& spec) {
    const int items = spec.value("items").toArray().size();
    return items > 0 ? items : int(factboxLinesOf(spec).size());
}

QImage TemplateSource::factboxPicture(const QJsonObject& spec, const OutputFormat& fmt) {
    const QString imgPath = spec.value("image").toString();
    if (imgPath.isEmpty()) return QImage();
    // Image under the header (path or file URL), scaled to the panel width and capped at 40 % of the raster height.
    const int width = qMax(120, int(spec.value("width").toInt(520) * (fmt.width / 1920.0)));
    QImage picture(imgPath.startsWith("file:") ? QUrl(imgPath).toLocalFile() : imgPath);
    if (picture.isNull()) return picture;
    picture = picture.scaledToWidth(width, Qt::SmoothTransformation);
    if (picture.height() > fmt.height * 0.4) picture = picture.scaledToHeight(int(fmt.height * 0.4), Qt::SmoothTransformation);
    return picture;
}

int TemplateSource::factboxVisibleRows(const QJsonObject& spec, double posSec) {
    const int n = factboxRowCount(spec);
    if (spec.value("reveal").toString("all") != "lines") return n;
    const double iv = qMax(0.1, spec.value("revealIntervalSec").toDouble(1.2));
    const double delay = spec.value("revealDelaySec").toDouble(0.4);
    return qBound(0, 1 + int(std::floor((posSec - delay) / iv)), n);
}

QImage TemplateSource::renderFactbox(const QJsonObject& spec, const OutputFormat& fmt, int visibleRows, QSize* outSize, const QImage* pictureIn) {
    const double sc = fmt.width / 1920.0;
    const QColor accent = colorOr(spec, "accentColor", QColor(0, 212, 255));
    const QColor bg = colorOr(spec, "bgColor", QColor(10, 12, 20, 232));
    const QColor fg = colorOr(spec, "color", Qt::white);
    const QColor titleColor = colorOr(spec, "titleColor", QColor(5, 5, 10));
    const QColor labelColor = colorOr(spec, "labelColor", QColor(170, 180, 200));
    const int fontPx = qMax(10, int(spec.value("fontSize").toInt(28) * sc));
    const int width = qMax(120, int(spec.value("width").toInt(520) * sc));
    const int pad = int(spec.value("padding").toInt(22) * sc);
    const double radius = spec.value("radius").toDouble(10.0) * sc;
    const bool shadow = spec.value("shadow").toBool(true);
    const int shadowM = shadow ? int(18 * sc) : 0;
    const QString family = spec.value("fontFamily").toString("Segoe UI");
    QFont rowF(family, -1, QFont::Normal); rowF.setPixelSize(fontPx);
    QFont labelF(family, -1, QFont::DemiBold); labelF.setPixelSize(int(fontPx * 0.78));
    QFont titleF(family, -1, QFont::Bold); titleF.setPixelSize(int(fontPx * 1.15));
    QFont footF(family, -1, QFont::Normal); footF.setPixelSize(int(fontPx * 0.7));
    const QFontMetrics rfm(rowF), lfm(labelF), tfm(titleF), ffm(footF);

    const QString title = spec.value("title").toString();
    const QString icon = spec.value("icon").toString();
    const QString footer = spec.value("footer").toString();
    const QJsonArray items = spec.value("items").toArray();
    const QStringList lines = factboxLinesOf(spec);
    const bool useItems = !items.isEmpty();
    const int rows = useItems ? items.size() : int(lines.size());

    // Image under the header: the caller's pre-scaled copy (TemplateSource decodes it once, off the clock
    // thread) or, for one-off renders (tests, previews), decoded here.
    const QImage picture = pictureIn ? *pictureIn : factboxPicture(spec, fmt);

    // Measure: header, image, rows (word-wrapped), footer.
    const int headerH = title.isEmpty() ? int(8 * sc) : int(tfm.height() * 1.7);
    const int innerW = width - pad * 2;
    const int bulletW = useItems ? 0 : int(fontPx * 0.9);
    const int labelW = useItems ? int(innerW * 0.38) : 0;
    const int rowGap = int(fontPx * 0.45);
    std::vector<QRect> rowRects; std::vector<int> rowHeights;
    int rowsH = 0;
    for (int i = 0; i < rows; ++i) {
        int h;
        if (useItems) {
            const QJsonObject it = items[i].toObject();
            const int vh = rfm.boundingRect(QRect(0, 0, innerW - labelW - int(10 * sc), 4000), Qt::TextWordWrap, it.value("value").toVariant().toString()).height();
            const int lh = lfm.boundingRect(QRect(0, 0, labelW, 4000), Qt::TextWordWrap, it.value("label").toString()).height();
            h = qMax(vh, lh);
        } else {
            h = rfm.boundingRect(QRect(0, 0, innerW - bulletW, 4000), Qt::TextWordWrap, lines[i]).height();
        }
        h = qMax(h, rfm.height());
        rowHeights.push_back(h); rowsH += h + (i + 1 < rows ? rowGap : 0);
    }
    const int footerH = footer.isEmpty() ? 0 : int(ffm.height() * 1.6);
    const int picH = picture.isNull() ? 0 : picture.height();
    const int panelH = headerH + picH + (rows > 0 ? pad + rowsH : int(6 * sc)) + pad + footerH;
    const int W = width + shadowM * 2, H = panelH + shadowM * 2;
    QImage img(W, H, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing); p.setRenderHint(QPainter::TextAntialiasing); p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF panel(shadowM, shadowM, width, panelH);

    // Drop shadow: stacked translucent rounded rects approximate a soft blur without a filter pass.
    if (shadow) {
        for (int i = 6; i >= 1; --i) {
            const double grow = i * 2.2 * sc;
            roundedRect(p, panel.adjusted(-grow, -grow + 6 * sc, grow, grow + 8 * sc), radius + grow, QColor(0, 0, 0, 14));
        }
    }
    // Panel body, clipped so the header bar and picture follow the rounded corners.
    QPainterPath clip; clip.addRoundedRect(panel, radius, radius);
    p.save(); p.setClipPath(clip);
    p.fillPath(clip, bg);
    int y = int(panel.top());
    if (!title.isEmpty()) {
        p.fillRect(QRectF(panel.left(), y, width, headerH), accent);
        int tx = int(panel.left()) + pad;
        if (!icon.isEmpty()) { QFont iF = titleF; iF.setPixelSize(int(fontPx * 1.3)); p.setFont(iF); p.setPen(titleColor); const int iw = QFontMetrics(iF).horizontalAdvance(icon); p.drawText(QRect(tx, y, iw + 4, headerH), Qt::AlignVCenter | Qt::AlignLeft, icon); tx += iw + int(12 * sc); }
        p.setFont(titleF); p.setPen(titleColor);
        p.drawText(QRect(tx, y, int(panel.right()) - pad - tx, headerH), Qt::AlignVCenter | Qt::AlignLeft, tfm.elidedText(title, Qt::ElideRight, int(panel.right()) - pad - tx));
        y += headerH;
    } else {
        p.fillRect(QRectF(panel.left(), y, width, headerH), accent);     // thin accent strip
        y += headerH;
    }
    if (!picture.isNull()) { p.drawImage(QPointF(panel.left() + (width - picture.width()) / 2.0, y), picture); y += picH; }
    if (rows > 0) {
        y += pad;
        const int shownRows = qBound(0, visibleRows, rows);
        for (int i = 0; i < shownRows; ++i) {
            const int h = rowHeights[size_t(i)];
            const int left = int(panel.left()) + pad;
            if (useItems) {
                const QJsonObject it = items[i].toObject();
                p.setFont(labelF); p.setPen(labelColor);
                p.drawText(QRect(left, y, labelW, h), Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, it.value("label").toString().toUpper());
                p.setFont(rowF); p.setPen(fg);
                p.drawText(QRect(left + labelW + int(10 * sc), y, innerW - labelW - int(10 * sc), h), Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, it.value("value").toVariant().toString());
            } else {
                // Accent bullet + wrapped text.
                const double d = fontPx * 0.32;
                p.setPen(Qt::NoPen); p.setBrush(accent);
                p.drawEllipse(QRectF(left + d * 0.2, y + rfm.ascent() - d * 1.35, d, d));
                p.setFont(rowF); p.setPen(fg);
                p.drawText(QRect(left + bulletW, y, innerW - bulletW, h), Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, lines[i]);
            }
            // Separator under every row but the last visible one.
            if (i + 1 < shownRows) { p.setPen(QColor(255, 255, 255, 22)); p.drawLine(QPointF(left, y + h + rowGap / 2.0), QPointF(panel.right() - pad, y + h + rowGap / 2.0)); }
            y += h + rowGap;
        }
    }
    if (!footer.isEmpty()) {
        p.setFont(footF); p.setPen(QColor(fg.red(), fg.green(), fg.blue(), 150));
        p.drawText(QRect(int(panel.left()) + pad, int(panel.bottom()) - footerH, innerW, footerH), Qt::AlignVCenter | Qt::AlignLeft, ffm.elidedText(footer, Qt::ElideRight, innerW));
    }
    p.restore();
    // Hairline border
    p.setPen(QPen(QColor(255, 255, 255, 28), 1)); p.setBrush(Qt::NoBrush); p.drawPath(clip);
    p.end();
    if (outSize) *outSize = img.size();
    return img;
}
