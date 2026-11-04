// PlayPlayButtonGallery (UXB-6, docs/plans/ux-buttons.md 3.11 and 5.4): a standalone viewer of the PlayPlay.Buttons module.
// Every component x role x size x state, the icon grid, the legacy-vs-module shim panel and the confirm popup, on pages of
// gallery.qml. No Supervisor, no network, no settings: it only reads the module it links (ui-buttons.qrc), the three UXB-5
// shims (qml/ui CButton / DeckButton / CToggle) and the frozen pre-shim copies it compares them with (gallery.qrc).
//
//   PlayPlayButtonGallery                                   interactive window (page chips, zoom, icon renderer, POSE)
//   PlayPlayButtonGallery --screenshot out.png [--zoom 1.5] [--page matrix|fills|composites|keys|icons|shim|popup|editor|all]
//   PlayPlayButtonGallery --bench [--bench-count 500] [--bench-frames 240]
//   PlayPlayButtonGallery --check --screenshot out.png      what the ctest runs (offscreen / software is fine)
//   --source                                                gallery.qml and the module from the source tree (no rebuild)
//
// --screenshot, --bench and --check never take the desktop: the window is a frameless tool window that does not accept focus,
// ignores input and sits far off screen (the live app runs on this machine). High-DPI scaling is off there, so one logical
// pixel is one image pixel and --zoom is the only scale (the item scale the app's uiScale applies to its root).
//
// --screenshot grabs each page in tiles of at most kTileW x kTileH (Windows will not size a window much past the desktop),
// stitches them, and stacks the pages for --page all (the default). pose() is called again before every tile.
// --bench creates the 500-key grid of IconButtons and the same grid of text-glyph PButtons, alternately, and reports
// creation (createObject, Repeater included), creation to the first frame, and the frame interval while the window redraws
// as fast as it can (swap interval 0, so the interval is the frame's real cost, GPU included). Gate (3.11): icon creation
// <= 120 ms and icon frame interval <= 1.10 x the glyph one; the exit code is 1 when it fails. The same grid of legacy
// CButtons (the pre-UXB-5 glyph key) and of their shim is created too and reported as a reference, not gated.
// --check fails (exit 1) on any QML warning or error, a page that does not load, an icon grid that misses or misnames an
// icon, a shim whose implicit size differs from the key it replaces by more than 2 px (UXB-5), or a blank page image.

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QImage>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QPainter>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr int kTileW = 1600, kTileH = 1000;
const QStringList kPages = { "matrix", "fills", "composites", "keys", "icons", "shim", "popup", "editor" };

// QML warnings and errors seen (console.warn from QML, binding / type errors through QQmlEngine::warnings). Platform-plugin
// chatter (the offscreen QPA's "does not support ..." notes) is not the gallery's and is not counted.
int g_qmlWarnings = 0;
QStringList g_qmlWarningText;
QtMessageHandler g_prevHandler = nullptr;

bool isQmlMessage(const QMessageLogContext& ctx, const QString& msg) {
    const QByteArray cat = ctx.category ? QByteArray(ctx.category) : QByteArray();
    return cat == "qml" || cat == "js" || cat.startsWith("qt.qml") || msg.contains(QLatin1String("qrc:/"))
        || msg.contains(QLatin1String(".qml:")) || msg.startsWith(QLatin1String("PlayPlay.Buttons"));
}

void messageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    if ((type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) && isQmlMessage(ctx, msg)) {
        ++g_qmlWarnings;
        if (g_qmlWarningText.size() < 40) g_qmlWarningText << msg;
    }
    if (g_prevHandler) g_prevHandler(type, ctx, msg);
}

void out(const QString& s) { std::fprintf(stdout, "%s\n", s.toUtf8().constData()); std::fflush(stdout); }

// The gallery's helper object: run mode for QML, and hover without a mouse.
class GalleryHost : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool shooting READ shooting CONSTANT)
public:
    explicit GalleryHost(bool shooting, QObject* parent = nullptr) : QObject(parent), m_shooting(shooting) {}
    bool shooting() const { return m_shooting; }
    // A HoverEnter sent to the key itself: QQuickControl sets `hovered` from it, the real cursor never moves, and the window's
    // own hover tracking never learns of it, so nothing un-hovers the key until a real mouse passes over it.
    Q_INVOKABLE void hover(QQuickItem* item) {
        if (!item) return;
        const QPointF c(item->width() / 2, item->height() / 2);
        const QPointF scene = item->mapToScene(c);
        QHoverEvent ev(QEvent::HoverEnter, scene, item->mapToGlobal(c), QPointF(-1, -1));
        QCoreApplication::sendEvent(item, &ev);
    }
private:
    bool m_shooting = false;
};

void pump(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// Waits for n frames (asking for each), or timeoutMs.
bool frames(QQuickWindow* w, int n, int timeoutMs = 5000) {
    int seen = 0;
    QEventLoop loop;
    const auto c = QObject::connect(w, &QQuickWindow::frameSwapped, &loop, [&] { if (++seen >= n) loop.quit(); else w->update(); },
                                    Qt::QueuedConnection);
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    w->update();
    loop.exec();
    QObject::disconnect(c);
    return seen >= n;
}

QVariantMap callMap(QObject* root, const char* fn) {
    QVariant v;
    QMetaObject::invokeMethod(root, fn, Q_RETURN_ARG(QVariant, v));
    if (v.canConvert<QJSValue>()) v = v.value<QJSValue>().toVariant();
    return v.toMap();
}

// True when the image has more than one colour (a page that rendered nothing is a flat background).
bool hasContent(const QImage& img) {
    if (img.isNull()) return false;
    const QRgb first = img.pixel(0, 0);
    const int stepX = std::max(1, img.width() / 200), stepY = std::max(1, img.height() / 200);
    for (int y = 0; y < img.height(); y += stepY)
        for (int x = 0; x < img.width(); x += stepX)
            if (img.pixel(x, y) != first) return true;
    return false;
}

QImage shootPage(QQuickWindow* w, QObject* root, const QString& page, QVariantMap* report) {
    root->setProperty("scrollX", 0);
    root->setProperty("scrollY", 0);
    root->setProperty("page", page);
    pump(30);
    const QSizeF cs = root->property("contentSize").toSizeF();
    const int cw = std::max(1, int(std::ceil(cs.width()))), ch = std::max(1, int(std::ceil(cs.height())));
    const int tw = std::min(cw, kTileW), th = std::min(ch, kTileH);
    w->resize(tw, th);
    frames(w, 3);
    pump(250);                                     // 90 ms colour behaviours, popup enter transitions
    QImage img(cw, ch, QImage::Format_ARGB32_Premultiplied);
    img.fill(w->color());
    QPainter p(&img);
    for (int ty = 0; ty < ch; ty += th) {
        for (int tx = 0; tx < cw; tx += tw) {
            root->setProperty("scrollX", tx);
            root->setProperty("scrollY", ty);
            QMetaObject::invokeMethod(root, "pose");
            frames(w, 2);
            pump(60);
            QImage tile = w->grabWindow();
            tile.setDevicePixelRatio(1);
            p.drawImage(QPoint(tx, ty), tile, QRect(0, 0, std::min(tw, cw - tx), std::min(th, ch - ty)));
        }
    }
    p.end();
    if (report) *report = callMap(root, "report");
    return img;
}

struct Stat { double median = 0, p95 = 0; int n = 0; };
Stat stat(std::vector<double> v) {
    Stat s;
    s.n = int(v.size());
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.median = v[v.size() / 2];
    s.p95 = v[std::min(v.size() - 1, size_t(std::ceil(v.size() * 0.95)) - 1)];
    return s;
}

struct BenchRun { double createMs = 0, firstFrameMs = 0; std::vector<double> intervals, render; };

// One grid: create it (timed), wait for its first frame (timed), then n frames redrawn back to back (each frameSwapped asks
// for the next), recording the swap-to-swap interval and the render thread's beforeRendering -> afterRendering time.
BenchRun benchOnce(QQuickWindow* w, QObject* root, const QString& kind, int count, int nFrames) {
    BenchRun r;
    QElapsedTimer clk;
    clk.start();
    QVariant obj;
    QMetaObject::invokeMethod(root, "buildBench", Q_RETURN_ARG(QVariant, obj), Q_ARG(QVariant, kind), Q_ARG(QVariant, count));
    r.createMs = clk.nsecsElapsed() / 1e6;
    frames(w, 1);
    r.firstFrameMs = clk.nsecsElapsed() / 1e6;
    frames(w, 20);                                 // settle

    QMutex mx;
    qint64 lastSwap = -1, renderStart = 0;
    int got = 0;
    QEventLoop loop;
    const auto c1 = QObject::connect(w, &QQuickWindow::beforeRendering, w, [&] { renderStart = clk.nsecsElapsed(); },
                                     Qt::DirectConnection);
    const auto c2 = QObject::connect(w, &QQuickWindow::afterRendering, w, [&] {
        QMutexLocker l(&mx);
        r.render.push_back((clk.nsecsElapsed() - renderStart) / 1e6);
    }, Qt::DirectConnection);
    const auto c3 = QObject::connect(w, &QQuickWindow::frameSwapped, w, [&] {
        const qint64 now = clk.nsecsElapsed();
        bool done = false;
        {
            QMutexLocker l(&mx);
            if (lastSwap >= 0) r.intervals.push_back((now - lastSwap) / 1e6);
            lastSwap = now;
            done = ++got > nFrames;
        }
        if (done) QMetaObject::invokeMethod(&loop, "quit", Qt::QueuedConnection);
        else QMetaObject::invokeMethod(w, "update", Qt::QueuedConnection);
    }, Qt::DirectConnection);
    QTimer::singleShot(60000, &loop, &QEventLoop::quit);
    w->update();
    loop.exec();
    QObject::disconnect(c1);
    QObject::disconnect(c2);
    QObject::disconnect(c3);
    pump(20);                                      // a queued update() may still be in flight
    BenchRun res;
    {   // a render-thread slot that was already running when we disconnected may still be writing r
        QMutexLocker l(&mx);
        res = r;
    }

    QMetaObject::invokeMethod(root, "destroyBench", Q_ARG(QVariant, obj));
    frames(w, 3);
    return res;
}

int runBench(QQuickWindow* w, QObject* root, int count, int nFrames) {
    root->setProperty("page", QString());
    w->resize(25 * 34 + 24, 20 * 34 + 24);
    frames(w, 5);
    // Warm-up: the first creation compiles the module's types; the app compiles them once per run, then builds pages.
    const BenchRun coldIcon = benchOnce(w, root, "icon", count, 10);
    const BenchRun coldGlyph = benchOnce(w, root, "glyph", count, 10);
    benchOnce(w, root, "legacy", count, 10);
    benchOnce(w, root, "shim", count, 10);
    std::vector<double> create[2], first[2], interval[2], render[2], legacy, shim;
    const QString kinds[2] = { "glyph", "icon" };
    for (int rep = 0; rep < 3; ++rep) {
        for (int k = 0; k < 2; ++k) {
            const BenchRun r = benchOnce(w, root, kinds[k], count, nFrames);
            create[k].push_back(r.createMs);
            first[k].push_back(r.firstFrameMs);
            interval[k].insert(interval[k].end(), r.intervals.begin(), r.intervals.end());
            render[k].insert(render[k].end(), r.render.begin(), r.render.end());
        }
        legacy.push_back(benchOnce(w, root, "legacy", count, 10).createMs);   // reference only: creation, not gated
        shim.push_back(benchOnce(w, root, "shim", count, 10).createMs);       // UXB-5's CButton shim of it; not gated
    }
    QJsonObject res;
    res["count"] = count;
    res["framesPerRun"] = nFrames;
    res["graphicsApi"] = int(w->rendererInterface()->graphicsApi());
    res["swapInterval"] = w->format().swapInterval();
    const Stat ci = stat(create[1]), cg = stat(create[0]), fi = stat(first[1]), fg = stat(first[0]);
    const Stat ii = stat(interval[1]), ig = stat(interval[0]), ri = stat(render[1]), rg = stat(render[0]);
    auto j = [](const Stat& s) { return QJsonObject{ { "median", std::round(s.median * 1000) / 1000 }, { "p95", std::round(s.p95 * 1000) / 1000 }, { "n", s.n } }; };
    res["coldCreateIconMs"] = std::round(coldIcon.createMs * 10) / 10;
    res["coldCreateGlyphMs"] = std::round(coldGlyph.createMs * 10) / 10;
    res["createIconMs"] = j(ci);
    res["createGlyphMs"] = j(cg);
    const Stat cl = stat(legacy);
    res["createLegacyCButtonMs"] = j(cl);
    const Stat cs = stat(shim);
    res["createShimCButtonMs"] = j(cs);
    res["createToFirstFrameIconMs"] = j(fi);
    res["createToFirstFrameGlyphMs"] = j(fg);
    res["frameIntervalIconMs"] = j(ii);
    res["frameIntervalGlyphMs"] = j(ig);
    res["renderThreadIconMs"] = j(ri);
    res["renderThreadGlyphMs"] = j(rg);
    const double ratio = ig.median > 0 ? ii.median / ig.median : 0;
    res["frameRatio"] = std::round(ratio * 1000) / 1000;
    const bool createOk = ci.median <= 120.0, frameOk = ratio > 0 && ratio <= 1.10;
    res["createOk"] = createOk;
    res["frameOk"] = frameOk;
    out(QString::fromUtf8(QJsonDocument(res).toJson(QJsonDocument::Indented)));
    out(QStringLiteral("BENCH %1: %2 icon keys created in %3 ms (budget 120; glyph keys %4 ms; legacy CButtons %9 ms, their shim %10 ms; cold %5 ms), frame %6 ms vs glyph %7 ms = x%8 (budget x1.10)")
            .arg(createOk && frameOk ? "PASS" : "FAIL").arg(count).arg(ci.median, 0, 'f', 1).arg(cg.median, 0, 'f', 1)
            .arg(coldIcon.createMs, 0, 'f', 1).arg(ii.median, 0, 'f', 3).arg(ig.median, 0, 'f', 3).arg(ratio, 0, 'f', 3)
            .arg(cl.median, 0, 'f', 1).arg(cs.median, 0, 'f', 1));
    return createOk && frameOk ? 0 : 1;
}

QString pagePath(const QString& path, const QString& page) {
    const QFileInfo fi(path);
    return fi.dir().filePath(fi.completeBaseName() + "-" + page + "." + (fi.suffix().isEmpty() ? QStringLiteral("png") : fi.suffix()));
}

} // namespace

int main(int argc, char* argv[]) {
    QStringList args;
    for (int i = 1; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);
    auto value = [&](const QString& name, const QString& def = QString()) {
        const int i = args.indexOf(name);
        return i >= 0 && i + 1 < args.size() ? args.at(i + 1) : def;
    };
    if (args.contains("--help") || args.contains("-h")) {
        std::fprintf(stdout, "PlayPlayButtonGallery [--screenshot <png> [--page <%s|all>] [--split]] [--zoom <r>] [--bench "
                             "[--bench-count n] [--bench-frames n]] [--check] [--renderer curve|geometry] [--source]\n",
                     kPages.join('|').toUtf8().constData());
        return 0;
    }
    const QString shot = value("--screenshot");
    const bool bench = args.contains("--bench"), check = args.contains("--check");
    const bool headless = !shot.isEmpty() || bench || check;
    const double zoom = std::clamp(value("--zoom", "1").toDouble(), 0.5, 4.0);

    if (headless) qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
    if (bench) {                                   // redraw as fast as the GPU allows, so the interval is the frame's cost
        QSurfaceFormat f = QSurfaceFormat::defaultFormat();
        f.setSwapInterval(0);
        QSurfaceFormat::setDefaultFormat(f);
    }
    g_prevHandler = qInstallMessageHandler(messageHandler);
    QGuiApplication app(argc, argv);
    app.setApplicationName("PlayPlayButtonGallery");
    QQuickStyle::setStyle("Fusion");               // as main.cpp

    GalleryHost host(headless);                    // before the engine, so it outlives the window's bindings
    QQmlApplicationEngine engine;
    // Engine warnings are counted here and printed here: left on, the engine would also print them through qWarning and
    // messageHandler would count each one twice. console.warn from QML still reaches messageHandler (it is no QQmlError).
    engine.setOutputWarningsToStandardError(false);
    QObject::connect(&engine, &QQmlEngine::warnings, &engine, [](const QList<QQmlError>& ws) {
        for (const QQmlError& e : ws) {
            ++g_qmlWarnings;
            if (g_qmlWarningText.size() < 40) g_qmlWarningText << e.toString();
            std::fprintf(stderr, "%s\n", e.toString().toUtf8().constData());
        }
    });
    engine.addImportPath("qrc:/qml");              // PlayPlay.Buttons from the linked ui-buttons.qrc, as the app finds it
    QUrl url("qrc:/tools/buttongallery/gallery.qml");
    if (args.contains("--source")) {               // edit-and-rerun: the module and the gallery from the source tree
        engine.addImportPath(QStringLiteral(PLAYPLAY_SOURCE_DIR "/Play_Play_Native/qml"));
        url = QUrl::fromLocalFile(QStringLiteral(PLAYPLAY_SOURCE_DIR "/tools/buttongallery/gallery.qml"));
    }
    engine.rootContext()->setContextProperty("galleryHost", &host);
    engine.load(url);
    if (engine.rootObjects().isEmpty()) { std::fprintf(stderr, "gallery.qml did not load\n"); return 2; }
    auto* w = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (!w) { std::fprintf(stderr, "gallery.qml's root is not a Window\n"); return 2; }
    QObject* root = w;
    root->setProperty("zoom", zoom);
    const QString renderer = value("--renderer");
    if (!renderer.isEmpty()) {
        QObject* tokens = engine.singletonInstance<QObject*>("PlayPlay.Buttons", "BtnTokens");
        if (tokens) tokens->setProperty("iconRenderer", renderer);
    }
    const QString startPage = value("--page", headless ? QStringLiteral("all") : QStringLiteral("matrix"));
    if (!headless) {
        if (kPages.contains(startPage)) root->setProperty("page", startPage);
        w->show();
        return app.exec();
    }

    // Off screen, never focused, never hit by the mouse.
    w->setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
    w->setPosition(-20000, 100);
    w->resize(kTileW, kTileH);
    w->show();
    if (!frames(w, 2, 10000)) { std::fprintf(stderr, "the window never rendered a frame\n"); return 2; }

    int rc = 0;
    QStringList failures;
    if (!shot.isEmpty() || check) {
        const QStringList pages = startPage == "all" ? kPages : QStringList{ startPage };
        if (startPage != "all" && !kPages.contains(startPage)) { std::fprintf(stderr, "unknown --page %s\n", qPrintable(startPage)); return 2; }
        QList<QImage> images;
        QJsonArray reports;
        for (const QString& page : pages) {
            QVariantMap rep;
            const QImage img = shootPage(w, root, page, &rep);
            images << img;
            reports << QJsonObject::fromVariantMap(rep);
            out(QStringLiteral("page %1: %2 x %3 px, %4 keys%5").arg(page).arg(img.width()).arg(img.height()).arg(rep.value("keys").toInt())
                    .arg(page == "icons" ? QStringLiteral(", %1 of %2 icons drawn").arg(rep.value("iconsKnown").toInt()).arg(rep.value("iconsTotal").toInt())
                                         : QString()));
            if (!rep.value("ready").toBool()) failures << page + ": did not load";
            if (!hasContent(img)) failures << page + ": blank image";
            if (page == "icons") {
                const int total = rep.value("iconsTotal").toInt();
                if (rep.value("iconsKnown").toInt() != total || rep.value("icons").toInt() != total)
                    failures << QStringLiteral("icons: %1 cells, %2 known, %3 in the set").arg(rep.value("icons").toInt()).arg(rep.value("iconsKnown").toInt()).arg(total);
                if (!rep.value("unknownIcons").toList().isEmpty()) failures << "icons: unknown " + rep.value("unknownIcons").toStringList().join(", ");
            }
            if (page == "shim") {
                int over = 0;
                for (const QVariant& v : rep.value("shim").toList()) {
                    const QVariantMap m = v.toMap();
                    // UXB-5: each cell is today's key (a frozen copy) next to its shim; a shim must keep its size within 2 px.
                    if (std::abs(m.value("dw").toDouble()) > 2 || m.value("dh").toDouble() != 0) {
                        ++over;
                        out(QStringLiteral("  shim: %1 dw %2 dh %3").arg(m.value("name").toString()).arg(m.value("dw").toDouble()).arg(m.value("dh").toDouble()));
                    }
                }
                out(QStringLiteral("  shim: %1 of %2 cases over the 2 px budget").arg(over).arg(rep.value("shim").toList().size()));
                if (over > 0) failures << QStringLiteral("shim: %1 cases change size by more than 2 px").arg(over);
            }
        }
        if (!shot.isEmpty()) {
            if (args.contains("--split") || pages.size() == 1) {
                for (int i = 0; i < images.size(); ++i) {
                    const QString path = pages.size() == 1 ? shot : pagePath(shot, pages.at(i));
                    if (!images.at(i).save(path)) failures << "could not write " + path;
                    else out("wrote " + QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()));
                }
            } else {                               // one image, the pages stacked
                int W = 0, H = 0;
                for (const QImage& im : images) { W = std::max(W, im.width()); H += im.height() + 8; }
                QImage all(W, H, QImage::Format_ARGB32_Premultiplied);
                all.fill(QColor("#05050a"));
                QPainter p(&all);
                int y = 0;
                for (const QImage& im : images) { p.drawImage(0, y, im); y += im.height() + 8; }
                p.end();
                if (!all.save(shot)) failures << "could not write " + shot;
                else out("wrote " + QDir::toNativeSeparators(QFileInfo(shot).absoluteFilePath()));
            }
        }
        if (check) {
            if (g_qmlWarnings > 0) failures << QStringLiteral("%1 QML warning(s): ").arg(g_qmlWarnings) + g_qmlWarningText.join(" | ");
            out(failures.isEmpty() ? QStringLiteral("CHECK PASS") : "CHECK FAIL: " + failures.join("; "));
            if (!failures.isEmpty()) rc = 1;
        } else if (!failures.isEmpty()) {
            out("FAIL: " + failures.join("; "));
            rc = 1;
        }
    }
    if (bench) rc = std::max(rc, runBench(w, root, std::max(1, value("--bench-count", "500").toInt()),
                                          std::max(10, value("--bench-frames", "240").toInt())));
    if (g_qmlWarnings > 0 && !check) out(QStringLiteral("note: %1 QML warning(s)").arg(g_qmlWarnings));
    w->hide();
    return rc;
}

#include "main.moc"
