// The scripts/spot_card.py layouts and the `card` / `scenes` sub-commands, see spotcard.h.
#include "spotcard.h"

#include "blur.h"
#include "cardkit.h"
#include "imageio.h"
#include "ppcli.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QPolygon>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>
#include <QVariant>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace spotcard {
namespace {

using cardkit::Face;
using cardkit::clean;
using cardkit::pilRect;

[[noreturn]] void raise(const QString& m) { throw std::runtime_error(m.toStdString()); }
QString what(const std::exception& e) { return QString::fromStdString(e.what()); }

// ---------------------------------------------------------------------------------------------- Python value semantics
// bool(v): what `spec.get(k) or default` tests.
bool truthy(const QJsonValue& v) {
    switch (v.type()) {
    case QJsonValue::Bool: return v.toBool();
    case QJsonValue::Double: return v.toDouble() != 0.0;
    case QJsonValue::String: return !v.toString().isEmpty();
    case QJsonValue::Array: return !v.toArray().isEmpty();
    case QJsonValue::Object: return !v.toObject().isEmpty();
    default: return false;
    }
}
bool isInteger(const QJsonValue& v) { return v.isDouble() && v.toVariant().typeId() == QMetaType::LongLong; }
// str(v) for the JSON values a spec holds
QString pyStr(const QJsonValue& v) {
    switch (v.type()) {
    case QJsonValue::String: return v.toString();
    case QJsonValue::Bool: return v.toBool() ? QStringLiteral("True") : QStringLiteral("False");
    case QJsonValue::Null: case QJsonValue::Undefined: return QStringLiteral("None");
    case QJsonValue::Double: {
        if (isInteger(v)) return QString::number(v.toVariant().toLongLong());
        const double d = v.toDouble();
        QString s = QString::number(d, 'g', QLocale::FloatingPointShortest);
        if (!s.contains(QLatin1Char('.')) && !s.contains(QLatin1Char('e')) && std::isfinite(d)) s += QLatin1String(".0");
        return s;
    }
    case QJsonValue::Array: return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
    case QJsonValue::Object: return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    }
    return {};
}
QString text(const QJsonValue& v) { return truthy(v) ? pyStr(v) : QString(); }                       // `v or ""`
QString orDefault(const QJsonValue& v, const QString& def) { return truthy(v) ? pyStr(v) : def; }  // `v or "DEF"`
QString pyStrip(const QString& s) {
    qsizetype b = 0, e = s.size();
    while (b < e && cardkit::isPySpace(s.at(b))) ++b;
    while (e > b && cardkit::isPySpace(s.at(e - 1))) --e;
    return s.mid(b, e - b);
}
qsizetype pyLen(const QString& s) { return s.toUcs4().size(); }
int fdiv(int a, int b) { const int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }   // Python's //

// int(v or def): a number truncates, a string goes through int(); anything else is a TypeError in the script.
int pyInt(const QJsonValue& v, int def, const char* key) {
    if (!truthy(v)) return def;
    if (v.isBool()) return 1;
    if (v.isDouble()) {
        const double d = std::trunc(v.toDouble());
        if (!std::isfinite(d) || std::abs(d) > 1e9) raise(QStringLiteral("%1: %2 is out of range").arg(QLatin1String(key), pyStr(v)));
        return int(d);
    }
    if (v.isString()) {
        QString t = pyStrip(v.toString());
        bool ok = false;
        const qlonglong n = t.remove(QRegularExpression(QStringLiteral("(?<=\\d)_(?=\\d)"))).toLongLong(&ok, 10);
        if (!ok) raise(QStringLiteral("invalid literal for int() with base 10: '%1'").arg(v.toString()));
        if (std::llabs(n) > 1000000000LL) raise(QStringLiteral("%1: %2 is out of range").arg(QLatin1String(key), v.toString()));
        return int(n);
    }
    raise(QStringLiteral("int() argument must be a string or a number, not '%1'").arg(v.isArray() ? "list" : "dict"));
}

// sc["key"]: present and a string (the script adds ".png" to it or opens it)
QString required(const QJsonObject& o, const char* key) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isUndefined()) raise(QStringLiteral("'%1'").arg(QLatin1String(key)));
    if (!v.isString()) raise(QStringLiteral("%1 must be a path").arg(QLatin1String(key)));
    return v.toString();
}

QColor opaque(const QColor& c) { return QColor(c.red(), c.green(), c.blue(), 255); }
QColor withAlpha(const QColor& c, int a) { return QColor(c.red(), c.green(), c.blue(), a); }
const QColor kWhite(255, 255, 255, 255);

// _tone_color(): green up, red down, gold for flat / unknown
QColor toneColor(const QString& tone) {
    return tone == QLatin1String("up") ? QColor(92, 214, 130) : tone == QLatin1String("down") ? QColor(255, 107, 107) : QColor(255, 214, 102);
}

// ---------------------------------------------------------------------------------------------- drawing with a trace
void recordText(Trace* t, const QString& s, QPoint at, const Face& f) {
    if (!t || s.isEmpty()) return;
    t->texts.append(TextItem{s, at, f.pixelSize(), f.width(s), f.height()});
}

// How far a blurred, offset copy of `r` reaches: the blur's spread plus a few pixels of glyph overhang.
QRect reach(const QRect& r, int blurPx, QPoint off) {
    const int m = (blurPx > 0 ? blur::spread(blur::gaussianBoxRadius(float(blurPx), 3), 3) : 0) + 6;
    return r.united(r.translated(off)).adjusted(-m, -m, m, m);
}

void textAt(QImage& img, Trace* t, QPoint at, const QString& s, const Face& f, const QColor& fill) {
    recordText(t, s, at, f);
    cardkit::drawText(img, QPointF(at), s, f, fill);
}

void shadow(QImage& img, Trace* t, const char* name, QPoint at, const QString& s, const Face& f, const QColor& fill,
            int blurPx, QPoint off, int alpha = 170) {
    recordText(t, s, at, f);
    if (t && !s.isEmpty()) t->addBox(QLatin1String(name), reach(QRect(at, QSize(std::max(1, f.width(s)), f.height())), blurPx, off) & img.rect());
    cardkit::shadowText(img, QPointF(at), s, f, fill, blurPx, off, alpha);
}

cardkit::Chip chip(QImage& img, Trace* t, const char* name, int x, int y, const QString& s, const Face& f, const QColor& bg,
                   const QColor& fg, double sc, const QColor& outline = QColor()) {
    recordText(t, s, QPoint(x + int(20 * sc), y + int(8 * sc)), f);
    const cardkit::Chip c = cardkit::chip(img, x, y, s, f, bg, fg, sc, outline);
    if (t) t->addBox(QLatin1String(name), QRect(QPoint(x, y), QPoint(c.right, y + c.height)) & img.rect());
    return c;
}

int spaced(QImage& img, Trace* t, const char* name, QPoint at, const QString& s, const Face& f, const QColor& fill, int spacing) {
    const int w = cardkit::spaced(img, QPointF(at), s, f, fill, spacing);
    if (t && !s.isEmpty()) {
        t->texts.append(TextItem{s, at, f.pixelSize(), w, f.height()});
        t->addBox(QLatin1String(name), QRect(at, QSize(std::max(1, w), f.height())).adjusted(-6, -6, 6, 6) & img.rect());
    }
    return w;
}

void setRect(QImage& img, Trace* t, const char* name, const QRect& r, const QColor& c) {
    cardkit::setRect(img, r, c);
    if (t) t->addBox(QLatin1String(name), r & img.rect());
}

// A layer holding one rectangle, Gaussian-blurred, alpha-composited (the script's full-frame shadow layers). Only the
// rectangle plus the blur's spread can turn non-zero, so a crop of that size, clamped to the frame where both repeat the
// border pixels, blurs to the full-frame result.
void blurredRect(QImage& img, Trace* t, const char* name, const QRect& r, const QColor& c, int radius) {
    const float br = blur::gaussianBoxRadius(float(radius), 3);
    const int m = blur::spread(br, 3) + 1;
    const QRect area = r.adjusted(-m, -m, m, m) & img.rect();
    if (area.isEmpty() || r.isEmpty()) return;
    QImage layer(area.size(), QImage::Format_ARGB32_Premultiplied);
    layer.fill(Qt::transparent);
    {
        QPainter p(&layer);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(r.translated(-area.topLeft()) & layer.rect(), c);
    }
    blur::boxBlur(layer, br, br, 3);
    QPainter p(&img);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.drawImage(area.topLeft(), layer);
    if (t) t->addBox(QLatin1String(name), area);
}

// draw.rounded_rectangle on a layer, composited: Pillow's shapes are not antialiased.
void roundedPanel(QImage& img, Trace* t, const char* name, const QRect& r, int radius, const QColor& c) {
    if (r.isEmpty()) return;
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawRoundedRect(QRectF(r), radius, radius);
    if (t) t->addBox(QLatin1String(name), r & img.rect());
}

// draw.ellipse(box, fill) / with an outline `ow` wide inside the box
void ellipse(QImage& img, const QRect& box, const QColor& fill, const QColor& outline = QColor(), int ow = 0) {
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setPen(Qt::NoPen);
    p.setBrush(outline.isValid() && ow > 0 ? outline : fill);
    p.drawEllipse(QRectF(box));
    if (outline.isValid() && ow > 0) { p.setBrush(fill); p.drawEllipse(QRectF(box).adjusted(ow, ow, -ow, -ow)); }
}

// ---------------------------------------------------------------------------------------------- Pillow's resampling
// Image.resize(size, LANCZOS / BICUBIC) as Resample.c does it: per-axis coefficient tables in double, normalised, then
// 22-bit fixed point (normalize_coeffs_8bpc), a horizontal pass then a vertical one, each rounded and clipped to 8 bits.
// The photo beds go through it so they match the script's (Qt's and libswscale's Lanczos differ in support and rounding).
enum class Filter { Bicubic, Lanczos };
constexpr int kPrecision = 32 - 8 - 2;

double filterValue(Filter f, double x) {
    if (f == Filter::Bicubic) {
        const double a = -0.5;
        if (x < 0.0) x = -x;
        if (x < 1.0) return ((a + 2.0) * x - (a + 3.0)) * x * x + 1;
        if (x < 2.0) return (((x - 5) * x + 8) * x - 4) * a;
        return 0.0;
    }
    auto sinc = [](double v) { if (v == 0.0) return 1.0; v = v * 3.14159265358979323846; return std::sin(v) / v; };
    return -3.0 <= x && x < 3.0 ? sinc(x) * sinc(x / 3) : 0.0;
}

struct Coeffs { int ksize = 0; std::vector<int> bounds; std::vector<qint32> k; };   // bounds: xmin, xmax per output

Coeffs precompute(Filter f, int inSize, int outSize) {
    const double support0 = f == Filter::Bicubic ? 2.0 : 3.0;
    const double scale = double(inSize) / outSize;
    const double filterscale = scale < 1.0 ? 1.0 : scale;
    const double support = support0 * filterscale;
    Coeffs c;
    c.ksize = int(std::ceil(support)) * 2 + 1;
    c.bounds.resize(size_t(outSize) * 2);
    c.k.assign(size_t(outSize) * size_t(c.ksize), 0);
    std::vector<double> pre(size_t(c.ksize));
    for (int xx = 0; xx < outSize; ++xx) {
        const double center = (xx + 0.5) * scale;
        const double ss = 1.0 / filterscale;
        int xmin = int(center - support + 0.5);
        if (xmin < 0) xmin = 0;
        int xmax = int(center + support + 0.5);
        if (xmax > inSize) xmax = inSize;
        xmax -= xmin;
        double ww = 0.0;
        for (int x = 0; x < xmax; ++x) { const double w = filterValue(f, (x + xmin - center + 0.5) * ss); pre[size_t(x)] = w; ww += w; }
        for (int x = 0; x < xmax; ++x) {
            const double v = ww != 0.0 ? pre[size_t(x)] / ww : pre[size_t(x)];
            c.k[size_t(xx) * size_t(c.ksize) + size_t(x)] = v < 0 ? qint32(-0.5 + v * (1 << kPrecision)) : qint32(0.5 + v * (1 << kPrecision));
        }
        c.bounds[size_t(xx) * 2] = xmin;
        c.bounds[size_t(xx) * 2 + 1] = xmax;
    }
    return c;
}

inline uchar clip8(qint64 v) { v >>= kPrecision; return uchar(v < 0 ? 0 : v > 255 ? 255 : v); }

// RGB32 in, RGB32 out (the padding byte kept at 0xff). Both passes walk memory in order: the vertical one adds whole
// source rows into a row of accumulators, tap by tap.
QImage pilResize(const QImage& in, QSize size, Filter f) {
    QImage src = in.convertToFormat(QImage::Format_RGB32);
    if (src.size() == size || size.isEmpty()) return src;
    const int sw = src.width(), sh = src.height(), dw = size.width(), dh = size.height();
    QImage mid;
    if (dw != sw) {
        mid = QImage(dw, sh, QImage::Format_RGB32);
        const Coeffs c = precompute(f, sw, dw);
        for (int y = 0; y < sh; ++y) {
            const uchar* s = src.constScanLine(y);
            uchar* d = mid.scanLine(y);
            for (int xx = 0; xx < dw; ++xx) {
                const int xmin = c.bounds[size_t(xx) * 2], xmax = c.bounds[size_t(xx) * 2 + 1];
                const qint32* k = &c.k[size_t(xx) * size_t(c.ksize)];
                const uchar* p = s + size_t(xmin) * 4;
                qint64 s0 = qint64(1) << (kPrecision - 1), s1 = s0, s2 = s0;
                for (int x = 0; x < xmax; ++x, p += 4) { s0 += qint64(p[0]) * k[x]; s1 += qint64(p[1]) * k[x]; s2 += qint64(p[2]) * k[x]; }
                d[xx * 4] = clip8(s0); d[xx * 4 + 1] = clip8(s1); d[xx * 4 + 2] = clip8(s2); d[xx * 4 + 3] = 0xff;
            }
        }
    } else {
        mid = src;
    }
    if (dh == sh) return mid;
    QImage out(dw, dh, QImage::Format_RGB32);
    const Coeffs c = precompute(f, sh, dh);
    const size_t n = size_t(dw) * 4;
    std::vector<qint64> acc(n);
    for (int yy = 0; yy < dh; ++yy) {
        const int ymin = c.bounds[size_t(yy) * 2], ymax = c.bounds[size_t(yy) * 2 + 1];
        const qint32* k = &c.k[size_t(yy) * size_t(c.ksize)];
        std::fill(acc.begin(), acc.end(), qint64(1) << (kPrecision - 1));
        for (int y = 0; y < ymax; ++y) {
            const uchar* row = mid.constScanLine(ymin + y);
            const qint64 kv = k[y];
            for (size_t i = 0; i < n; ++i) acc[i] += qint64(row[i]) * kv;
        }
        uchar* d = out.scanLine(yy);
        for (size_t i = 0; i < n; ++i) d[i] = (i & 3) == 3 ? uchar(0xff) : clip8(acc[i]);
    }
    return out;
}

// _cover(img, w, h, ybias) with Pillow's Lanczos
QImage pilCover(const QImage& img, int w, int h, double ybias) {
    const double k = std::max(double(w) / img.width(), double(h) / img.height());
    const int nw = std::max(w, int(std::nearbyint(img.width() * k)));      // round(): half to even
    const int nh = std::max(h, int(std::nearbyint(img.height() * k)));
    const QImage r = pilResize(img, QSize(nw, nh), Filter::Lanczos);
    return r.copy((nw - w) / 2, int((nh - h) * ybias), w, h);
}

// .convert("RGB"): alpha dropped, the stored colour kept (no compositing)
QImage toRgb(const QImage& in) {
    QImage a = in.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < a.height(); ++y) {
        auto* p = reinterpret_cast<QRgb*>(a.scanLine(y));
        for (int x = 0; x < a.width(); ++x) p[x] |= 0xff000000u;
    }
    return a.convertToFormat(QImage::Format_RGB32);
}

QImage loadRgb(const QString& path) {
    const imageio::Loaded l = imageio::load(path);
    if (!l.ok()) raise(l.error.isEmpty() ? QStringLiteral("cannot identify image file '%1'").arg(path) : l.error);
    return toRgb(l.image);
}

// ---------------------------------------------------------------------------------------------- shared pieces
void drawStrap(QImage& img, const QJsonObject& spec, const Setup& su, const QString& footer, Trace* t) {
    const int W = su.W, H = su.H;
    const double s = su.s;
    const int safe_x = int(W * 0.058);
    const int strap_h = int(84 * s);
    const int sy = H - strap_h - int(26 * s);
    const QRect band = pilRect(0, sy, W, sy + strap_h);
    cardkit::fillRect(img, band, withAlpha(su.ink, 228));
    if (t) t->addBox(QStringLiteral("strap"), band & img.rect());
    cardkit::setRect(img, pilRect(0, sy, W, sy + int(5 * s)), opaque(su.accent));
    const QString brand = clean(text(spec.value("brand"))).toUpper();
    const QString brandName = clean(text(spec.value("brandName")));
    const QString series = clean(text(spec.value("series")));
    const Face f_b = cardkit::loadFont(su.bold, int(34 * s), true);
    const Face f_r = cardkit::loadFont(su.reg, int(26 * s));
    int tx = safe_x;
    const int ty = sy + fdiv(strap_h - int(34 * s), 2) - int(2 * s);
    if (!brand.isEmpty()) {
        textAt(img, t, QPoint(tx, ty), brand, f_b, kWhite);
        tx += f_b.width(brand) + int(22 * s);
    }
    QStringList parts;
    if (brandName.toUpper() != brand && !brandName.isEmpty()) parts << brandName;
    if (!series.isEmpty()) parts << series;
    const QString sub = parts.join(QLatin1String(" - "));
    const int fw = footer.isEmpty() ? 0 : f_r.width(footer);
    if (!sub.isEmpty()) {
        const int room = std::max(int(W * 0.2), W - safe_x - fw - int(40 * s) - tx);
        textAt(img, t, QPoint(tx, ty + int(7 * s)), cardkit::ellipsize(sub, f_r, std::min(int(W * 0.45), room)), f_r, QColor(170, 184, 200));
    }
    if (!footer.isEmpty()) textAt(img, t, QPoint(W - safe_x - fw, ty + int(7 * s)), footer, f_r, QColor(200, 210, 222));
}

QString footerOf(const QJsonObject& spec) {        // _footer(): "date  |  site"
    QStringList p;
    for (const char* k : {"date", "site"}) { const QString v = clean(text(spec.value(QLatin1String(k)))); if (!v.isEmpty()) p << v; }
    return p.join(QLatin1String("  |  "));
}

// _story_box(): the post's own picture, whole, framed on the right. nullopt when it cannot be read.
std::optional<int> storyBox(QImage& img, const QString& photoPath, const Setup& su, int safe_x, int strap_top, Trace* t) {
    const int W = su.W, H = su.H;
    const double s = su.s;
    QImage im;
    try { im = loadRgb(photoPath); } catch (const std::exception& e) { ppcli::log(QStringLiteral("box picture unreadable: %1").arg(what(e))); return std::nullopt; }
    const double ar = double(im.width()) / im.height();
    int bw, bh;
    if (ar >= 1.0) {
        bw = int(W * 0.47);
        bh = int(bw / ar);
        if (bh > int(H * 0.60)) { bh = int(H * 0.60); bw = int(bh * ar); }
    } else {
        bh = int(H * 0.66);
        bw = int(bh * ar);
    }
    const int top = int(196 * s), bottom = strap_top - int(40 * s);
    if (bh > bottom - top) { bh = bottom - top; bw = int(bh * ar); }
    const int x = W - safe_x - bw;
    const int y = std::max(top, std::min(bottom - bh, int(H * 0.535) - fdiv(bh, 2)));
    blurredRect(img, t, "boxShadow", pilRect(x - int(4 * s), y + int(10 * s), x + bw + int(4 * s), y + bh + int(24 * s)), QColor(0, 0, 0, 190),
                std::max(1, int(22 * s)));
    if (bw < 1 || bh < 1) raise(QStringLiteral("height and width must be > 0"));
    {
        QPainter p(&img);                                      // img.paste(): RGB replaces, opaque
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(x, y, pilResize(im, QSize(bw, bh), Filter::Lanczos));
    }
    cardkit::ring(img, pilRect(x - 1, y - 1, x + bw, y + bh), QColor(255, 255, 255, 90), std::max(1, int(2 * s)), true);
    cardkit::setRect(img, pilRect(x, y + bh, x + bw, y + bh + int(7 * s)), opaque(su.accent));
    if (t) t->addBox(QStringLiteral("storyBox"), pilRect(x - 1, y - 1, x + bw, y + bh + int(7 * s)) & img.rect());
    return x;
}

// _draw_spark(): a line chart in (x, y, w, h); soft fill under the line, the last point dotted, an optional label.
void drawSpark(QImage& img, Trace* t, int x, int y, int w, int h, const QJsonValue& values, const QColor& color, double s, const QString& label) {
    std::vector<double> vals;
    for (const QJsonValue& v : values.toArray()) {
        if (v.isDouble()) vals.push_back(v.toDouble());
        else if (v.isBool()) vals.push_back(v.toBool() ? 1.0 : 0.0);   // isinstance(True, int)
    }
    if (vals.size() < 2 || w < 10 || h < 10) return;
    const auto [lo, hi] = std::minmax_element(vals.begin(), vals.end());
    const double l = *lo, span = (*hi - *lo) != 0.0 ? *hi - *lo : 1.0;
    const int pad = int(h * 0.12);
    QPolygon pts;
    for (size_t i = 0; i < vals.size(); ++i)
        pts << QPoint(x + int(double(qint64(i) * w) / double(vals.size() - 1)), y + h - pad - int((vals[i] - l) / span * (h - 2 * pad)));
    const int lw = std::max(2, int(4 * s)), r = std::max(4, int(7 * s)), ow = std::max(2, int(3 * s));
    QImage layer = cardkit::newCanvas(img.width(), img.height());
    {
        QPainter p(&layer);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(color, 46));
        QPolygon fill = pts;
        fill << QPoint(pts.last().x(), y + h) << QPoint(pts.first().x(), y + h);
        p.drawPolygon(fill);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(opaque(color), lw, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));   // joint="curve"
        p.drawPolyline(pts);
    }
    ellipse(layer, QRect(QPoint(pts.last().x() - r, pts.last().y() - r), QPoint(pts.last().x() + r, pts.last().y() + r)), kWhite, opaque(color), ow);
    {
        QPainter p(&img);
        p.drawImage(0, 0, layer);
    }
    if (t) t->addBox(QStringLiteral("spark"), QRect(x, y, w, h).adjusted(-r - lw, -r - lw, r + lw, r + lw) & img.rect());
    if (!label.isEmpty()) {
        const Face f = cardkit::loadFont(cardkit::regularCandidates(), int(24 * s));
        textAt(img, t, QPoint(x, y + h + int(8 * s)), label, f, QColor(206, 216, 228, 220));
        if (t) t->addBox(QStringLiteral("sparkLabel"), QRect(QPoint(x, y + h + int(8 * s)), QSize(f.width(label), f.height())).adjusted(-6, -6, 6, 6) & img.rect());
    }
}

// _draw_rows(): label + sub, a sparkline, the price and a coloured change chip per row; returns the y below the table.
int drawRows(QImage& img, Trace* t, const QJsonValue& rowsV, int x0, int y0, int w, double s, const Setup& su) {
    QList<QJsonObject> rows;
    for (const QJsonValue& v : rowsV.toArray()) if (v.isObject() && rows.size() < 6) rows << v.toObject();
    if (rows.isEmpty()) return y0;
    const int row_h = int(118 * s);
    const int n = int(rows.size());
    roundedPanel(img, t, "rowsPanel", pilRect(x0 - int(22 * s), y0 - int(12 * s), x0 + w + int(22 * s), y0 + row_h * n + int(12 * s)), int(18 * s), QColor(6, 12, 24, 128));
    const Face f_l = cardkit::loadFont(su.bold, int(46 * s), true), f_s = cardkit::loadFont(su.reg, int(28 * s));
    const Face f_v = cardkit::loadFont(su.bold, int(50 * s), true), f_c = cardkit::loadFont(su.bold, int(36 * s), true);
    for (int i = 0; i < n; ++i) {
        const QJsonObject& r = rows[i];
        const int top = y0 + i * row_h;
        const QColor color = toneColor(text(r.value("tone")));
        if (i) {                                               // d.line(width=k) straight onto the card: replaces
            const int k = std::max(1, int(2 * s));
            cardkit::setRect(img, QRect(QPoint(x0, top - k / 2), QPoint(x0 + w, top - k / 2 + k - 1)), QColor(255, 255, 255, 38));
        }
        textAt(img, t, QPoint(x0 + int(10 * s), top + int(14 * s)), clean(text(r.value("label"))), f_l, kWhite);
        if (truthy(r.value("sub")))
            textAt(img, t, QPoint(x0 + int(10 * s), top + int(14 * s) + int(52 * s)), clean(pyStr(r.value("sub"))), f_s, QColor(190, 202, 218, 235));
        drawSpark(img, t, x0 + int(w * 0.34), top + int(18 * s), int(w * 0.20), row_h - int(36 * s), r.value("spark"), color, s, QString());
        const QString val = clean(text(r.value("value")));
        textAt(img, t, QPoint(x0 + int(w * 0.77) - f_v.width(val), top + int(26 * s)), val, f_v, kWhite);
        const QString chg = clean(text(r.value("change")));
        if (!chg.isEmpty()) {
            const int cw = f_c.width(chg) + int(34 * s);
            chip(img, t, "change", x0 + w - cw, top + int(30 * s), chg, f_c, opaque(color), QColor(8, 14, 26), s);
        }
    }
    return y0 + row_h * n;
}

void addTextBoxes(Trace* t, const QImage& img) {      // every text item's own box, so the trace covers all the ink
    if (!t) return;
    for (const TextItem& it : t->texts) t->addBox(QStringLiteral("text"), it.box().adjusted(-6, -6, 6, 6) & img.rect());
}

QJsonObject readSpec(const QString& path, const char* prefix) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) raise(QStringLiteral("%1[Errno 2] No such file or directory: '%2'").arg(QLatin1String(prefix), path));
    QByteArray b = f.readAll();
    if (b.startsWith("\xef\xbb\xbf")) b.remove(0, 3);          // utf-8-sig
    QJsonParseError pe{};
    const QJsonDocument d = QJsonDocument::fromJson(b, &pe);
    if (pe.error != QJsonParseError::NoError) raise(QStringLiteral("%1%2 at offset %3").arg(QLatin1String(prefix), pe.errorString()).arg(pe.offset));
    if (!d.isObject()) raise(QStringLiteral("%1the spec is not a JSON object").arg(QLatin1String(prefix)));
    return d.object();
}

}   // namespace

// ---------------------------------------------------------------------------------------------- public
QRect Trace::box(const QString& name) const {
    QRect u;
    for (const auto& b : boxes) if (b.first == name) u = u.united(b.second);
    return u;
}

Setup setup(const QJsonObject& spec) {
    Setup su;
    su.W = pyInt(spec.value("width"), 1920, "width");
    su.H = pyInt(spec.value("height"), 1080, "height");
    if (su.W <= 0 || su.H <= 0) raise(QStringLiteral("height and width must be > 0"));
    if (su.W > 16384 || su.H > 16384) raise(QStringLiteral("frame %1x%2 is too large (16384 a side at most)").arg(su.W).arg(su.H));
    su.s = su.H / 1080.0;
    su.accent = cardkit::hexRgb(spec.value("accent").isString() ? spec.value("accent").toString() : QString(), QColor(216, 0, 24));
    su.ink = cardkit::hexRgb(spec.value("ink").isString() ? spec.value("ink").toString() : QString(), QColor(11, 18, 32));
    su.bold = cardkit::fontPaths(spec, true);
    su.reg = cardkit::fontPaths(spec, false);
    return su;
}

QImage renderFurniture(const QJsonObject& spec, FurnitureInfo* info, Trace* t) {
    const Setup su = setup(spec);
    const int W = su.W, H = su.H;
    const double s = su.s;
    const bool photo = truthy(spec.value("photo"));
    QImage img = cardkit::newCanvas(W, H);
    const int safe_x = int(W * 0.058);
    const int text_col_w = photo ? int(W * 0.56) : int(W * 0.80);
    if (photo) {                                               // left-to-right scrim over the photograph
        cardkit::scrim(img, su.ink, 0.70, 225);
        if (t) t->addBox(QStringLiteral("scrim"), QRect(0, 0, int(W * 0.70), H));
    } else {                                                   // a soft bottom vignette otherwise
        cardkit::bottomVignette(img, 0.25, 90);
        if (t) t->addBox(QStringLiteral("vignette"), QRect(0, H - int(H * 0.25), W, int(H * 0.25)));
    }
    // kicker chip
    const QString kicker = clean(text(spec.value("kicker"))).toUpper();
    int y = int(140 * s);
    if (!kicker.isEmpty()) {
        const Face f_k = cardkit::loadFont(su.bold, int(34 * s), true);
        const int kw = f_k.width(kicker);
        const int pad_x = int(22 * s), pad_y = int(10 * s);
        const int chip_h = int(34 * s) + pad_y * 2 + int(6 * s);
        setRect(img, t, "kicker", pilRect(safe_x, y, safe_x + kw + pad_x * 2, y + chip_h), opaque(su.accent));
        textAt(img, t, QPoint(safe_x + pad_x, y + pad_y - int(2 * s)), kicker, f_k, kWhite);
        y += chip_h + int(26 * s);
    }
    // headline: bold, up to 3 lines, shrinks once
    const QString headline = clean(text(spec.value("headline")));
    QStringList head;
    if (!headline.isEmpty()) {
        int size = int(76 * s);
        Face f_h = cardkit::loadFont(su.bold, size, true);
        head = cardkit::wrap(headline, f_h, text_col_w, 3);
        if (head.size() == 3 && pyLen(headline) > 70) {
            size = int(64 * s);
            f_h = cardkit::loadFont(su.bold, size, true);
            head = cardkit::wrap(headline, f_h, text_col_w, 3);
        }
        for (const QString& ln : head) {
            shadow(img, t, "headline", QPoint(safe_x, y), ln, f_h, kWhite, int(7 * s), QPoint(0, int(3 * s)));
            y += int(size * 1.12);
        }
        y += int(22 * s);
    }
    // bullet lines
    QStringList lines;
    for (const QJsonValue& v : spec.value("lines").toArray())
        if (!pyStrip(pyStr(v)).isEmpty() && lines.size() < 4) lines << clean(pyStr(v));
    if (!lines.isEmpty()) {
        const Face f_l = cardkit::loadFont(su.reg, int(40 * s));
        const int bullet = int(14 * s);
        for (const QString& ln : lines) {
            const double cy = y + int(40 * s) * 0.55;
            setRect(img, t, "bullet", pilRect(safe_x, cy - bullet / 2.0, safe_x + bullet, cy + bullet / 2.0), opaque(su.accent));
            const QString txt = cardkit::ellipsize(ln, f_l, text_col_w - bullet - int(20 * s));
            shadow(img, t, "line", QPoint(safe_x + bullet + int(20 * s), y), txt, f_l, QColor(230, 236, 244), int(5 * s), QPoint(0, int(2 * s)), 140);
            y += int(40 * s * 1.45);
        }
    }
    drawStrap(img, spec, su, clean(text(spec.value("footer"))), t);
    addTextBoxes(t, img);
    if (info) { info->headlineLines = head; info->lines = int(lines.size()); }
    return img;
}

QImage renderStory(const QJsonObject& spec, const QJsonObject& sc, bool photo, const QString& layout, const QString& photoPath, Trace* t) {
    const Setup su = setup(spec);
    const int W = su.W, H = su.H;
    const double s = su.s;
    QImage img = cardkit::newCanvas(W, H);
    const int safe_x = int(W * 0.058);
    const int strap_top = H - int(84 * s) - int(26 * s);
    std::optional<int> box_x;
    if (layout == QLatin1String("box") && !photoPath.isEmpty()) {
        cardkit::scrim(img, su.ink, 0.62, 205);                // the text column over the moving blurred backdrop
        if (t) t->addBox(QStringLiteral("scrim"), QRect(0, 0, int(W * 0.62), H));
        box_x = storyBox(img, photoPath, su, safe_x, strap_top, t);
    }
    int col_w;
    if (box_x) {
        col_w = std::max(int(W * 0.30), *box_x - int(56 * s) - safe_x);
    } else if (photo) {
        col_w = int(W * 0.56);
        cardkit::scrim(img, su.ink, 0.74, 236);
        if (t) t->addBox(QStringLiteral("scrim"), QRect(0, 0, int(W * 0.74), H));
    } else {
        col_w = int(W * 0.80);
        cardkit::wash(img, su.ink, 40);
        if (t) t->addBox(QStringLiteral("wash"), img.rect());
    }
    const bool boxed = box_x.has_value();
    int y = int(104 * s);
    const cardkit::Chip k = chip(img, t, "kicker", safe_x, y, clean(orDefault(spec.value("kicker"), QStringLiteral("HEADLINE NEWS"))).toUpper(),
                                 cardkit::loadFont(su.bold, int(32 * s), true), opaque(su.accent), kWhite, s);
    const int right = k.right, chip_h = k.height;
    QString cat = clean(text(sc.value("category"))).toUpper();
    if (!cat.isEmpty()) {
        const Face f_c = cardkit::loadFont(su.bold, int(26 * s), true);
        cat = cardkit::ellipsize(cat, f_c, std::max(int(120 * s), col_w - (right - safe_x) - int(40 * s)));
        const int cy = y + fdiv(chip_h - (f_c.height() + int(16 * s)), 2);
        chip(img, t, "category", right + int(14 * s), cy, cat, f_c, QColor(255, 255, 255, 40), QColor(255, 255, 255, 240), s, QColor(255, 255, 255, 120));
    }
    const QString counter = clean(text(sc.value("counter")));
    if (!counter.isEmpty()) {
        const Face f_n = cardkit::loadFont(su.bold, int(30 * s), true);
        const int cw = f_n.width(counter) + int(40 * s);
        chip(img, t, "counter", W - safe_x - cw, y, counter, f_n, QColor(0, 0, 0, 150), kWhite, s);
    }
    y += chip_h + int(40 * s);
    const QString place = clean(text(sc.value("place"))).toUpper();
    if (!place.isEmpty()) {
        const Face f_p = cardkit::loadFont(su.bold, int(30 * s), true);
        const int dot = int(12 * s);
        const QRect d = pilRect(safe_x, y + int(12 * s), safe_x + dot, y + int(12 * s) + dot);
        ellipse(img, d, opaque(su.accent));
        if (t) t->addBox(QStringLiteral("placeDot"), d);
        spaced(img, t, "place", QPoint(safe_x + dot + int(14 * s), y), place, f_p, QColor(255, 255, 255, 235), int(4 * s));
        y += int(30 * s * 1.6);
    }
    const QString headline = clean(truthy(sc.value("headline")) ? pyStr(sc.value("headline")) : text(sc.value("title")));
    if (!headline.isEmpty()) {
        const int big = boxed ? 60 : 74, small = boxed ? 52 : 62, max_l = boxed ? 4 : 3;   // the boxed column is narrower
        int size = int(big * s);
        Face f_h = cardkit::loadFont(su.bold, size, true);
        QStringList lines = cardkit::wrap(headline, f_h, col_w, max_l);
        if (lines.size() == max_l && (pyLen(headline) > 26 * max_l || lines.last().endsWith(QLatin1String("...")))) {
            size = int(small * s);
            f_h = cardkit::loadFont(su.bold, size, true);
            lines = cardkit::wrap(headline, f_h, col_w, max_l);
        }
        for (const QString& ln : lines) {
            shadow(img, t, "headline", QPoint(safe_x, y), ln, f_h, kWhite, int(7 * s), QPoint(0, int(3 * s)));
            y += int(size * 1.1);
        }
        y += int(20 * s);
    }
    if (truthy(sc.value("rows")))                              // a quote table (markets)
        y = drawRows(img, t, sc.value("rows"), safe_x, y + int(10 * s), W - 2 * safe_x, s, su) + int(20 * s);
    const QString detail = clean(text(sc.value("detail")));    // showtimes, the score, the price
    if (!detail.isEmpty()) {
        const bool big = truthy(sc.value("spark"));            // a price next to its chart is the headline number
        const int px = big ? 88 : 46;
        const Face f_d = cardkit::loadFont(su.bold, int(px * s), true);
        const QColor dc = opaque(toneColor(text(sc.value("tone"))));
        for (const QString& ln : cardkit::wrap(detail, f_d, col_w, 2)) {
            shadow(img, t, "detail", QPoint(safe_x, y), ln, f_d, dc, int(6 * s), QPoint(0, int(3 * s)));
            y += int(px * s * 1.18);
        }
        y += int(14 * s);
    }
    if (truthy(sc.value("spark"))) {
        const int pw = int(W * 0.40), ph = int(H * 0.40);
        const int px = W - safe_x - pw, py = int(250 * s);
        roundedPanel(img, t, "sparkPanel", pilRect(px - int(28 * s), py - int(24 * s), px + pw + int(28 * s), py + ph + int(64 * s)), int(20 * s), QColor(6, 12, 24, 128));
        drawSpark(img, t, px, py, pw, ph, sc.value("spark"), toneColor(text(sc.value("tone"))), s, clean(text(sc.value("sparkLabel"))));
    }
    const QString summary = clean(text(sc.value("summary")));
    if (!summary.isEmpty()) {
        const int fs = int((boxed ? 32 : 36) * s);
        const Face f_s = cardkit::loadFont(su.reg, fs);
        const int step = int(fs * 1.42);
        const int max_lines = std::max(1, std::min(boxed ? 4 : 3, fdiv(strap_top - int(30 * s) - y, std::max(1, step))));
        for (const QString& ln : cardkit::wrap(summary, f_s, col_w, max_lines)) {
            shadow(img, t, "summary", QPoint(safe_x, y), ln, f_s, QColor(226, 233, 243), int(5 * s), QPoint(0, int(2 * s)), 150);
            y += step;
        }
    }
    drawStrap(img, spec, su, footerOf(spec), t);
    addTextBoxes(t, img);
    return img;
}

QImage renderIntro(const QJsonObject& spec, const QJsonObject& sc, bool photo, Trace* t) {
    const Setup su = setup(spec);
    const int W = su.W, H = su.H;
    const double s = su.s;
    QImage img = cardkit::newCanvas(W, H);
    const int safe_x = int(W * 0.058);
    cardkit::wash(img, su.ink, photo ? 120 : 30);
    if (t) t->addBox(QStringLiteral("wash"), img.rect());
    int y = int(196 * s);
    const cardkit::Chip c = chip(img, t, "eyebrow", safe_x, y, clean(orDefault(sc.value("eyebrow"), QStringLiteral("TOP STORIES"))).toUpper(),
                                 cardkit::loadFont(su.bold, int(28 * s), true), opaque(su.accent), kWhite, s);
    y += c.height + int(18 * s);
    QString title = clean(orDefault(spec.value("kicker"), QStringLiteral("HEADLINE NEWS"))).toUpper();
    const Face f_t = cardkit::loadFont(su.bold, int(128 * s), true);
    title = cardkit::ellipsize(title, f_t, int(W * 0.86));
    shadow(img, t, "title", QPoint(safe_x - int(4 * s), y), title, f_t, kWhite, int(9 * s), QPoint(0, int(4 * s)));
    y += int(128 * s * 1.12);
    setRect(img, t, "rule", pilRect(safe_x, y, safe_x + int(180 * s), y + int(9 * s)), opaque(su.accent));
    y += int(30 * s);
    const QString siteName = clean(truthy(spec.value("siteName")) ? pyStr(spec.value("siteName")) : text(spec.value("brandName")));
    QString sub = clean(text(sc.value("sub")));                // a scene's own sub line replaces "from <site>"
    if (sub.isEmpty() && !siteName.isEmpty()) sub = QStringLiteral("from ") + siteName;
    if (!sub.isEmpty()) {
        const Face f_sub = cardkit::loadFont(su.reg, int(44 * s));
        shadow(img, t, "sub", QPoint(safe_x, y), cardkit::ellipsize(sub, f_sub, int(W * 0.86)), f_sub, QColor(214, 222, 234), int(5 * s), QPoint(0, int(2 * s)), 150);
        y += int(44 * s * 1.3);
    }
    const QString date = clean(text(spec.value("date")));
    if (!date.isEmpty()) {
        spaced(img, t, "date", QPoint(safe_x, y + int(4 * s)), date.toUpper(), cardkit::loadFont(su.bold, int(30 * s), true), QColor(236, 241, 248), int(3 * s));
        y += int(30 * s * 1.7);
    }
    y += int(24 * s);
    QStringList rundown;
    for (const QJsonValue& v : sc.value("rundown").toArray())
        if (!pyStrip(pyStr(v)).isEmpty() && rundown.size() < 5) rundown << clean(pyStr(v));
    const Face f_r = cardkit::loadFont(su.reg, int(32 * s));
    const Face f_n = cardkit::loadFont(su.bold, int(26 * s), true);
    const int box = int(42 * s);
    const int strap_top = H - int(84 * s) - int(26 * s);
    for (int i = 0; i < rundown.size(); ++i) {
        if (y + box > strap_top - int(16 * s)) break;
        setRect(img, t, "rundownNumber", pilRect(safe_x, y, safe_x + box, y + box), opaque(su.accent));
        const QString num = QString::number(i + 1);
        textAt(img, t, QPoint(safe_x + fdiv(box - f_n.width(num), 2), y + int(5 * s)), num, f_n, kWhite);
        shadow(img, t, "rundown", QPoint(safe_x + box + int(20 * s), y + int(1 * s)), cardkit::ellipsize(rundown[i], f_r, int(W * 0.80)), f_r,
               QColor(236, 241, 248), int(4 * s), QPoint(0, int(2 * s)), 140);
        y += int(58 * s);
    }
    drawStrap(img, spec, su, clean(text(spec.value("site"))), t);
    addTextBoxes(t, img);
    return img;
}

QImage renderOutro(const QJsonObject& spec, const QJsonObject& sc, bool photo, Trace* t) {
    const Setup su = setup(spec);
    const int W = su.W, H = su.H;
    const double s = su.s;
    QImage img = cardkit::newCanvas(W, H);
    const int safe_x = int(W * 0.058);
    cardkit::wash(img, su.ink, photo ? 150 : 30);
    if (t) t->addBox(QStringLiteral("wash"), img.rect());
    const QString site = clean(text(spec.value("site")));
    const QString name = clean(truthy(spec.value("brandName")) ? pyStr(spec.value("brandName")) : text(spec.value("siteName")));
    int y = int(318 * s);
    QString eyebrow = clean(text(sc.value("eyebrow"))).toUpper();
    if (eyebrow.isEmpty()) eyebrow = site.isEmpty() ? QStringLiteral("THAT'S THE LATEST") : QStringLiteral("FOR MORE ON THESE STORIES");
    spaced(img, t, "eyebrow", QPoint(safe_x, y), eyebrow, cardkit::loadFont(su.bold, int(38 * s), true), opaque(su.accent), int(5 * s));
    y += int(38 * s * 1.8);
    QString big = clean(text(sc.value("big")));
    if (big.isEmpty()) big = !site.isEmpty() ? site : !name.isEmpty() ? name : QStringLiteral("HEADLINE NEWS");
    const Face f_b = cardkit::loadFont(su.bold, int(124 * s), true);
    big = cardkit::ellipsize(big, f_b, int(W * 0.86));
    shadow(img, t, "big", QPoint(safe_x - int(4 * s), y), big, f_b, kWhite, int(9 * s), QPoint(0, int(4 * s)));
    y += int(124 * s * 1.18);
    setRect(img, t, "rule", pilRect(safe_x, y, safe_x + f_b.width(big), y + int(10 * s)), opaque(su.accent));
    y += int(44 * s);
    QString sub = clean(text(sc.value("sub")));                // phone, address, source: replaces the site name
    if (sub.isEmpty() && !name.isEmpty() && name.toLower() != big.toLower()) sub = name;
    if (!sub.isEmpty()) {
        const Face f_sub = cardkit::loadFont(su.reg, int(42 * s));
        for (const QString& ln : cardkit::wrap(sub, f_sub, int(W * 0.86), 2)) {
            shadow(img, t, "sub", QPoint(safe_x, y), ln, f_sub, QColor(206, 216, 228), int(5 * s), QPoint(0, int(2 * s)), 150);
            y += int(42 * s * 1.3);
        }
    }
    drawStrap(img, spec, su, clean(text(spec.value("date"))), t);
    addTextBoxes(t, img);
    return img;
}

QImage backdrop(const QImage& rgb, double dim) {
    QImage small = pilCover(rgb, kBedW / 8, kBedH / 8, 0.5);
    blur::gaussianBlur(small, 5.0f);
    return imageio::brightness(pilResize(small, QSize(kBedW, kBedH), Filter::Bicubic), dim).convertToFormat(QImage::Format_RGB32);
}

PhotoBed photoBed(const QString& photo, const QString& kind, const QString& storyLayout) {
    const QImage im = loadRgb(photo);
    const int w = im.width(), h = im.height();
    if (w < 80 || h < 80) raise(QStringLiteral("photo too small (%1x%2)").arg(w).arg(h));
    PhotoBed r;
    r.photoSize = QSize(w, h);
    if (kind == QLatin1String("intro") || kind == QLatin1String("outro")) {
        r.image = backdrop(im, 0.42); r.layout = QStringLiteral("backdrop");
    } else if (storyLayout == QLatin1String("box")) {
        r.image = backdrop(im, 0.55); r.layout = QStringLiteral("box");
    } else if (double(w) / h >= 1.5) {                         // landscape: fill the frame, cropped a little above centre
        r.image = pilCover(im, kBedW, kBedH, 0.4); r.layout = QStringLiteral("cover");
    } else {                                                   // square / portrait: inset on the right over a blurred copy
        r.layout = QStringLiteral("inset");
        QImage bed = backdrop(im, 0.5);
        int fh = int(kBedH * 0.92);
        int fw = int(std::nearbyint(double(w) * fh / h));
        if (fw > int(kBedW * 0.5)) { fw = int(kBedW * 0.5); fh = int(std::nearbyint(double(h) * fw / w)); }
        const int x = kBedW - fw - int(kBedW * 0.10), y = (kBedH - fh) / 2;
        blurredRect(bed, nullptr, "", pilRect(x - 6, y + 14, x + fw + 6, y + fh + 26), QColor(0, 0, 0, 170), 28);
        QPainter p(&bed);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(x, y, pilResize(im, QSize(fw, fh), Filter::Lanczos));
        p.end();
        r.image = bed;
    }
    r.image = r.image.convertToFormat(QImage::Format_RGB32);
    return r;
}

bool savePng(const QImage& img, const QString& path, QString* error) {
    auto failWith = [&](const QString& e) { if (error) *error = e; return false; };
    if (img.isNull()) return failWith(QStringLiteral("no picture to write to %1").arg(path));
    const QString dir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dir)) return failWith(QStringLiteral("cannot create %1").arg(dir));
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return failWith(QStringLiteral("cannot write %1: %2").arg(path, f.errorString()));
    QImageWriter w(&f, "png");
    w.setCompression(35);                                      // zlib level 3, as imageio::save (compress_level=3)
    if (!w.write(img)) { f.cancelWriting(); return failWith(QStringLiteral("cannot encode %1: %2").arg(path, w.errorString())); }
    if (!f.commit()) return failWith(QStringLiteral("cannot write %1: %2").arg(path, f.errorString()));
    return true;
}

QString resolvedPath(const QString& p) {
    const QFileInfo fi(p);
    const QString c = fi.canonicalFilePath();
    return QDir::toNativeSeparators(c.isEmpty() ? QDir::cleanPath(fi.absoluteFilePath()) : c);
}

QJsonObject runCard(const QString& specPath, const QString& out, const QString& bed, Trace* t) {
    if (specPath.isEmpty() || out.isEmpty()) return ppcli::failure(QStringLiteral("--spec and --out are required (or --scenes)"));
    QJsonObject spec;
    try { spec = readSpec(specPath, "bad spec: "); } catch (const std::exception& e) { return ppcli::failure(what(e)); }
    try {
        FurnitureInfo info;
        const QImage img = renderFurniture(spec, &info, t);
        QString err;
        if (!savePng(img, out, &err)) raise(err);
        QString bedOut;
        if (!bed.isEmpty()) {
            const Setup su = setup(spec);
            if (!savePng(cardkit::gradientBed(su.W, su.H, su.accent, su.ink), bed, &err)) raise(err);
            bedOut = resolvedPath(bed);
        }
        return QJsonObject{{"ok", true}, {"furniture", resolvedPath(out)}, {"bed", bedOut},
                           {"headlineLines", QJsonArray::fromStringList(info.headlineLines)}, {"lines", info.lines}};
    } catch (const std::exception& e) {
        ppcli::log(what(e));
        return ppcli::failure(what(e));
    }
}

QJsonObject runScenes(const QString& scenesPath, QList<Trace>* traces) {
    try {
        const QJsonObject spec = readSpec(scenesPath, "");
        const QJsonValue scenesV = spec.value("scenes");
        if (truthy(scenesV) && !scenesV.isArray()) raise(QStringLiteral("scenes must be a list"));
        const QJsonArray scenes = scenesV.toArray();
        for (int i = 0; i < scenes.size(); ++i)
            if (!scenes[i].isObject()) raise(QStringLiteral("scene %1 is not an object").arg(i));
        // Animated scenes are weather_cards.py's (DEP-09): refuse the whole spec before writing anything, so the
        // Supervisor runs the script for all of it rather than mixing the two renderers in one spot.
        for (int i = 0; i < scenes.size(); ++i)
            if (truthy(scenes[i].toObject().value("anim")))
                return ppcli::unsupportedResult(QStringLiteral("animated scene (weather_cards.py)"), QJsonObject{{"scene", i}});
        const Setup su = setup(spec);
        const QString storyLayout = orDefault(spec.value("storyLayout"), QStringLiteral("box")).toLower();

        // The gradient bed depends on the frame and the colours only: painted once, written per scene.
        std::mutex bedMu;
        QImage gradient;
        auto gradientBed = [&]() { std::lock_guard<std::mutex> l(bedMu); if (gradient.isNull()) gradient = cardkit::gradientBed(su.W, su.H, su.accent, su.ink); return gradient; };

        auto renderOne = [&](int i, Trace* t) -> QJsonObject {
            const QJsonObject sc = scenes[i].toObject();
            const QString kind = orDefault(sc.value("kind"), QStringLiteral("story"));
            const QString photo = text(sc.value("photo"));
            QString bed, layout = QStringLiteral("gradient");
            QJsonValue size = QJsonValue::Null;
            if (!photo.isEmpty() && QFileInfo(photo).isFile()) {
                try {
                    const QString base = required(sc, "bedBase");
                    const PhotoBed pb = photoBed(photo, kind, storyLayout);
                    const QString out = base + QStringLiteral(".jpg");
                    QString err;
                    if (!imageio::save(pb.image, out, &err, 90)) raise(err);
                    bed = out; layout = pb.layout;
                    size = QJsonArray{pb.photoSize.width(), pb.photoSize.height()};
                } catch (const std::exception& e) {
                    ppcli::log(QStringLiteral("scene %1: photo unusable (%2); gradient bed").arg(i).arg(what(e)));
                    bed.clear(); layout = QStringLiteral("gradient");
                }
            }
            if (bed.isEmpty()) {
                bed = required(sc, "bedBase") + QStringLiteral(".png");
                QString err;
                if (!savePng(gradientBed(), bed, &err)) raise(err);
            }
            const QString furniture = required(sc, "furniture");
            const bool hasPhoto = layout != QLatin1String("gradient");
            const QImage img = kind == QLatin1String("intro") ? renderIntro(spec, sc, hasPhoto, t)
                             : kind == QLatin1String("outro") ? renderOutro(spec, sc, hasPhoto, t)
                             : renderStory(spec, sc, hasPhoto, layout, layout == QLatin1String("box") ? photo : QString(), t);
            QString err;
            if (!savePng(img, furniture, &err)) raise(err);
            return QJsonObject{{"kind", kind}, {"furniture", resolvedPath(furniture)}, {"bed", resolvedPath(bed)},
                               {"photo", hasPhoto}, {"layout", layout}, {"photoSize", size}};
        };

        // Scenes are independent files, so they render on a few threads (the card kit is reentrant; DEP-08a checks
        // threaded output is byte-identical). The script stops at the first failing scene; here no new scene starts
        // after a failure, and the first failure by scene order is the one reported.
        const int n = int(scenes.size());
        int threads = std::clamp(QThread::idealThreadCount(), 1, 4);
        bool ok = false;
        const int env = qEnvironmentVariableIntValue("PLAYPLAY_SPOTRENDER_THREADS", &ok);
        if (ok && env > 0) threads = std::min(env, 16);
        threads = std::max(1, std::min(threads, n));
        const size_t count = size_t(n);
        std::vector<QJsonObject> made(count);
        std::vector<QString> errors(count);
        std::vector<Trace> trs(count);
        std::atomic<int> next{0};
        std::atomic<bool> failed{false};
        auto worker = [&]() {
            for (int i; !failed.load() && (i = next.fetch_add(1)) < n;) {
                try { made[size_t(i)] = renderOne(i, traces ? &trs[size_t(i)] : nullptr); }
                catch (const std::exception& e) { errors[size_t(i)] = what(e); failed = true; }
                catch (...) { errors[size_t(i)] = QStringLiteral("scene %1: render failed").arg(i); failed = true; }
            }
        };
        std::vector<std::thread> pool;
        for (int k = 1; k < threads; ++k) pool.emplace_back(worker);
        worker();
        for (std::thread& th : pool) th.join();
        for (int i = 0; i < n; ++i)
            if (!errors[size_t(i)].isEmpty()) raise(errors[size_t(i)]);
        QJsonArray done;
        for (int i = 0; i < n; ++i) {
            done.append(made[size_t(i)]);
            if (traces) traces->append(trs[size_t(i)]);
        }
        return QJsonObject{{"ok", true}, {"scenes", done}};
    } catch (const std::exception& e) {
        ppcli::log(what(e));
        return ppcli::failure(what(e));
    }
}

// ---------------------------------------------------------------------------------------------- command lines
namespace {

const char* const kCardUsage =
    "usage: PlayPlaySpotRender card [-h] [--spec SPEC] [--out OUT] [--bed BED] [--scenes SCENES]\n\n"
    "scripts/spot_card.py: the furniture plane of a spot (1920x1080 RGBA PNG) and, with --bed, the gradient bed;\n"
    "--scenes paints every scene of a Headline News scenes.json (furniture PNG + 2880x1620 photo bed or gradient bed).\n"
    "Prints one JSON line: {ok, furniture, bed, headlineLines, lines} / {ok, scenes: [...]}. Exit 0 / 2; 64 for an\n"
    "option or an animated (weather) scene this build does not render.\n";

// argparse for spot_card.py's four options: "--opt value", "--opt=value", unique prefixes; the last one given wins.
struct Parsed { QHash<QString, QString> v; bool help = false; int exit = -1; QJsonObject result; };

Parsed parseArgs(const QStringList& args, const QStringList& names, bool positionalScenes) {
    Parsed r;
    auto unsupported = [&r](const QString& a) { r.result = ppcli::unsupportedResult(a); r.exit = ppcli::ExitUnsupported; return r; };
    auto failWith = [&r](const QString& e) { r.result = ppcli::failure(e); r.exit = ppcli::ExitFailed; return r; };
    for (qsizetype i = 0; i < args.size(); ++i) {
        const QString a = args[i];
        if (a == QLatin1String("-h") || a == QLatin1String("--help")) { r.help = true; continue; }
        if (a.startsWith(QLatin1String("--")) && a.size() > 2) {
            const qsizetype eq = a.indexOf(QLatin1Char('='));
            const QString key = eq < 0 ? a : a.left(eq);
            QStringList hits;
            for (const QString& n : names) { if (n == key) { hits = {n}; break; } if (n.startsWith(key)) hits << n; }
            if (hits.isEmpty()) return unsupported(a);
            if (hits.size() > 1) return failWith(QStringLiteral("ambiguous option: %1 could match %2").arg(key, hits.join(QLatin1String(", "))));
            QString value;
            if (eq >= 0) value = a.mid(eq + 1);
            else {
                static const QRegularExpression negNum(QStringLiteral("^-\\d+$|^-\\d*\\.\\d+$"));
                if (i + 1 >= args.size() || (args[i + 1].startsWith(QLatin1Char('-')) && !negNum.match(args[i + 1]).hasMatch()))
                    return failWith(QStringLiteral("argument %1: expected one argument").arg(hits.first()));
                value = args[++i];
            }
            r.v.insert(hits.first(), value);
            continue;
        }
        if (positionalScenes && !a.startsWith(QLatin1Char('-')) && !r.v.contains(QStringLiteral("--scenes"))) { r.v.insert(QStringLiteral("--scenes"), a); continue; }
        return unsupported(a);                                 // argparse: unrecognized arguments
    }
    return r;
}

int printUsage() {
    fwrite(kCardUsage, 1, std::strlen(kCardUsage), stdout);
    fflush(stdout);
    return ppcli::ExitOk;
}

}   // namespace

int cardCli(const QStringList& args) {
    const Parsed p = parseArgs(args, {QStringLiteral("--spec"), QStringLiteral("--out"), QStringLiteral("--bed"), QStringLiteral("--scenes")}, false);
    if (p.help) return printUsage();
    if (p.exit >= 0) return ppcli::finish(p.result);
    const QString scenes = p.v.value(QStringLiteral("--scenes"));
    if (!scenes.isEmpty()) return ppcli::finish(runScenes(scenes));
    return ppcli::finish(runCard(p.v.value(QStringLiteral("--spec")), p.v.value(QStringLiteral("--out")), p.v.value(QStringLiteral("--bed"))));
}

int scenesCli(const QStringList& args) {
    const Parsed p = parseArgs(args, {QStringLiteral("--scenes")}, true);
    if (p.help) return printUsage();
    if (p.exit >= 0) return ppcli::finish(p.result);
    const QString scenes = p.v.value(QStringLiteral("--scenes"));
    if (scenes.isEmpty()) return ppcli::fail(QStringLiteral("a scenes.json is required (scenes <file> or --scenes <file>)"));
    return ppcli::finish(runScenes(scenes));
}

}   // namespace spotcard
