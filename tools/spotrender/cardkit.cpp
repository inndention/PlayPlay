// The spot_card.py drawing primitives, see cardkit.h.
#include "cardkit.h"

#include "blur.h"

#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHash>
#include <QMutex>
#include <QPainter>
#include <QRawFont>

#include <algorithm>
#include <cmath>

namespace cardkit {
namespace {

// The script's tables (spot_card.py L48-49). Pillow also finds the bare DejaVu names in its own font folders; on Windows
// they are not there, so the loop goes on to the fallback.
const char* const kBold[] = {"C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/arialbd.ttf", "C:/Windows/Fonts/calibrib.ttf", "DejaVuSans-Bold.ttf"};
const char* const kRegular[] = {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/calibri.ttf", "DejaVuSans.ttf"};

// Metrics and painting both resolve fonts against a QImage (96 dpi), so a width measured is the width drawn whatever the
// screen's scaling. Pixel-sized fonts make the DPI moot, but this keeps the two from ever drifting apart.
const QPaintDevice* metricsDevice() {
    static const QImage dev(1, 1, QImage::Format_ARGB32_Premultiplied);
    return &dev;
}

// ImageFont.truetype's lookup: the path as given; when that fails, its base name in %WINDIR%\Fonts (Pillow walks that
// folder; it is flat on Windows), and a name without an extension as .ttf first, as Pillow prefers.
QString resolveFontFile(const QString& p) {
    if (p.isEmpty()) return {};
    if (QFileInfo(p).isFile()) return QFileInfo(p).absoluteFilePath();
    const QString base = QFileInfo(p).fileName();
    if (base.isEmpty()) return {};
    const QDir fonts(QDir(qEnvironmentVariable("WINDIR", QStringLiteral("C:/Windows"))).filePath(QStringLiteral("Fonts")));
    QStringList names{base};
    if (QFileInfo(base).suffix().isEmpty()) names = {base + ".ttf", base + ".otf", base + ".ttc"};
    for (const QString& n : names)
        if (QFileInfo(fonts.filePath(n)).isFile()) return fonts.filePath(n);
    return {};
}

struct FileFace { bool ok = false; QString family, style; int weight = QFont::Normal; bool italic = false; };

FileFace fileFace(const QString& file) {
    static QMutex mu;
    static QHash<QString, FileFace> cache;
    const QString key = QDir::cleanPath(file).toLower();
    QMutexLocker lock(&mu);
    if (auto it = cache.constFind(key); it != cache.constEnd()) return *it;
    FileFace f;
    const int id = QFontDatabase::addApplicationFont(file);
    const QStringList families = id >= 0 ? QFontDatabase::applicationFontFamilies(id) : QStringList();
    const QRawFont raw(file, 16);                              // the face's own style name and weight
    if (!families.isEmpty()) {
        f.ok = true;
        f.family = families.first();
        if (raw.isValid()) { f.style = raw.styleName(); f.weight = raw.weight(); f.italic = raw.style() != QFont::StyleNormal; }
    }
    cache.insert(key, f);
    return f;
}

void tuneFont(QFont& font, int pixelSize) {
    font.setPixelSize(std::max(1, pixelSize));
    font.setHintingPreference(QFont::PreferNoHinting);        // design-unit advances, as close to FreeType's layout as it gets
    font.setStyleStrategy(QFont::StyleStrategy(QFont::PreferAntialias | QFont::NoSubpixelAntialias));   // grey AA on alpha
    font.setKerning(true);
}

// last[:-1], one code point
void chopCodePoint(QString& s) {
    if (s.isEmpty()) return;
    const qsizetype n = s.size();
    s.chop(n >= 2 && s.at(n - 1).isLowSurrogate() && s.at(n - 2).isHighSurrogate() ? 2 : 1);
}

// `s.rsplit(" ", 1)[0] if " " in s else s[:-1]`
QString dropTail(QString s) {
    const qsizetype sp = s.lastIndexOf(QLatin1Char(' '));
    if (sp >= 0) return s.left(sp);
    chopCodePoint(s);
    return s;
}

// int(text, 16) for a slice of hex_rgb's string: whitespace around, one sign, digits with single underscores between
// them (Unicode decimal digits count, as in Python). int()'s whitespace is narrower than str.strip()'s: CPython maps
// non-ASCII spaces to ' ' but keeps ASCII as is, and then skips only \t \n \v \f \r and ' ' (so \x1c..\x1f fail).
bool pyHexInt(const QList<uint>& cps, int* out) {
    qsizetype b = 0, e = cps.size();
    auto space = [](uint c) { return (c >= 0x09 && c <= 0x0d) || c == 0x20 || (c >= 0x7f && c < 0x10000 && isPySpace(QChar(char16_t(c)))); };
    while (b < e && space(cps[b])) ++b;
    while (e > b && space(cps[e - 1])) --e;
    bool neg = false;
    if (b < e && (cps[b] == '+' || cps[b] == '-')) { neg = cps[b] == '-'; ++b; }
    if (b >= e) return false;
    int v = 0; bool lastDigit = false;
    for (qsizetype i = b; i < e; ++i) {
        const uint c = cps[i];
        int d = -1;
        if (c >= '0' && c <= '9') d = int(c - '0');
        else if (c >= 'a' && c <= 'f') d = int(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = int(c - 'A' + 10);
        else if (c >= 0x80 && QChar::isDigit(char32_t(c))) d = QChar::digitValue(char32_t(c));
        else if (c == '_' && lastDigit && i + 1 < e) { lastDigit = false; continue; }
        if (d < 0) return false;
        v = v * 16 + d; lastDigit = true;
    }
    if (!lastDigit) return false;
    *out = neg ? -v : v;
    return true;
}

}   // namespace

// ---------------------------------------------------------------------------------------------- text hygiene, colours
bool isPySpace(QChar c) {
    const char16_t u = c.unicode();
    return (u >= 0x09 && u <= 0x0d) || (u >= 0x1c && u <= 0x20) || u == 0x85 || u == 0xa0 || u == 0x1680
        || (u >= 0x2000 && u <= 0x200a) || u == 0x2028 || u == 0x2029 || u == 0x202f || u == 0x205f || u == 0x3000;
}

QString clean(const QString& s) {
    // The replacements in the script's order. No replacement produces a later key, so one pass is the same as the
    // script's chain of str.replace() calls.
    QString r;
    r.reserve(s.size());
    for (QChar c : s) {
        switch (c.unicode()) {
        case 0x2018: case 0x2019: r += QLatin1Char('\''); break;
        case 0x201c: case 0x201d: r += QLatin1Char('"'); break;
        case 0x2013: case 0x00b7: case 0x2022: r += QLatin1Char('-'); break;
        case 0x2014: r += QLatin1String(" - "); break;
        case 0x2026: r += QLatin1String("..."); break;
        case 0x00a0: case 0x0009: r += QLatin1Char(' '); break;
        default: if (c.unicode() >= 32) r += c; break;        // control characters go (a newline is not a space here)
        }
    }
    // " ".join(s.split())
    QString out;
    out.reserve(r.size());
    bool pending = false;
    for (QChar c : r) {
        if (isPySpace(c)) { pending = !out.isEmpty(); continue; }
        if (pending) { out += QLatin1Char(' '); pending = false; }
        out += c;
    }
    return out;
}

QColor hexRgb(const QString& in, const QColor& fallback) {
    QString t = in;
    qsizetype b = 0, e = t.size();
    while (b < e && isPySpace(t.at(b))) ++b;
    while (e > b && isPySpace(t.at(e - 1))) --e;
    t = t.mid(b, e - b);
    while (t.startsWith(QLatin1Char('#'))) t.remove(0, 1);
    QList<uint> cps = t.toUcs4();
    if (cps.size() == 3) cps = {cps[0], cps[0], cps[1], cps[1], cps[2], cps[2]};
    int v[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i)
        if (!pyHexInt(cps.mid(i * 2, 2), &v[i])) return fallback;
    // a part like "-f" parses to -15 in Python; Pillow clips ink to 0..255, which is what a fill then shows
    return QColor(std::clamp(v[0], 0, 255), std::clamp(v[1], 0, 255), std::clamp(v[2], 0, 255));
}

// ---------------------------------------------------------------------------------------------- fonts
Face::Face() : Face(QFont(QStringLiteral("Segoe UI")), QString()) {}

Face::Face(const QFont& font, const QString& path)
    : m_font(font), m_path(path), m_fm(m_font, metricsDevice()) {
    if (m_font.pixelSize() <= 0) { tuneFont(m_font, 16); m_fm = QFontMetricsF(m_font, metricsDevice()); }
    computeMetrics();
}

// FreeType (what Pillow reports): face->ascender / descender come from hhea; FT_Set_Pixel_Sizes gives
// y_scale = FT_DivFix(px << 6, upem); size metrics are FT_PIX_CEIL(FT_MulFix(ascender, y_scale)) and
// FT_PIX_FLOOR(FT_MulFix(descender, y_scale)), and getmetrics() returns them in whole pixels.
void Face::computeMetrics() {
    m_ascent = int(std::ceil(m_fm.ascent() - 1e-6));
    m_descent = int(std::ceil(m_fm.descent() - 1e-6));
    const QRawFont raw = QRawFont::fromFont(m_font);
    const QByteArray head = raw.isValid() ? raw.fontTable("head") : QByteArray(), hhea = raw.isValid() ? raw.fontTable("hhea") : QByteArray();
    if (head.size() < 20 || hhea.size() < 8) return;
    auto be16 = [](const QByteArray& b, int at) { return quint16((uchar(b[at]) << 8) | uchar(b[at + 1])); };
    const qint64 upem = be16(head, 18);
    const qint64 asc = qint16(be16(hhea, 4)), desc = qint16(be16(hhea, 6));
    if (upem <= 0 || (asc == 0 && desc == 0)) return;
    const qint64 scale = ((qint64(std::max(1, m_font.pixelSize())) << 6 << 16) + upem / 2) / upem;   // FT_DivFix
    auto mulFix = [scale](qint64 a) { const qint64 v = (std::abs(a) * scale + 0x8000) >> 16; return a < 0 ? -v : v; };
    const qint64 a26 = mulFix(asc), d26 = mulFix(desc);
    m_ascent = int((a26 + 63) >> 6);                                       // ceil
    m_descent = int(-((d26 >= 0 ? d26 : d26 - 63) / 64));                 // -floor
}

int Face::width(const QString& s) const {
    if (s.isEmpty()) return 0;
    const double adv = m_fm.horizontalAdvance(s);
    const QRectF ink = m_fm.boundingRect(s);
    const double right = ink.isEmpty() ? adv : std::max(adv, ink.right());
    const double left = ink.isEmpty() ? 0.0 : std::min(0.0, ink.left());
    return int(std::lround(right) - std::lround(left));
}

double Face::length(const QString& s) const { return s.isEmpty() ? 0.0 : m_fm.horizontalAdvance(s); }

QStringList boldCandidates() { QStringList l; for (const char* p : kBold) l << QString::fromLatin1(p); return l; }
QStringList regularCandidates() { QStringList l; for (const char* p : kRegular) l << QString::fromLatin1(p); return l; }

QStringList fontPaths(const QJsonObject& spec, bool bold) {
    const QJsonObject fonts = spec.value(QStringLiteral("fonts")).toObject();
    const QString own = fonts.value(bold ? QStringLiteral("bold") : QStringLiteral("regular")).toString();
    return (own.isEmpty() ? QStringList() : QStringList{own}) + (bold ? boldCandidates() : regularCandidates());
}

bool fontFileFace(const QString& path, QString* family, QString* styleName) {
    const QString file = resolveFontFile(path);
    if (file.isEmpty()) return false;
    const FileFace f = fileFace(file);
    if (!f.ok) return false;
    if (family) *family = f.family;
    if (styleName) *styleName = f.style;
    return true;
}

Face loadFont(const QStringList& paths, int pixelSize, bool boldFallback) {
    for (const QString& p : paths) {
        const QString file = resolveFontFile(p);
        if (file.isEmpty()) continue;
        const FileFace f = fileFace(file);
        if (!f.ok) continue;
        QFont font(f.family);
        if (!f.style.isEmpty()) font.setStyleName(f.style);   // picks this file's face out of the family
        font.setWeight(QFont::Weight(f.weight));
        font.setItalic(f.italic);
        tuneFont(font, pixelSize);
        return Face(font, file);
    }
    QFont font(QStringLiteral("Segoe UI"));
    font.setWeight(boldFallback ? QFont::Bold : QFont::Normal);
    tuneFont(font, pixelSize);
    return Face(font, QString());
}

// ---------------------------------------------------------------------------------------------- wrap / ellipsize
QStringList wrap(const QString& s, const Measure& width, int maxW, int maxLines) {
    const QStringList words = clean(s).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList lines;
    QString cur;
    for (const QString& w : words) {
        const QString trial = cur.isEmpty() ? w : cur + QLatin1Char(' ') + w;
        if (cur.isEmpty() || width(trial) <= maxW) cur = trial;
        else { lines << cur; cur = w; }
    }
    if (!cur.isEmpty()) lines << cur;
    if (lines.size() > maxLines) {
        if (maxLines < 1) return {};                           // the script would raise here; no caller passes it
        lines = lines.mid(0, maxLines);
        QString last = lines.last();
        while (!last.isEmpty() && width(last + QLatin1String("...")) > maxW) last = dropTail(last);
        lines.last() = last + QLatin1String("...");
    }
    return lines;
}

QStringList wrap(const QString& s, const Face& f, int maxW, int maxLines) {
    return wrap(s, [&f](const QString& t) { return f.width(t); }, maxW, maxLines);
}

QString ellipsize(const QString& s, const Measure& width, int maxW) {
    QString t = clean(s);
    if (width(t) <= maxW) return t;
    while (!t.isEmpty() && width(t + QLatin1String("...")) > maxW) t = dropTail(t);
    return t + QLatin1String("...");
}

QString ellipsize(const QString& s, const Face& f, int maxW) {
    return ellipsize(s, [&f](const QString& t) { return f.width(t); }, maxW);
}

// ---------------------------------------------------------------------------------------------- drawing
QImage newCanvas(int w, int h) {
    QImage img(std::max(1, w), std::max(1, h), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    return img;
}

void preparePainter(QPainter& p) {
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
}

void drawText(QPainter& p, const QPointF& xy, const QString& s, const Face& f, const QColor& fill) {
    if (s.isEmpty()) return;
    p.setFont(f.font());
    p.setPen(fill);
    p.drawText(QPointF(xy.x(), xy.y() + f.ascent()), s);      // anchor "la": the baseline sits one ascent below xy
}

void drawText(QImage& img, const QPointF& xy, const QString& s, const Face& f, const QColor& fill) {
    if (s.isEmpty()) return;
    QPainter p(&img);
    preparePainter(p);
    drawText(p, xy, s, f, fill);
}

void shadowText(QImage& img, const QPointF& xy, const QString& s, const Face& f, const QColor& fill, int blurPx, const QPoint& off, int alpha) {
    if (s.isEmpty()) return;
    alpha = std::clamp(alpha, 0, 255);
    if (alpha > 0) {
        // The script draws the shadow on a full-frame layer and blurs all of it. Only the text's box plus the blur's
        // reach can turn non-zero, and the crop's own edges stay zero through every pass, so blurring the crop (clamped to
        // the frame, where both repeat the same border pixels) gives the full-frame pixels.
        const QPointF at = xy + QPointF(off);
        const QFontMetricsF fm(f.font(), metricsDevice());
        const QRectF ink = fm.boundingRect(s).translated(0, f.ascent());
        const QRectF pen(0, 0, std::max(1.0, fm.horizontalAdvance(s)), f.height());
        const float br = blurPx > 0 ? blur::gaussianBoxRadius(float(blurPx), 3) : 0.0f;
        const int m = blur::spread(br, 3) + 3;                 // + antialiasing fringe
        const QRect area = (ink.isEmpty() ? pen : pen.united(ink)).translated(at).toAlignedRect().adjusted(-m, -m, m, m) & img.rect();
        if (!area.isEmpty()) {
            QImage layer(area.size(), QImage::Format_ARGB32_Premultiplied);   // black at `alpha`: only the alpha byte is set
            layer.fill(Qt::transparent);
            {
                QPainter p(&layer);
                preparePainter(p);
                drawText(p, at - QPointF(area.topLeft()), s, f, QColor(0, 0, 0, alpha));
            }
            if (br > 0) blur::boxBlur(layer, br, br, 3);
            QPainter p(&img);
            preparePainter(p);
            p.drawImage(area.topLeft(), layer);
        }
    }
    drawText(img, xy, s, f, fill);
}

QRect pilRect(double x0, double y0, double x1, double y1) {
    return QRect(QPoint(int(x0), int(y0)), QPoint(int(x1), int(y1)));   // (int) casts, corners inclusive
}

void fillRect(QImage& img, const QRect& r, const QColor& c) {
    if (r.isEmpty() || c.alpha() == 0) return;
    QPainter p(&img);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.fillRect(r, c);
}

void setRect(QImage& img, const QRect& r, const QColor& c) {
    if (r.isEmpty()) return;
    QPainter p(&img);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(r, c);
}

void ring(QImage& img, const QRect& r, const QColor& c, int w, bool replace) {
    if (r.isEmpty()) return;
    w = std::max(1, w);
    // four bands that never overlap (an overlap would blend twice in the layer case): full-width top and bottom rows,
    // the sides between them; a box thinner than two widths is all ring
    const int bh = std::min(w, r.height()), bw = std::min(w, r.width());
    const int bottomTop = std::max(r.top() + bh, r.bottom() - w + 1), rightLeft = std::max(r.left() + bw, r.right() - w + 1);
    const QRect bands[] = {
        QRect(r.left(), r.top(), r.width(), bh),
        QRect(QPoint(r.left(), bottomTop), r.bottomRight()),
        QRect(QPoint(r.left(), r.top() + bh), QPoint(r.left() + bw - 1, bottomTop - 1)),
        QRect(QPoint(rightLeft, r.top() + bh), QPoint(r.right(), bottomTop - 1)),
    };
    for (const QRect& b : bands) replace ? setRect(img, b, c) : fillRect(img, b, c);
}

Chip chip(QImage& img, int x, int y, const QString& text, const Face& f, const QColor& bg, const QColor& fg, double s, const QColor& outline) {
    const int tw = f.width(text);
    const int px = int(20 * s), py = int(8 * s);
    const int hgt = f.height() + py * 2;
    const QRect box(QPoint(x, y), QPoint(x + tw + px * 2, y + hgt));
    if (outline.isValid()) {
        // Pillow fills the box, then draws the outline over its edge pixels, inside the box, on the layer: the ring
        // shows the outline colour, the inside the fill.
        const int w = std::max(1, int(2 * s));
        fillRect(img, box.adjusted(w, w, -w, -w), bg);
        ring(img, box, outline, w);
    } else {
        fillRect(img, box, bg);
    }
    drawText(img, QPointF(x + px, y + py), text, f, fg);
    return {x + tw + px * 2, hgt};
}

int spaced(QImage& img, const QPointF& xy, const QString& text, const Face& f, const QColor& fill, int spacing) {
    QPainter p(&img);
    preparePainter(p);
    double x = xy.x();
    for (qsizetype i = 0; i < text.size();) {
        const qsizetype n = text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate() ? 2 : 1;
        const QString ch = text.mid(i, n);
        drawText(p, QPointF(x, xy.y()), ch, f, fill);
        x += f.length(ch) + spacing;                           // the advance: a space moves on without ink
        i += n;
    }
    return int(x - xy.x());
}

// ---------------------------------------------------------------------------------------------- readability layers
void scrim(QImage& img, const QColor& ink, double span, int peak) {
    const int W = img.width(), H = img.height();
    const int n = int(W * span);
    if (n <= 0 || H <= 0) return;
    QPainter p(&img);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    for (int x = 0; x < n && x < W; ++x) {
        const int a = int(peak * std::pow(1.0 - double(x) / n, 1.35));
        if (a > 0) p.fillRect(QRect(x, 0, 1, H), QColor(ink.red(), ink.green(), ink.blue(), std::min(a, 255)));
    }
}

void bottomVignette(QImage& img, double frac, int peak) {
    const int W = img.width(), H = img.height();
    const int n = int(H * frac);
    if (n <= 0 || W <= 0) return;
    QPainter p(&img);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    for (int yy = 0; yy < n && yy < H; ++yy) {
        const int a = int(peak * (1.0 - yy / (H * frac)));
        if (a > 0) p.fillRect(QRect(0, H - 1 - yy, W, 1), QColor(0, 0, 0, std::min(a, 255)));
    }
}

void wash(QImage& img, const QColor& ink, int alpha) {
    const int W = img.width(), H = img.height();
    if (W <= 0 || H <= 0) return;
    alpha = std::clamp(alpha, 0, 255);
    const int n = std::min(int(H * 0.3), H);
    QPainter p(&img);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    if (alpha > 0 && H - n > 0) p.fillRect(QRect(0, 0, W, H - n), QColor(ink.red(), ink.green(), ink.blue(), alpha));
    for (int yy = 0; yy < n; ++yy) {                           // the layer's bottom rows are replaced, not blended, with black
        const int a = std::min(255, alpha + int(80 * (1.0 - yy / (H * 0.3))));
        if (a > 0) p.fillRect(QRect(0, H - 1 - yy, W, 1), QColor(0, 0, 0, a));
    }
}

// ---------------------------------------------------------------------------------------------- beds
QImage gradientBed(int w, int h, const QColor& accent, const QColor& ink) {
    QImage img(std::max(1, w), std::max(1, h), QImage::Format_RGB888);
    w = img.width(); h = img.height();
    // numpy (2.x, NEP 50): Python floats meet float32 arrays as float32, so every step below is a float operation in
    // the script's order. linspace computes in double and casts, with the last row set to exactly 1.
    const float top[3] = {float(std::min(255, ink.red() + 22)), float(std::min(255, ink.green() + 26)), float(std::min(255, ink.blue() + 34))};
    const float bot[3] = {float(std::max(0, ink.red() - 6)), float(std::max(0, ink.green() - 8)), float(std::max(0, ink.blue() - 10))};
    const float acc[3] = {float(accent.red()), float(accent.green()), float(accent.blue())};
    const float cool[3] = {40.0f, 90.0f, 140.0f};
    const float cx = float(w * 0.86), cy = float(h * 0.12), r = float(w * 0.55);
    const float cx2 = float(w * 0.08), cy2 = float(h * 0.95), r2 = float(w * 0.5);
    const double step = h > 1 ? 1.0 / (h - 1) : 0.0;
    for (int y = 0; y < h; ++y) {
        const float yv = h > 1 && y == h - 1 ? 1.0f : float(double(y) * step);
        float base[3];
        for (int c = 0; c < 3; ++c) base[c] = top[c] * (1.0f - yv) + bot[c] * yv;
        const float dy = float(y) - cy, dy2 = float(y) - cy2;
        const float scan = y % 6 == 0 ? 10.0f : 0.0f;
        uchar* row = img.scanLine(y);
        for (int x = 0; x < w; ++x) {
            float px[3] = {base[0], base[1], base[2]};
            const float dx = float(x) - cx;
            const float g1 = std::clamp(1.0f - std::sqrt(dx * dx + dy * dy) / r, 0.0f, 1.0f);
            if (g1 > 0.0f) {                                   // a zero glow leaves the pixel exactly as it is in numpy too
                const float g = std::pow(g1, 2.2f) * 0.42f;
                for (int c = 0; c < 3; ++c) px[c] = px[c] * (1 - g) + acc[c] * g;
            }
            const float dx2 = float(x) - cx2;
            const float g2 = std::clamp(1.0f - std::sqrt(dx2 * dx2 + dy2 * dy2) / r2, 0.0f, 1.0f);
            if (g2 > 0.0f) {
                const float g = std::pow(g2, 2.5f) * 0.22f;
                for (int c = 0; c < 3; ++c) px[c] = px[c] * (1 - g) + cool[c] * g;
            }
            for (int c = 0; c < 3; ++c) row[x * 3 + c] = uchar(std::clamp(px[c] + scan, 0.0f, 255.0f));   // astype: truncate
        }
    }
    return img;
}

}   // namespace cardkit
