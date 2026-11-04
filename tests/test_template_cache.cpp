#include <QtTest/QtTest>
#include <QGuiApplication>
#include <QPainter>
#include <QFontMetrics>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <set>
#include "playout/engine/compositor/basicsources.h"
#include "playout/engine/mixer.h"

/**
 * PE-07 template render cache (docs/plans/perf-engine.md, lane perf-engine).
 *
 *   glyphSnapping          the font-engine property the ticker strip relies on: text drawn at a fractional x lands
 *                          exactly on TemplateSource::textPixelX(x), moved by whole pixels (Segoe UI / Arial / Consolas)
 *   tickerMatchesPerFrame  acceptance: the cached ticker against the per-frame render (TemplateSource::render, the
 *                          pre-PE-07 path) at 50 positions over a period + the 29.97 cadence + positions aimed at
 *                          QFixed's truncation (a copy just left of a pixel at negative x), straight and premultiplied
 *   tickerFallbacks        texts / sizes the strip cannot reproduce exactly fall back to the per-frame render
 *   tickerReusesPictures   a crawl that has not moved a whole pixel hands out the same picture (slow / stopped crawls)
 *   tickerUpdateRebuilds   update() of the text rebuilds the strip
 *   countdownChangesOnSecondBoundaries  a new picture exactly on the frames where the string changes, equal to render()
 *   wallClockRepaintsOncePerSecond      clock / targetUtcMs countdown on the real clock: a new picture iff the string changed
 *   switchOffIsPerFrame    PLAYPLAY_TEMPLATE_CACHE=0: every frame painted, as before
 *   cacheIsCheaper         video() cost with and without the cache (numbers in the log)
 *
 * Runs on the offscreen QPA (FreeType) unless QT_QPA_PLATFORM is set; ctest also runs it as
 * test_template_cache_directwrite with QT_QPA_PLATFORM=windows (DirectWrite, what the live engine draws text with).
 */
class TestTemplateCache : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void glyphSnapping();
    void tickerMatchesPerFrame_data();
    void tickerMatchesPerFrame();
    void tickerFallbacks();
    void tickerReusesPictures();
    void tickerUpdateRebuilds();
    void countdownChangesOnSecondBoundaries();
    void wallClockRepaintsOncePerSecond();
    void switchOffIsPerFrame();
    void cacheIsCheaper();
};

namespace {

constexpr double kFrameS = 1001.0 / 30000.0;

/// Sets environment variables for a scope (a null value unsets) and puts the previous values back.
class EnvScope {
public:
    EnvScope(const char* name, const QByteArray& value) : m_name(name), m_old(qgetenv(name)), m_was(qEnvironmentVariableIsSet(name)) {
        if (value.isNull()) qunsetenv(name); else qputenv(name, value);
    }
    ~EnvScope() { if (m_was) qputenv(m_name.constData(), m_old); else qunsetenv(m_name.constData()); }
private:
    QByteArray m_name, m_old; bool m_was;
};

/// What the per-frame path hands out for `posSec`: render() converted the way video() converts it.
FramePtr perFrame(const QJsonObject& spec, const OutputFormat& fmt, double posSec, bool premul) {
    const QImage img = TemplateSource::render(spec, fmt, posSec);
    return premul ? Mixer::imageToFramePremultiplied(img) : Mixer::imageToFrame(img);
}

struct Diff { int max = 0; qint64 pixels = 0; bool sizeOk = true; };
Diff diff(const FramePtr& a, const FramePtr& b) {
    Diff d;
    if (!a || !b || a->width != b->width || a->height != b->height) { d.sizeOk = false; d.max = 255; return d; }
    for (int y = 0; y < a->height; ++y) {
        const uint8_t* ra = a->data[0] + size_t(y) * a->linesize[0];
        const uint8_t* rb = b->data[0] + size_t(y) * b->linesize[0];
        for (int x = 0; x < a->width; ++x) {
            int m = 0;
            for (int c = 0; c < 4; ++c) m = qMax(m, qAbs(int(ra[x * 4 + c]) - int(rb[x * 4 + c])));
            if (m) { ++d.pixels; d.max = qMax(d.max, m); }
        }
    }
    return d;
}

/// The pixel x of every copy of the crawl render() draws at posSec (the same loop, from tickerMetrics).
std::vector<int> copyPixels(const QJsonObject& m, double speed, double posSec) {
    const int labelW = m.value("labelW").toInt(), w = m.value("width").toInt(), textW = m.value("textW").toInt(), period = m.value("period").toInt();
    std::vector<int> xs;
    for (double x = labelW + w - std::fmod(posSec * speed, double(period)); x > labelW - textW; x -= period) {
        xs.push_back(TemplateSource::textPixelX(x));
        if (x < labelW) break;
    }
    return xs;
}

QJsonObject tickerSpec(const QJsonObject& extra = {}) {
    QJsonObject s{{"type", "template"}, {"kind", "ticker"}, {"label", "PERF"},
                  {"text", "Isolated engine benchmark ticker: a long crawl of headline text that keeps the painter busy for the whole width of the raster"}};
    for (auto it = extra.begin(); it != extra.end(); ++it) { if (it.value().isNull()) s.remove(it.key()); else s[it.key()] = it.value(); }
    return s;
}

QString longText(int headlines) {
    QStringList h;
    for (int i = 0; i < headlines; ++i) h << QString("Headline %1: council approves the budget, schools open late on Monday, Route %2 lane closures until Friday").arg(i + 1).arg(57 + i);
    return h.join(QStringLiteral("   •   "));
}

QImage drawTextAt(const QFont& f, const QString& text, double x, int w, int h) {
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
    QPainter p(&img); p.setRenderHint(QPainter::TextAntialiasing); p.setFont(f); p.setPen(Qt::white);
    p.drawText(QRectF(x, 0, QFontMetrics(f).horizontalAdvance(text) + 10, h), Qt::AlignVCenter | Qt::AlignLeft, text);
    p.end();
    return img;
}

/// True when `img` equals `ref` moved right by `dx` whole pixels (columns that exist in both).
bool equalsShifted(const QImage& img, const QImage& ref, int dx) {
    for (int y = 0; y < img.height(); ++y) {
        const uint32_t* a = reinterpret_cast<const uint32_t*>(img.constScanLine(y));
        const uint32_t* b = reinterpret_cast<const uint32_t*>(ref.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) { const int xr = x - dx; if (xr >= 0 && xr < ref.width() && a[x] != b[xr]) return false; }
    }
    return true;
}

} // namespace

void TestTemplateCache::initTestCase() {
    qInfo().noquote() << "platform" << QGuiApplication::platformName() << "- ticker font resolves to" << QFontInfo(QFont("Segoe UI", 32, QFont::DemiBold)).family();
    QVERIFY2(TemplateSource::renderCacheFromEnv(), "run without PLAYPLAY_TEMPLATE_CACHE=0");
}

// The strip is exact only if a copy of the text drawn at x looks exactly like the copy at a whole pixel, moved to
// textPixelX(x). The source checks this per text at run time (status "per-frame: sub-pixel text" otherwise); this
// case shows it holds for this box's font engine, so the strip is what the engine actually uses here.
void TestTemplateCache::glyphSnapping() {
    // textPixelX itself: QFixed truncation (x * 64 toward zero), then floor.
    QCOMPARE(TemplateSource::textPixelX(100.0), 100);
    QCOMPARE(TemplateSource::textPixelX(100.99), 100);
    QCOMPARE(TemplateSource::textPixelX(-49.0078125), -49);       // -3136.5 truncates to -3136 = -49.0
    QCOMPARE(TemplateSource::textPixelX(-49.015625), -50);
    QCOMPARE(TemplateSource::textPixelX(-0.25), -1);
    QCOMPARE(TemplateSource::textPixelX(-1e-9), 0);
    const int h = 64;
    const QString text = "Wj headline: crawl text, 0123456789 AVAWAy";
    struct F { const char* family; int weight; } fonts[] = {{"Segoe UI", QFont::DemiBold}, {"Arial", QFont::Normal}, {"Consolas", QFont::Bold}};
    for (const F& ff : fonts) {
        const QFont f(ff.family, int(h * 0.5), ff.weight);
        const int W = QFontMetrics(f).horizontalAdvance(text) + 400;
        const QImage ref = drawTextAt(f, text, 200.0, W, h);
        QStringList bad; int checked = 0;
        for (double base : {-60.0, -1.0, 0.0, 37.0}) {
            std::vector<double> xs;
            for (int k = 0; k < 64; ++k) xs.push_back(base + k / 64.0 + 1.0 / 256.0);   // every 1/64 step, off the grid
            for (double e : {1e-9, 1.0 / 256, 1.0 / 128, 1.0 / 64 + 1e-6}) { xs.push_back(base + 1.0 - e); xs.push_back(base + e); }
            for (double x : xs) {
                ++checked;
                const QImage img = drawTextAt(f, text, x, W, h);
                if (!equalsShifted(img, ref, TemplateSource::textPixelX(x) - 200)) bad << QString::number(x, 'f', 9);
            }
        }
        qInfo().noquote() << QString("glyphSnapping %1 (%2): %3 positions, %4 not at textPixelX%5").arg(ff.family, QFontInfo(f).family())
                             .arg(checked).arg(bad.size()).arg(bad.isEmpty() ? QString() : ": " + bad.mid(0, 8).join(' '));
        QVERIFY2(bad.isEmpty(), qPrintable(QString("%1 draws text at sub-pixel positions here: the ticker strip would fall back to per-frame").arg(ff.family)));
    }
}

void TestTemplateCache::tickerMatchesPerFrame_data() {
    QTest::addColumn<QJsonObject>("spec");
    QTest::addColumn<int>("rasterH");
    QTest::addColumn<bool>("premul");
    QTest::addColumn<bool>("mustStrip");
    const QJsonArray items{"Storm warning for Henry County until 9 pm", "Martinsville council meets Tuesday", "Danville schools: two-hour delay"};
    struct R { const char* name; QJsonObject extra; int rasterH; bool mustStrip; } rows[] = {
        {"bench ticker 1080p", {}, 1080, true},
        {"no label", QJsonObject{{"label", QJsonValue::Null}}, 1080, true},
        {"items, colours, translucent bg", QJsonObject{{"items", items}, {"label", "BREAKING"}, {"bgColor", "#80203040"}, {"color", "#ffe0e0"},
                                                       {"accentColor", "#c01020"}, {"labelColor", "#ffffff"}, {"speed", 200}}, 1080, true},
        {"right-to-left speed", QJsonObject{{"speed", -90}}, 1080, true},
        {"long crawl in chunks", QJsonObject{{"text", longText(9)}, {"speed", 600}}, 1080, true},
        {"720p, custom size and font", QJsonObject{{"height", 50}, {"fontSize", 20}, {"fontFamily", "Arial"}, {"padding", 10}}, 720, true},
        {"tall font (line taller than the ticker)", QJsonObject{{"height", 40}, {"fontSize", 40}, {"text", "HEADLINE NEWS CRAWL 2026"}}, 1080, false},
    };
    for (const R& r : rows)
        for (bool pm : {false, true})
            QTest::newRow(qPrintable(QString("%1 %2").arg(r.name, pm ? "premultiplied" : "straight"))) << tickerSpec(r.extra) << r.rasterH << pm << r.mustStrip;
}

// Acceptance: the cached ticker against the per-frame render (old path) at 50 positions over one period, plus 120 frames
// of 29.97 cadence and positions aimed at copies landing just left of a pixel at negative x (where QFixed truncates).
void TestTemplateCache::tickerMatchesPerFrame() {
    QFETCH(QJsonObject, spec);
    QFETCH(int, rasterH);
    QFETCH(bool, premul);
    QFETCH(bool, mustStrip);
    EnvScope pm("PLAYPLAY_PREMUL", premul ? QByteArray("1") : QByteArray());
    OutputFormat fmt; fmt.height = rasterH; fmt.width = rasterH == 720 ? 1280 : 1920;
    TemplateSource src(spec);
    QVERIFY(src.open(fmt));
    QCOMPARE(src.deliversPremultiplied(), premul);
    const QJsonObject m = TemplateSource::tickerMetrics(spec, fmt);
    const double speed = spec.value("speed").toDouble(120.0);
    const int period = m.value("period").toInt(), labelW = m.value("labelW").toInt(), w = m.value("width").toInt();

    std::vector<std::pair<QString, double>> positions;
    for (int i = 0; i < 50; ++i) positions.push_back({"spread", 3.0 + (period / std::abs(speed)) * i / 50.0});   // 50 over one period
    for (int f = 0; f < 120; ++f) positions.push_back({"cadence", f * kFrameS});
    // Copies whose x lands just below a whole pixel: negative (QFixed truncation: drawn one pixel right of floor) and positive.
    int truncTargets = 0;
    for (double target : {-5.0 - 1.0 / 128, -0.5 / 128, -137.0 - 1.0 / 128, double(labelW) - 1.0 / 128, 41.0 + 127.0 / 128}) {
        const double offset = std::fmod(double(labelW + w) - target, double(period));   // a copy at `target` (mod period)
        const double off = offset < 0 ? offset + period : offset;
        positions.push_back({"edge", (speed > 0 ? off : off - period) / speed});
        if (target < 0) ++truncTargets;
    }
    int worst = 0, differing = 0, truncHits = 0;
    qint64 diffPixels = 0;
    for (size_t i = 0; i < positions.size(); ++i) {
        const double pos = positions[i].second;
        const FramePtr got = src.video(qint64(i), pos);
        const FramePtr want = perFrame(spec, fmt, pos, premul);
        QVERIFY(got && want);
        QCOMPARE(src.nativeSize(), QSize(want->width, want->height));
        const Diff d = diff(got, want);
        QVERIFY2(d.sizeOk, "picture size differs");
        worst = qMax(worst, d.max); diffPixels += d.pixels; if (d.pixels) ++differing;
        QVERIFY2(d.max <= 2, qPrintable(QString("%1 position %2 (posSec %3): max diff %4 over %5 px").arg(positions[i].first).arg(i).arg(pos, 0, 'f', 6).arg(d.max).arg(d.pixels)));
        // count positions where some copy really sits at a negative x just below a whole pixel (the truncation case)
        const double offset = std::fmod(pos * speed, double(period));
        for (double x = labelW + w - offset; x > labelW - m.value("textW").toInt(); x -= period) {
            if (x < 0 && TemplateSource::textPixelX(x) != int(std::floor(x)) && x + m.value("textW").toInt() > labelW) { ++truncHits; break; }
            if (x < labelW) break;
        }
    }
    const QJsonObject st = src.status();
    const bool strip = st.value("cache").toString() == "strip";
    qInfo().noquote() << QString("ticker %1x%2 period %3 (%4): %5 positions, max diff %6, %7 positions differ (%8 px); truncation cases hit %9; cache '%10' paints %11 tiles %12")
        .arg(w).arg(m.value("height").toInt()).arg(period).arg(QString(premul ? "premultiplied" : "straight")).arg(positions.size()).arg(worst).arg(differing).arg(diffPixels)
        .arg(truncHits).arg(st.value("cache").toString()).arg(st.value("paints").toDouble()).arg(st.value("tiles").toInt());
    if (mustStrip) {
        QVERIFY2(strip, qPrintable("expected the strip, got cache '" + st.value("cache").toString() + "'"));
        // the strip paints its base and tile once, not once per frame; a long crawl keeps at most 6 chunks
        if (period <= 8192) QCOMPARE(st.value("paints").toDouble(), 2.0);
        else QVERIFY2(st.value("tiles").toInt() >= 1 && st.value("tiles").toInt() <= 6, "long crawl: chunks not bounded");
    }
    QVERIFY2(truncHits >= 1 || truncTargets == 0, "no position exercised the negative-x truncation case");
    QCOMPARE(worst, 0);    // bit for bit: the acceptance tolerance is ±2, the strip is exact
    src.close();
}

// Texts and sizes the strip could not reproduce exactly fall back to the per-frame render (same pixels, status says why).
void TestTemplateCache::tickerFallbacks() {
    OutputFormat fmt;
    struct C { const char* name; QJsonObject extra; QString cache; } cases[] = {
        {"multi-line text", QJsonObject{{"text", "first line\nsecond line"}}, "per-frame: multi-line text"},
        {"label wider than the ticker", QJsonObject{{"width", 120}, {"label", "A VERY LONG LABEL THAT FILLS IT"}}, "per-frame: no crawl area"},
        {"gap narrower than the glyphs", QJsonObject{{"width", 3}, {"label", QJsonValue::Null}, {"text", "WWW www W"}}, "per-frame: gap narrower than the glyphs"},
        {"line taller than the ticker, glyph left of its origin", QJsonObject{{"height", 30}, {"fontSize", 40}, {"text", "jjj jump"}}, QString()},
    };
    for (const C& c : cases) {
        const QJsonObject spec = tickerSpec(c.extra);
        TemplateSource src(spec);
        QVERIFY(src.open(fmt));
        for (int i = 0; i < 50; ++i) {
            const double pos = 0.173 * i;
            const FramePtr got = src.video(i, pos), want = perFrame(spec, fmt, pos, false);
            const Diff d = diff(got, want);
            QVERIFY2(d.sizeOk && d.max == 0, qPrintable(QString("%1: frame %2 differs (max %3)").arg(c.name).arg(i).arg(d.max)));
        }
        const QString cache = src.status().value("cache").toString();
        qInfo().noquote() << QString("fallback '%1': cache '%2' paints %3").arg(c.name, cache).arg(src.status().value("paints").toDouble());
        if (!c.cache.isEmpty()) QCOMPARE(cache, c.cache);
        else QVERIFY2(cache == "strip" || cache.startsWith("per-frame"), qPrintable(cache));   // strip only if no ink is cut
    }
}

// A crawl that has not moved by a whole pixel hands out the same picture (no copy, no new upload downstream).
void TestTemplateCache::tickerReusesPictures() {
    OutputFormat fmt;
    for (double speed : {10.0, 0.0}) {
        const QJsonObject spec = tickerSpec(QJsonObject{{"speed", speed}});
        TemplateSource src(spec);
        QVERIFY(src.open(fmt));
        const QJsonObject m = TemplateSource::tickerMetrics(spec, fmt);
        const AVFrame* last = nullptr; int changes = 0, expectChanges = 0;
        std::vector<int> lastXs;
        for (int f = 0; f < 90; ++f) {
            const double pos = f * kFrameS;
            const FramePtr got = src.video(f, pos);       // the previous picture is still held by the source: a new one has a new address
            const std::vector<int> xs = copyPixels(m, speed, pos);
            if (xs != lastXs) { ++expectChanges; lastXs = xs; }
            if (got.get() != last) { ++changes; last = got.get(); }
            QCOMPARE(diff(got, perFrame(spec, fmt, pos, false)).max, 0);
        }
        qInfo().noquote() << QString("speed %1 px/s: %2 new pictures in 90 frames (%3 whole-pixel positions), paints %4")
            .arg(speed).arg(changes).arg(expectChanges).arg(src.status().value("paints").toDouble());
        QCOMPARE(changes, expectChanges);
        if (speed == 0.0) QCOMPARE(changes, 1);
        QCOMPARE(src.status().value("paints").toDouble(), 2.0);           // base + one tile, nothing per frame
    }
}

void TestTemplateCache::tickerUpdateRebuilds() {
    OutputFormat fmt;
    QJsonObject spec = tickerSpec();
    TemplateSource src(spec);
    QVERIFY(src.open(fmt));
    QCOMPARE(diff(src.video(0, 1.0), perFrame(spec, fmt, 1.0, false)).max, 0);
    const double paints0 = src.status().value("paints").toDouble();
    const QJsonObject upd{{"text", "Updated crawl text after a headline refresh"}, {"label", "NEWS"}, {"speed", 150}};
    src.update(upd);
    for (auto it = upd.begin(); it != upd.end(); ++it) spec[it.key()] = it.value();
    for (int i = 0; i < 20; ++i) {
        const double pos = 1.0 + i * kFrameS;
        QCOMPARE(diff(src.video(i, pos), perFrame(spec, fmt, pos, false)).max, 0);
    }
    QCOMPARE(src.status().value("cache").toString(), QString("strip"));
    QCOMPARE(src.status().value("paints").toDouble(), paints0 + 2.0);     // a new base and tile, once
    // A kind change drops the strip; back to a ticker builds it again.
    src.update(QJsonObject{{"kind", "lower_third"}, {"title", "now a lower third"}});
    QVERIFY(src.video(30, 2.0));
    QVERIFY(!src.status().contains("cache"));
    src.update(QJsonObject{{"kind", "ticker"}});
    spec["kind"] = "ticker"; spec["title"] = "now a lower third";
    QCOMPARE(diff(src.video(31, 2.5), perFrame(spec, fmt, 2.5, false)).max, 0);
    QCOMPARE(src.status().value("cache").toString(), QString("strip"));
}

// Countdown on layer time: a new picture exactly on the frames where the string changes (second boundaries), equal to
// the per-frame render, and the picture size follows the string (1:00:00 -> 59:59).
void TestTemplateCache::countdownChangesOnSecondBoundaries() {
    OutputFormat fmt;
    for (const QJsonObject& spec : {QJsonObject{{"type", "template"}, {"kind", "countdown"}, {"seconds", 5}, {"label", "ON AIR IN"}},
                                    QJsonObject{{"type", "template"}, {"kind", "countdown"}, {"seconds", 3602.5}}}) {
        for (bool premul : {false, true}) {
            EnvScope pm("PLAYPLAY_PREMUL", premul ? QByteArray("1") : QByteArray());
            TemplateSource src(spec);
            QVERIFY(src.open(fmt));
            const AVFrame* last = nullptr; QString lastText; int changes = 0; QList<int> changeFrames;
            for (int f = 0; f < 200; ++f) {
                const double pos = f * kFrameS;
                const FramePtr got = src.video(f, pos);
                const QString text = TemplateSource::clockText(spec, pos);
                // independent check of the string: whole seconds left, truncated
                const qint64 s = qMax<qint64>(0, qint64((spec.value("seconds").toDouble() - pos) * 1000.0)) / 1000;
                QVERIFY2(text.endsWith(QString("%1:%2").arg((s / 60) % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'))), qPrintable(text));
                const bool changed = got.get() != last;
                QCOMPARE(changed, text != lastText);
                if (changed) { ++changes; changeFrames << f; last = got.get(); lastText = text; }
                const FramePtr want = perFrame(spec, fmt, pos, premul);
                QCOMPARE(src.nativeSize(), QSize(want->width, want->height));
                QCOMPARE(diff(got, want).max, 0);
            }
            // the string changes when pos crosses seconds - n: frame ceil((seconds - n) / frame period)
            for (int i = 1; i < changeFrames.size(); ++i) {
                const int f = changeFrames[i];
                const double secs = spec.value("seconds").toDouble();
                QVERIFY2(std::floor(secs - (f - 1) * kFrameS) != std::floor(secs - f * kFrameS) || std::floor(secs - f * kFrameS) <= 0,
                         qPrintable(QString("changed at frame %1, not on a second boundary").arg(f)));
            }
            qInfo().noquote() << QString("countdown %1s %2: %3 pictures in 200 frames at %4, paints %5")
                .arg(spec.value("seconds").toDouble()).arg(premul ? "premultiplied" : "straight").arg(changes)
                .arg([&] { QStringList l; for (int f : changeFrames) l << QString::number(f); return l.join(','); }())
                .arg(src.status().value("paints").toDouble());
            QCOMPARE(src.status().value("cache").toString(), QString("string"));
            QCOMPARE(src.status().value("paints").toDouble(), double(changes));
            QVERIFY(changes >= 6 && changes <= 8);        // 200 frames = 6.67 s
        }
    }
}

// Clock and a targetUtcMs countdown on the real wall clock: a new picture iff the string changed. The string is sampled
// right before and after each video() call; calls that straddle a second are not judged.
void TestTemplateCache::wallClockRepaintsOncePerSecond() {
    OutputFormat fmt;
    const qint64 target = QDateTime::currentMSecsSinceEpoch() + 10000;
    for (const QJsonObject& spec : {QJsonObject{{"type", "template"}, {"kind", "clock"}, {"format", "hh:mm:ss"}},
                                    QJsonObject{{"type", "template"}, {"kind", "countdown"}, {"targetUtcMs", double(target)}}}) {
        TemplateSource src(spec);
        QVERIFY(src.open(fmt));
        const AVFrame* last = nullptr; QString lastText; bool known = false;    // known: lastText is the string of `last`
        int pictures = 0, judged = 0, compared = 0;
        QElapsedTimer t; t.start();
        int f = 0;
        while (t.elapsed() < 2300) {
            const QString before = TemplateSource::clockText(spec, 0.0);
            const FramePtr got = src.video(f, f * kFrameS);
            const QString after = TemplateSource::clockText(spec, 0.0);
            const bool changed = got.get() != last;
            if (changed) { ++pictures; last = got.get(); }
            if (before == after) {
                // the call saw one string: the picture shows it, and it is new exactly when the string is
                if (known) { ++judged; QCOMPARE(changed, before != lastText); }
                lastText = before; known = true;
                const QImage ref = TemplateSource::render(spec, fmt, 0.0);
                if (TemplateSource::clockText(spec, 0.0) == before) { ++compared; QCOMPARE(diff(got, Mixer::imageToFrame(ref)).max, 0); }
            } else {
                known = false;                // straddled a second: either string may be on the picture
            }
            ++f;
            QThread::msleep(10);
        }
        qInfo().noquote() << QString("%1 on the wall clock: %2 calls over 2.3 s, %3 pictures (paints %4), %5 judged, %6 compared to render()")
            .arg(spec.value("kind").toString()).arg(f).arg(pictures).arg(src.status().value("paints").toDouble()).arg(judged).arg(compared);
        QVERIFY(pictures >= 2 && pictures <= 4);
        QVERIFY(judged > f / 2);
        QCOMPARE(src.status().value("paints").toDouble(), double(pictures));
    }
}

void TestTemplateCache::switchOffIsPerFrame() {
    EnvScope off("PLAYPLAY_TEMPLATE_CACHE", "0");
    QVERIFY(!TemplateSource::renderCacheFromEnv());
    OutputFormat fmt;
    const QJsonObject ticker = tickerSpec(QJsonObject{{"speed", 0}});
    const QJsonObject countdown{{"type", "template"}, {"kind", "countdown"}, {"seconds", 60}};
    for (const QJsonObject& spec : {ticker, countdown}) {
        TemplateSource src(spec);
        QVERIFY(src.open(fmt));
        std::vector<FramePtr> held;                              // all kept alive, so every new picture has its own address
        std::set<const AVFrame*> seen;
        for (int f = 0; f < 10; ++f) {
            const FramePtr got = src.video(f, 0.5);              // the same position every time
            held.push_back(got); seen.insert(got.get());
            QCOMPARE(diff(got, perFrame(spec, fmt, 0.5, false)).max, 0);
        }
        QCOMPARE(src.status().value("cache").toString(), QString("off"));
        QCOMPARE(src.status().value("paints").toDouble(), 10.0);  // painted every frame, as before PE-07
        QCOMPARE(int(seen.size()), 10);
    }
}

// Cost of video() per frame with and without the cache (the bench_engine template rows measure the same at 300 frames).
void TestTemplateCache::cacheIsCheaper() {
    OutputFormat fmt;
    auto measure = [&](const QJsonObject& spec, bool cache) {
        EnvScope env("PLAYPLAY_TEMPLATE_CACHE", cache ? QByteArray() : QByteArray("0"));
        TemplateSource src(spec);
        if (!src.open(fmt)) return -1.0;
        for (int i = 0; i < 5; ++i) src.video(i, i * kFrameS);    // warm-up, strip built
        std::vector<double> ms;
        for (int i = 5; i < 125; ++i) { QElapsedTimer t; t.start(); src.video(i, i * kFrameS); ms.push_back(t.nsecsElapsed() / 1e6); }
        std::sort(ms.begin(), ms.end());
        return ms[ms.size() / 2];
    };
    const QJsonObject ticker = tickerSpec();
    const QJsonObject countdown{{"type", "template"}, {"kind", "countdown"}, {"seconds", 3600}, {"label", "ON AIR IN"}};
    const double tOn = measure(ticker, true), tOff = measure(ticker, false);
    const double cOn = measure(countdown, true), cOff = measure(countdown, false);
    qInfo().noquote() << QString("video() median: ticker %1 ms cached vs %2 ms per frame; countdown %3 ms vs %4 ms").arg(tOn, 0, 'f', 4).arg(tOff, 0, 'f', 4).arg(cOn, 0, 'f', 4).arg(cOff, 0, 'f', 4);
    QVERIFY(tOn >= 0 && cOn >= 0);
    QVERIFY2(tOn < tOff, "the cached ticker is not cheaper");
    QVERIFY2(cOn < cOff, "the cached countdown is not cheaper");
}

int main(int argc, char** argv) {
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    TestTemplateCache t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_template_cache.moc"
