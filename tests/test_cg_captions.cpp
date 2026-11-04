#include <QtTest/QtTest>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <cstdlib>
#include "playout/core/framerate.h"
#include "playout/engine/compositor/compositor.h"
#include "playout/engine/compositor/basicsources.h"
#include "playout/engine/mixer.h"

/**
 * Caption overlay tests (TemplateSource kinds "karaoke" and "factbox"; no media needed).
 *
 *  - karaoke layout: words break into lines at the character budget and segment boundaries.
 *  - karaoke picture: opaque pixels in the title-safe bottom band while a line is live, the
 *    current word carries the highlight colour, and nothing is drawn after the last word.
 *  - factbox picture: panel-sized, accent header on top, bullet rows revealed progressively.
 *  - compositor: a karaoke layer is driven by the PROGRAM position (wantsProgramTime), not by
 *    seconds since it was shown; it is cut when the program item changes, frozen while the program
 *    is held (pause), skipped while it has no line, and its transcript stays out of the telemetry.
 *  - regressions of the Phase 9 review: line switch in contiguous speech, pixel-width line breaking,
 *    caption-box-only delivery, state key per mode, #AARRGGBB colours, fact box string lines and
 *    the picture decoded once.
 *  - PE-08: karaoke and fact box composite within ±2 per channel with PLAYPLAY_PREMUL=1 (premultiplied frames).
 * Rendered frames are written as PNGs to $PLAYPLAY_TEST_OUT (or <temp>/playplay_cg) for eyeballing.
 */
class TestCgCaptions : public QObject {
    Q_OBJECT

    static QString outDir() {
        QString d = qEnvironmentVariable("PLAYPLAY_TEST_OUT");
        if (d.isEmpty()) d = QDir::tempPath() + "/playplay_cg";
        QDir().mkpath(d);
        return d;
    }
    static OutputFormat fmt1080() { OutputFormat f; f.width = 1920; f.height = 1080; f.rate = FrameRate(30, 1); return f; }
    static QJsonObject word(const QString& w, double s, double e) { QJsonObject o; o["word"] = w; o["start"] = s; o["end"] = e; return o; }
    static QJsonObject karaokeSpec() {
        // Two segments; the first is long enough to split at the character budget.
        QJsonObject spec; spec["type"] = "template"; spec["kind"] = "karaoke"; spec["maxChars"] = 24; spec["maxLines"] = 2;
        QJsonArray segs;
        QJsonObject s1; s1["start"] = 30.0; s1["end"] = 34.0; s1["text"] = "Welcome back to the evening news on PLAY PLAY";
        QJsonArray w1; const QStringList t1 = {"Welcome", "back", "to", "the", "evening", "news", "on", "PLAY", "PLAY"};
        for (int i = 0; i < t1.size(); ++i) w1.append(word(t1[i], 30.0 + i * 0.44, 30.0 + i * 0.44 + 0.4));
        s1["words"] = w1; segs.append(s1);
        QJsonObject s2; s2["start"] = 35.0; s2["end"] = 37.0; s2["text"] = "Here is the weather";
        QJsonArray w2; const QStringList t2 = {"Here", "is", "the", "weather"};
        for (int i = 0; i < t2.size(); ++i) w2.append(word(t2[i], 35.0 + i * 0.5, 35.0 + i * 0.5 + 0.45));
        s2["words"] = w2; segs.append(s2);
        spec["segments"] = segs;
        return spec;
    }
    static QJsonObject factboxSpec() {
        QJsonObject spec; spec["type"] = "template"; spec["kind"] = "factbox"; spec["title"] = "By the numbers"; spec["icon"] = "★";
        QJsonArray lines; lines << "Population 13,400 (2024 estimate)" << "Founded 1793 along the Smith River" << "Home of the Martinsville Speedway" << "Elevation 1,010 ft";
        spec["lines"] = lines; spec["footer"] = "Source: county records"; spec["reveal"] = "lines"; spec["revealIntervalSec"] = 1.0; spec["revealDelaySec"] = 0.0;
        return spec;
    }
    // Count pixels with alpha > 0 inside `r` and return the first highlight-ish (yellow) pixel found.
    static int opaqueIn(const QImage& img, const QRect& r, bool* sawHighlight = nullptr) {
        int n = 0; bool hi = false;
        for (int y = r.top(); y < r.bottom(); ++y) for (int x = r.left(); x < r.right(); ++x) {
            const QRgb p = img.pixel(x, y);
            if (qAlpha(p) > 0) ++n;
            if (qAlpha(p) > 200 && qRed(p) > 200 && qGreen(p) > 170 && qBlue(p) < 90) hi = true;
        }
        if (sawHighlight) *sawHighlight = hi;
        return n;
    }
    // Count near-white, near-opaque pixels (the fact box row text is white on a dark translucent body: the body itself
    // has alpha > 0 everywhere, so opaqueIn() cannot tell a drawn row from an undrawn one).
    static int brightIn(const QImage& img, const QRect& r) {
        int n = 0;
        for (int y = r.top(); y < r.bottom(); ++y) for (int x = r.left(); x < r.right(); ++x) {
            const QRgb p = img.pixel(x, y);
            if (qAlpha(p) > 200 && qRed(p) > 180 && qGreen(p) > 180 && qBlue(p) > 180) ++n;
        }
        return n;
    }
    static QRgb px(const FramePtr& f, int x, int y) { return reinterpret_cast<const QRgb*>(f->data[0] + y * f->linesize[0])[x]; }
    // Bright pixels in the caption band of a composited frame (the program underneath is black).
    static int lit(const FramePtr& f) {
        int n = 0;
        for (int y = 800; y < 1020; y += 2) for (int x = 200; x < 1720; x += 2) { const QRgb p = px(f, x, y); if (qRed(p) + qGreen(p) + qBlue(p) > 60) ++n; }
        return n;
    }
    static std::unique_ptr<LayerSource> blackProgram(const OutputFormat& fmt) {
        QJsonObject spec; spec["type"] = "color"; spec["color"] = "#000000";
        std::unique_ptr<LayerSource> s = LayerSource::create(spec);
        if (s && !s->open(fmt)) s.reset();
        return s;
    }

private slots:
    void karaokeLayoutBreaksLines() {
        const TemplateSource::KaraokeLayout L = TemplateSource::karaokeLayout(karaokeSpec());
        QCOMPARE(int(L.words.size()), 13);
        QVERIFY2(L.lines.size() >= 3, qPrintable(QString("lines=%1").arg(L.lines.size())));
        // The second segment starts a new line even though characters would still fit.
        const TemplateSource::KWord& here = L.words[9];
        QCOMPARE(here.text, QString("Here"));
        QCOMPARE(L.lines[size_t(here.line)].first, 9);
        for (const auto& ln : L.lines) QVERIFY(ln.end >= ln.start);
        // Line visibility: before the first word nothing; during; after the tail nothing.
        QCOMPARE(TemplateSource::karaokeLineAt(L, karaokeSpec(), 10.0), -1);
        QVERIFY(TemplateSource::karaokeLineAt(L, karaokeSpec(), 31.0) >= 0);
        QCOMPARE(TemplateSource::karaokeLineAt(L, karaokeSpec(), 36.0), int(L.lines.size()) - 1);
        QCOMPARE(TemplateSource::karaokeLineAt(L, karaokeSpec(), 60.0), -1);
    }

    void karaokeTimeBase() {
        QJsonObject spec = karaokeSpec(); spec["inPointSec"] = 12.0; spec["syncOffsetSec"] = -0.5;
        QCOMPARE(TemplateSource::karaokeTime(spec, 20.0, true), 31.5);          // program: pos + inPoint + offset
        spec["startAtProgramSec"] = 18.0;
        QCOMPARE(TemplateSource::karaokeTime(spec, 2.0, false), 31.5);          // layer: pos + startAt + inPoint + offset
    }

    void karaokeRendersCurrentLine() {
        const OutputFormat fmt = fmt1080();
        QSize sz;
        // t = 31.0 -> the third word ("to") is live: 30.88..31.28
        QJsonObject spec = karaokeSpec(); spec["position"] = "bottom";
        const QImage img = TemplateSource::render(spec, fmt, 31.0, &sz);
        QCOMPARE(sz, QSize(1920, 1080));
        img.save(outDir() + "/karaoke_t31.png");
        bool hi = false;
        const int bottomBand = opaqueIn(img, QRect(0, 760, 1920, 1080 - 760 - 54), &hi);
        QVERIFY2(bottomBand > 5000, qPrintable(QString("opaque pixels in the bottom band: %1").arg(bottomBand)));
        QVERIFY2(hi, "the current word should carry the highlight colour");
        QCOMPARE(opaqueIn(img, QRect(0, 0, 1920, 500)), 0);                    // nothing above the band
        // Title safe: nothing in the outer 5 % at the bottom.
        QCOMPARE(opaqueIn(img, QRect(0, 1080 - 50, 1920, 50)), 0);
        // Past the last word + tail: fully transparent.
        const QImage none = TemplateSource::render(spec, fmt, 60.0, &sz);
        QCOMPARE(opaqueIn(none, none.rect()), 0);
        // Top placement moves the box into the top band.
        spec["position"] = "top";
        const QImage top = TemplateSource::render(spec, fmt, 31.0, &sz);
        top.save(outDir() + "/karaoke_top_t31.png");
        QVERIFY(opaqueIn(top, QRect(0, 54, 1920, 300)) > 5000);
        QCOMPARE(opaqueIn(top, QRect(0, 600, 1920, 480)), 0);
    }

    void factboxRendersPanel() {
        const OutputFormat fmt = fmt1080();
        QJsonObject spec = factboxSpec(); spec["reveal"] = "all";
        QSize sz;
        const QImage img = TemplateSource::render(spec, fmt, 0.0, &sz);
        img.save(outDir() + "/factbox.png");
        QVERIFY(sz.width() > 520 && sz.width() < 620);                          // 520 panel + shadow margins
        QVERIFY(sz.height() > 200 && sz.height() < 700);
        QCOMPARE(sz, img.size());
        // Accent header: a pixel just inside the top-left corner of the panel is accent coloured (cyan by default).
        const QRgb hdr = img.pixel(18 + 40, 18 + 12);
        QVERIFY2(qAlpha(hdr) > 240 && qBlue(hdr) > 200 && qRed(hdr) < 80, qPrintable(QString("header px %1,%2,%3,%4").arg(qRed(hdr)).arg(qGreen(hdr)).arg(qBlue(hdr)).arg(qAlpha(hdr))));
        // Body is translucent dark, and there is text in the row area.
        QVERIFY(opaqueIn(img, QRect(40, 90, sz.width() - 80, sz.height() - 140)) > 1000);
        // Items variant renders too.
        QJsonObject items = factboxSpec(); items.remove("lines"); items["reveal"] = "all";
        QJsonArray arr; QJsonObject a; a["label"] = "Population"; a["value"] = "13,400"; arr.append(a); QJsonObject b; b["label"] = "Founded"; b["value"] = 1793; arr.append(b); items["items"] = arr;
        const QImage im2 = TemplateSource::render(items, fmt, 0.0, &sz);
        im2.save(outDir() + "/factbox_items.png");
        QVERIFY(opaqueIn(im2, im2.rect()) > 1000);
    }

    void factboxRevealsRows() {
        const QJsonObject spec = factboxSpec();
        QCOMPARE(TemplateSource::factboxRowCount(spec), 4);
        QCOMPARE(TemplateSource::factboxVisibleRows(spec, 0.0), 1);
        QCOMPARE(TemplateSource::factboxVisibleRows(spec, 1.5), 2);
        QCOMPARE(TemplateSource::factboxVisibleRows(spec, 10.0), 4);
        QJsonObject all = spec; all["reveal"] = "all";
        QCOMPARE(TemplateSource::factboxVisibleRows(all, 0.0), 4);
        // Fewer rows drawn early: fewer opaque text pixels, same panel size.
        const OutputFormat fmt = fmt1080(); QSize s0, s1;
        const QImage early = TemplateSource::render(spec, fmt, 0.0, &s0), late = TemplateSource::render(spec, fmt, 10.0, &s1);
        early.save(outDir() + "/factbox_reveal_1.png"); late.save(outDir() + "/factbox_reveal_4.png");
        QCOMPARE(s0, s1);
        const int earlyText = brightIn(early, QRect(40, 90, s0.width() - 80, s0.height() - 140)), lateText = brightIn(late, QRect(40, 90, s1.width() - 80, s1.height() - 140));
        QVERIFY2(earlyText > 100 && earlyText < lateText, qPrintable(QString("text pixels early=%1 late=%2").arg(earlyText).arg(lateText)));
    }

    void compositorUsesProgramTimeForKaraoke() {
        const OutputFormat fmt = fmt1080();
        Compositor c(fmt);
        Compositor::LayerSpec spec; spec.id = 31; spec.role = "lower_third"; spec.source = karaokeSpec();
        QString err;
        QVERIFY2(c.addLayer(spec, 0, &err), qPrintable(err));
        // Layer shown at frame 0; frame 1 is 1/30 s of layer time, but the program is at 31 s: captions on.
        MediaFrame on = c.render(1, 31.0);
        QVERIFY(on.video);
        int lit = 0;
        for (int y = 800; y < 1020; y += 2) for (int x = 200; x < 1720; x += 2) { const QRgb p = px(on.video, x, y); if (qRed(p) + qGreen(p) + qBlue(p) > 60) ++lit; }
        QVERIFY2(lit > 500, qPrintable(QString("lit=%1").arg(lit)));
        // Program at 100 s: nothing (black program + transparent caption raster).
        MediaFrame off = c.render(2, 100.0);
        int lit2 = 0;
        for (int y = 800; y < 1020; y += 2) for (int x = 200; x < 1720; x += 2) { const QRgb p = px(off.video, x, y); if (qRed(p) + qGreen(p) + qBlue(p) > 60) ++lit2; }
        QCOMPARE(lit2, 0);
        // Same program position twice: the source returns its cached frame (status reports the line).
        MediaFrame again = c.render(3, 31.0);
        QVERIFY(again.video);
        const QJsonArray layers = c.layersJson();
        QCOMPARE(layers.size(), 1);
        QVERIFY(layers[0].toObject().value("sourceStatus").toObject().value("programTime").toBool());
    }

    // ---- regressions ----------------------------------------------------------------------------

    // The pre-roll (leadSec) must not cut the last word of a line in contiguous speech.
    void karaokeLineSwitchWaitsForTheLastWord() {
        QJsonObject spec; spec["type"] = "template"; spec["kind"] = "karaoke"; spec["maxChars"] = 12; spec["breakOnSegments"] = false;
        QJsonArray ws; for (int i = 0; i < 8; ++i) ws.append(word(QString("word%1").arg(i), 10.0 + i * 0.4, 10.0 + (i + 1) * 0.4));
        spec["words"] = ws;                                   // two words per line, lines touch: line 0 = 10.0..10.8, line 1 = 10.8..11.6
        const TemplateSource::KaraokeLayout L = TemplateSource::karaokeLayout(spec);
        QCOMPARE(int(L.lines.size()), 4);
        QCOMPARE(TemplateSource::karaokeLineAt(L, spec, 9.8), 0);        // pre-roll of the first line
        QCOMPARE(TemplateSource::karaokeLineAt(L, spec, 10.6), 0);       // used to be 1: 0.25 s before the end of "word1"
        QCOMPARE(TemplateSource::karaokeLineAt(L, spec, 10.79), 0);
        QCOMPARE(TemplateSource::karaokeLineAt(L, spec, 10.81), 1);
        // With a real pause the pre-roll still applies: line 2 starts at 13.0, so it goes live at 12.75.
        QJsonArray gap; gap.append(word("a", 10.0, 10.4)); gap.append(word("b", 10.4, 10.8)); gap.append(word("c", 13.0, 13.4)); gap.append(word("d", 13.4, 13.8));
        QJsonObject g = spec; g["words"] = gap; g["maxChars"] = 48;
        const TemplateSource::KaraokeLayout G = TemplateSource::karaokeLayout(g);
        QCOMPARE(int(G.lines.size()), 2);
        QCOMPARE(TemplateSource::karaokeLineAt(G, g, 10.79), 0);
        QCOMPARE(TemplateSource::karaokeLineAt(G, g, 12.2), -1);         // past the 1.2 s tail, before the pre-roll
        QCOMPARE(TemplateSource::karaokeLineAt(G, g, 12.8), 1);
    }

    // Lines break at the measured width of the title-safe box, not only at maxChars.
    void karaokeBreaksByPixelWidth() {
        const OutputFormat fmt = fmt1080();
        QJsonObject spec = karaokeSpec(); spec["maxChars"] = 200; spec["fontSize"] = 100; spec["breakOnSegments"] = false; spec["lineGapSec"] = 100;
        const TemplateSource::KaraokeLayout byChars = TemplateSource::karaokeLayout(spec);
        const TemplateSource::KaraokeLayout byPixels = TemplateSource::karaokeLayout(spec, &fmt);
        QCOMPARE(int(byChars.lines.size()), 1);                          // 13 words in one 60-character line...
        QVERIFY2(byPixels.lines.size() >= 2, qPrintable(QString("lines=%1").arg(byPixels.lines.size())));   // ...which at 100 px is far wider than the screen
        QFont f("Segoe UI", -1, QFont::Bold); f.setPixelSize(100);
        const QFontMetrics fm(f);
        const int inner = 1920 - 96 * 2 - 22 * 2;                        // title-safe box minus padding
        for (size_t li = 0; li < byPixels.lines.size(); ++li) {
            const auto& ln = byPixels.lines[li];
            int w = 0; for (int i = ln.first; i <= ln.last; ++i) { w += fm.horizontalAdvance(byPixels.words[size_t(i)].text); if (i < ln.last) w += fm.horizontalAdvance(' '); }
            if (ln.last > ln.first) QVERIFY2(w <= inner, qPrintable(QString("line %1 is %2 px wide (max %3)").arg(li).arg(w).arg(inner)));
            // and the box that is rendered for it stays inside the 5 % margins
            QRect box; const QImage img = TemplateSource::renderKaraokeBox(spec, fmt, byPixels, int(li), ln.start + 0.05, 1.0, &box);
            QVERIFY(!img.isNull());
            QVERIFY2(box.left() >= 96 && box.right() < 1920 - 96, qPrintable(QString("box %1..%2").arg(box.left()).arg(box.right())));
        }
    }

    // video() hands the compositor the caption box only (canvas = raster), and says so when there is nothing to draw.
    void karaokeDeliversOnlyTheCaptionBox() {
        const OutputFormat fmt = fmt1080();
        TemplateSource src(karaokeSpec());
        QVERIFY(src.open(fmt));
        QVERIFY(!src.video(0, 10.0));                                     // before the first line: nothing, not a transparent raster
        QVERIFY(src.isBlank());
        const FramePtr f = src.video(1, 31.0);
        QVERIFY(f);
        QVERIFY(!src.isBlank());
        QCOMPARE(src.nativeSize(), QSize(1920, 1080));                    // the canvas: `position` is honoured inside it
        const QRect r = src.contentRect();
        QVERIFY(!r.isEmpty());
        QCOMPARE(f->width, r.width()); QCOMPARE(f->height, r.height());
        QVERIFY2(qint64(f->width) * f->height < qint64(1920) * 1080 / 4, "the picture should be the box, not the raster");
        QVERIFY(r.left() >= 96 && r.right() < 1920 - 96 && r.bottom() <= 1080 - 54);
        QVERIFY(!src.video(2, 100.0));                                    // past the tail
        QVERIFY(src.isBlank());
        // Nothing else leaks the old behaviour: the full-raster entry point still places the same box.
        QSize sz; const QImage full = TemplateSource::render(karaokeSpec(), fmt, 31.0, &sz);
        QCOMPARE(sz, QSize(1920, 1080));
        QVERIFY(opaqueIn(full, r) > 1000);
    }

    // The state key follows only what is visible: line mode / no fill must not re-render per word or fill step.
    void karaokeStateKeyFollowsWhatIsVisible() {
        const OutputFormat fmt = fmt1080();
        // `same`: the second call hands back the cached picture of the first (no re-render). A source that cannot
        // open or draw would make every `!same` below pass vacuously, so the probe at the end checks both frames exist.
        bool drew = true;
        auto same = [&](const QJsonObject& spec, double a, double b) {
            TemplateSource src(spec);
            if (!src.open(fmt)) { drew = false; return false; }
            const FramePtr fa = src.video(0, a), fb = src.video(1, b);
            if (!fa || !fb) drew = false;
            return fa && fa.get() == fb.get();
        };
        QJsonObject spec = karaokeSpec(); spec["fadeSec"] = 0.0;
        // Word "to" is live 30.88..31.28; "the" starts at 31.32.
        spec["mode"] = "word"; spec["fill"] = true;
        QVERIFY(!same(spec, 31.00, 31.12));                               // sliding fill: new step
        spec["fill"] = false;
        QVERIFY(same(spec, 31.00, 31.12));                                // same word, no fill: same picture
        QVERIFY(!same(spec, 31.00, 31.40));                               // next word: new colours
        spec["mode"] = "line"; spec["fill"] = true;
        QVERIFY(same(spec, 31.00, 31.12));
        QVERIFY(same(spec, 31.00, 31.40));                                // line mode: nothing depends on the word
        QVERIFY(drew);
    }

    // Colours are Qt #AARRGGBB (what QML sends and the docs write): the box keeps its 0xbe alpha.
    void karaokeColoursAreQtArgb() {
        const OutputFormat fmt = fmt1080();
        QJsonObject spec = karaokeSpec(); spec["bgColor"] = "#be05050a";
        const TemplateSource::KaraokeLayout L = TemplateSource::karaokeLayout(spec, &fmt);
        QRect box; const QImage img = TemplateSource::renderKaraokeBox(spec, fmt, L, TemplateSource::karaokeLineAt(L, spec, 31.0), 31.0, 1.0, &box);
        QVERIFY(!img.isNull());
        const QRgb p = img.pixel(img.width() / 2, 3);                     // inside the box, above the text
        QVERIFY2(qAlpha(p) >= 186 && qAlpha(p) <= 194, qPrintable(QString("alpha %1").arg(qAlpha(p))));
        QVERIFY2(qBlue(p) < 40, qPrintable(QString("blue %1 (the CSS reading #05050abe would give 190)").arg(qBlue(p))));
    }

    // `lines` as a newline-separated string (documented form) counts and draws like the array form.
    void factboxStringLinesAreCounted() {
        const OutputFormat fmt = fmt1080();
        QJsonObject s = factboxSpec(); s["reveal"] = "lines"; s["lines"] = "First fact\nSecond fact\n\nThird fact\r\n";
        QCOMPARE(TemplateSource::factboxRowCount(s), 3);
        QCOMPARE(TemplateSource::factboxVisibleRows(s, 0.0), 1);
        QCOMPARE(TemplateSource::factboxVisibleRows(s, 10.0), 3);
        QJsonObject arr = s; arr["lines"] = QJsonArray{"First fact", "Second fact", "Third fact"};
        QSize s1, s2;
        const QImage a = TemplateSource::render(s, fmt, 10.0, &s1), b = TemplateSource::render(arr, fmt, 10.0, &s2);
        QCOMPARE(s1, s2);
        QVERIFY(a == b);
        QVERIFY(brightIn(a, QRect(40, 90, s1.width() - 80, s1.height() - 140)) > 100);    // the body is not empty (rows are drawn)
    }

    // The fact box picture is decoded and scaled once (open), not on every reveal step on the clock thread.
    void factboxPictureIsDecodedOnce() {
        const OutputFormat fmt = fmt1080();
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString png = dir.filePath("pic.png");
        QImage pic(1200, 800, QImage::Format_ARGB32); pic.fill(QColor(200, 30, 30)); QVERIFY(pic.save(png));
        QJsonObject spec = factboxSpec(); spec["image"] = png; spec["revealDelaySec"] = 0.0; spec["revealIntervalSec"] = 1.0;
        QSize plain; TemplateSource::render([&] { QJsonObject p = spec; p.remove("image"); return p; }(), fmt, 0.0, &plain);
        TemplateSource src(spec);
        QVERIFY(src.open(fmt));
        QVERIFY(src.video(0, 0.0));                                       // reveal step 1
        const QSize withPicture = src.nativeSize();
        QVERIFY2(withPicture.height() > plain.height() + 100, "the picture should be part of the panel");
        QVERIFY(QFile::remove(png));                                      // gone from disk: later renders must not need it
        QVERIFY(src.video(1, 1.5));                                       // reveal step 2 re-renders the panel
        QCOMPARE(src.nativeSize(), withPicture);
        QVERIFY(src.video(2, 2.5));                                       // reveal step 3
        QCOMPARE(src.nativeSize(), withPicture);
    }

    // The layer list that rides the 1 Hz telemetry carries counts, not the transcript.
    void layersJsonCarriesNoTranscript() {
        const OutputFormat fmt = fmt1080();
        Compositor c(fmt);
        QJsonObject karaoke = karaokeSpec();
        QJsonArray words; for (int i = 0; i < 5000; ++i) words.append(word(QString("word%1").arg(i), i * 0.3, i * 0.3 + 0.25));
        QJsonObject seg; seg["start"] = 0.0; seg["end"] = 1500.0; seg["text"] = "long"; seg["words"] = words;
        karaoke["segments"] = QJsonArray{seg};
        Compositor::LayerSpec spec; spec.id = 31; spec.role = "lower_third"; spec.source = karaoke;
        QString err; QVERIFY2(c.addLayer(spec, 0, &err), qPrintable(err));
        const QJsonArray layers = c.layersJson();
        const QJsonObject src = layers[0].toObject().value("source").toObject();
        QVERIFY(!src.contains("segments"));
        QCOMPARE(src.value("segmentsCount").toInt(), 1);
        QCOMPARE(src.value("kind").toString(), QString("karaoke"));       // what the UIs read
        QCOMPARE(src.value("type").toString(), QString("template"));
        const int bytes = int(QJsonDocument(layers).toJson(QJsonDocument::Compact).size());
        QVERIFY2(bytes < 4000, qPrintable(QString("layers JSON is %1 bytes").arg(bytes)));
        // The layer still holds the full spec: its round trip keeps the words.
        QCOMPARE(c.layersJson().size(), 1);
    }

    // A take (item change) cuts the captions of the old item on the same frame.
    void programTakeCutsKaraoke() {
        const OutputFormat fmt = fmt1080();
        Compositor c(fmt);
        Compositor::LayerSpec spec; spec.id = 31; spec.role = "lower_third"; spec.source = karaokeSpec();
        QString err; QVERIFY2(c.addLayer(spec, 0, &err), qPrintable(err));
        QVERIFY(lit(c.render(1, 31.0).video) > 500);
        std::unique_ptr<LayerSource> next = blackProgram(fmt); QVERIFY(next);
        c.programTake(std::move(next), Compositor::Transition(), 2);
        QCOMPARE(c.layersJson()[0].toObject().value("state").toString(), QString("hidden"));
        QCOMPARE(lit(c.render(3, 31.0).video), 0);                        // the new item's position restarts: no replay of the old words
        // A layer that is not program-time (a plain lower third) is left alone.
        QJsonObject lt; lt["type"] = "template"; lt["kind"] = "lower_third"; lt["title"] = "Name";
        Compositor::LayerSpec ls; ls.id = 30; ls.role = "lower_third"; ls.source = lt;
        QVERIFY2(c.addLayer(ls, 4, &err), qPrintable(err));
        std::unique_ptr<LayerSource> next2 = blackProgram(fmt); QVERIFY(next2);
        c.programTake(std::move(next2), Compositor::Transition(), 5);
        for (const QJsonValue& v : c.layersJson()) if (v.toObject().value("id").toInt() == 30) QCOMPARE(v.toObject().value("state").toString(), QString("shown"));
    }

    // PE-08: the caption kinds (karaoke box with its translucent body, text shadows and sliding fill; the fact box with its
    // stacked drop shadow) composite the same with PLAYPLAY_PREMUL=1 as straight, within ±2 per channel, over a mid grey
    // program; unset and "0" are identical.
    void captionsPremultipliedMatchStraight() {
        const OutputFormat fmt = fmt1080();
        auto render = [&](const char* env, const QJsonObject& src, qint64 frame, double programPos, bool* premul) -> QImage {
            if (env) qputenv("PLAYPLAY_PREMUL", env); else qunsetenv("PLAYPLAY_PREMUL");
            Compositor c(fmt);
            std::unique_ptr<LayerSource> prog = LayerSource::create(QJsonObject{{"type", "color"}, {"color", "#808080"}});
            if (prog && prog->open(fmt)) c.programTake(std::move(prog), Compositor::Transition(), 0);
            Compositor::LayerSpec spec; spec.id = 31; spec.role = "lower_third"; spec.source = src;
            QString err; const bool ok = c.addLayer(spec, 0, &err);       // the source reads PLAYPLAY_PREMUL here
            qunsetenv("PLAYPLAY_PREMUL");
            if (!ok) return QImage();
            const MediaFrame mf = c.render(frame, programPos);
            *premul = c.layersJson()[0].toObject().value("sourceStatus").toObject().value("premultiplied").toBool();
            return QImage(mf.video->data[0], fmt.width, fmt.height, mf.video->linesize[0], QImage::Format_ARGB32).copy();
        };
        const struct { const char* name; QJsonObject src; qint64 frame; double pos; } cases[] = {
            {"karaoke", karaokeSpec(), 1, 31.3}, {"factbox", factboxSpec(), 75, 0.0}};
        for (const auto& k : cases) {
            bool pmUnset = true, pmOff = true, pmOn = false;
            const QImage unset = render(nullptr, k.src, k.frame, k.pos, &pmUnset), off = render("0", k.src, k.frame, k.pos, &pmOff), on = render("1", k.src, k.frame, k.pos, &pmOn);
            QVERIFY(!unset.isNull() && !off.isNull() && !on.isNull());
            QVERIFY(!pmUnset && !pmOff && pmOn);
            QVERIFY2(unset == off, k.name);
            int maxDiff = 0, differing = 0, drawn = 0;
            for (int y = 0; y < fmt.height; ++y) for (int x = 0; x < fmt.width; ++x) {
                const QRgb a = off.pixel(x, y), b = on.pixel(x, y);
                if (a != qRgb(128, 128, 128)) ++drawn;
                if (a == b) continue;
                ++differing;
                maxDiff = qMax(maxDiff, qMax(qMax(std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b))), qMax(std::abs(qBlue(a) - qBlue(b)), std::abs(qAlpha(a) - qAlpha(b)))));
            }
            qInfo().noquote() << QString("PE-08 %1: %2 pixels drawn over the program; straight vs premultiplied: %3 differ, max %4").arg(k.name).arg(drawn).arg(differing).arg(maxDiff);
            QVERIFY2(drawn > 5000, k.name);
            QVERIFY2(maxDiff <= 2, qPrintable(QString("%1: max diff %2").arg(k.name).arg(maxDiff)));
        }
    }

    // Pause: the program picture is held, and so are the captions that follow its position.
    void pauseFreezesKaraoke() {
        const OutputFormat fmt = fmt1080();
        Compositor c(fmt);
        Compositor::LayerSpec spec; spec.id = 31; spec.role = "lower_third"; spec.source = karaokeSpec();
        QString err; QVERIFY2(c.addLayer(spec, 0, &err), qPrintable(err));
        QVERIFY(lit(c.render(1, 31.0).video) > 500);
        c.setProgramHold(true);
        QVERIFY(lit(c.render(2, 100.0).video) > 500);                     // the engine keeps counting; the captions stay at 31 s
        QVERIFY(lit(c.render(3, 200.0).video) > 500);
        c.setProgramHold(false);
        QCOMPARE(lit(c.render(4, 200.0).video), 0);                       // resumed at a position with no line
    }
};

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    TestCgCaptions tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "test_cg_captions.moc"
