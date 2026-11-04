#include <QJsonDocument>
// HtmlSource / BrowserHost against a real headless browser. Skipped (QSKIP) when no Chromium-family browser is installed.
// Serves nothing: the page is a data: URL with a transparent body and one opaque red box.
#include <QtTest>
#include <QBuffer>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonObject>
#include <cmath>
#include <cstdlib>
#include "compositor/browserhost.h"
#include "core/browserlocator.h"
#include "compositor/livesources.h"
#include "compositor/compositor.h"

static QString pageUrl() {
    const QByteArray html = "<html><body style=\"margin:0;background:transparent\"><div id=\"box\" style=\"position:absolute;left:100px;top:100px;width:400px;height:200px;background:#ff0000;color:#fff;font:40px sans-serif\">"
                            "<span data-field=\"name\">HELLO</span></div></body></html>";
    return "data:text/html;base64," + QString::fromLatin1(html.toBase64());
}

static QImage toImage(const FramePtr& f) {
    return QImage(f->data[0], f->width, f->height, f->linesize[0], QImage::Format_ARGB32).copy();
}

// PE-08: translucent boxes, a gradient to transparent, rounded (antialiased) corners and 50 % text on a transparent page,
// so the decode has to carry many alpha values.
static QString translucentPageUrl() {
    const QByteArray html = "<html><body style=\"margin:0;background:transparent\">"
        "<div style=\"position:absolute;left:20px;top:20px;width:200px;height:120px;background:rgba(255,0,0,0.5);border-radius:30px\"></div>"
        "<div style=\"position:absolute;left:260px;top:20px;width:340px;height:120px;background:linear-gradient(90deg,rgba(0,128,255,0),rgba(0,128,255,1))\"></div>"
        "<div style=\"position:absolute;left:20px;top:180px;width:580px;height:150px;background:rgba(255,255,255,0.25);color:rgba(255,255,255,0.5);font:bold 80px sans-serif\">HALF</div>"
        "</body></html>";
    return "data:text/html;base64," + QString::fromLatin1(html.toBase64());
}

// PE-19: an "animated" page whose frames are identical: a composited box animates its opacity between values that round to
// alpha 0, so the browser keeps sending screencast frames with the same pixels; one static opaque box beside it.
static QString ghostPageUrl() {
    const QByteArray html = "<html><head><style>@keyframes ghost{from{opacity:.0004}to{opacity:.0012}}</style></head>"
        "<body style=\"margin:0;background:transparent\">"
        "<div style=\"position:absolute;left:300px;top:40px;width:200px;height:200px;background:#00f;will-change:opacity;animation:ghost .4s linear infinite alternate\"></div>"
        "<div style=\"position:absolute;left:20px;top:40px;width:200px;height:120px;background:#0a0\"></div>"
        "</body></html>";
    return "data:text/html;base64," + QString::fromLatin1(html.toBase64());
}

// PE-19: a real-looking CDP screencast message (Chrome's key order) around `b64`.
static QString screencastMessage(const QByteArray& b64, const QString& sid = "8D1F00AA") {
    return QString::fromLatin1("{\"method\":\"Page.screencastFrame\",\"params\":{\"data\":\"") + QString::fromLatin1(b64)
         + QString::fromLatin1("\",\"metadata\":{\"offsetTop\":0,\"pageScaleFactor\":1,\"deviceWidth\":1920,\"deviceHeight\":1080,"
                               "\"scrollOffsetX\":0,\"scrollOffsetY\":0,\"timestamp\":1761480000.123},\"sessionId\":7},\"sessionId\":\"")
         + sid + QStringLiteral("\"}");
}

class TestBrowserHost : public QObject {
    Q_OBJECT
    bool m_haveBrowser = false;
private slots:
    void initTestCase() { m_haveBrowser = !cgbrowser::find().isEmpty(); }
    void init() {   // the PE-19 ingest helper tests need no browser
        if (!m_haveBrowser && !QByteArray(QTest::currentTestFunction()).startsWith("ingest")) QSKIP("no Chromium-family browser installed");
    }
    void cleanupTestCase() { if (m_haveBrowser) BrowserHost::shutdown(); }

    // PE-19: the byte-level split gives the same picture bytes and the same small message as a whole-JSON parse.
    void ingestSplitMatchesJsonParse() {
        QImage img(320, 180, QImage::Format_ARGB32); img.fill(qRgba(10, 200, 30, 128));
        QByteArray png; { QBuffer buf(&png); buf.open(QIODevice::WriteOnly); QVERIFY(img.save(&buf, "PNG")); }
        // a multi-MB payload, like a 1080p frame (base64 alphabet incl. '+' and '/')
        QByteArray big; big.reserve(3 << 20); while (big.size() < (3 << 20)) big += png; big = big.toBase64();
        for (const QByteArray& b64 : {png.toBase64(), big}) {
            const QString msg = screencastMessage(b64);
            QByteArray got; QString rest;
            QVERIFY(htmlingest::splitScreencastData(msg, &got, &rest));
            QCOMPARE(got, b64);
            QVERIFY(rest.size() < 400);
            const QJsonObject full = QJsonDocument::fromJson(msg.toUtf8()).object(), small = QJsonDocument::fromJson(rest.toUtf8()).object();
            QCOMPARE(small.value("method").toString(), QStringLiteral("Page.screencastFrame"));
            QCOMPARE(small.value("sessionId").toString(), QStringLiteral("8D1F00AA"));
            QCOMPARE(small.value("params").toObject().value("data").toString(), QString());
            QCOMPARE(small.value("params").toObject().value("sessionId").toInt(), 7);
            QCOMPARE(small.value("params").toObject().value("metadata"), full.value("params").toObject().value("metadata"));
            QCOMPARE(full.value("params").toObject().value("data").toString().toLatin1(), b64);
            QCOMPARE(QByteArray::fromBase64(got), b64 == png.toBase64() ? png : QByteArray::fromBase64(b64));
        }
        {   // host-thread cost per 1080p-sized message, before (whole JSON) and after (split + small JSON): informational
            const QString msg = screencastMessage(big);
            QElapsedTimer t; t.start();
            for (int i = 0; i < 5; ++i) { const QJsonObject o = QJsonDocument::fromJson(msg.toUtf8()).object(); QVERIFY(!o.value("params").toObject().value("data").toString().toLatin1().isEmpty()); }
            const double oldMs = double(t.nsecsElapsed()) / 5e6;
            t.restart();
            for (int i = 0; i < 5; ++i) { QByteArray g; QString r; QVERIFY(htmlingest::splitScreencastData(msg, &g, &r)); QVERIFY(!QJsonDocument::fromJson(r.toUtf8()).object().isEmpty()); }
            const double newMs = double(t.nsecsElapsed()) / 5e6;
            qInfo().noquote() << QString("PE-19 parse of a %1 MB screencast message on the host thread: whole JSON %2 ms, byte-level %3 ms")
                                     .arg(double(msg.size()) / 1048576.0, 0, 'f', 1).arg(oldMs, 0, 'f', 2).arg(newMs, 0, 'f', 2);
        }
        // shapes it must leave to the whole-JSON parse
        QByteArray b; QString r;
        QVERIFY(!htmlingest::splitScreencastData(u"{\"method\":\"Page.loadEventFired\",\"params\":{\"timestamp\":1}}", &b, &r));
        QVERIFY(!htmlingest::splitScreencastData(u"{\"params\":{\"data\":\"abc\\/def\"}}", &b, &r));      // escaped '/'
        QVERIFY(!htmlingest::splitScreencastData(u"{\"params\":{\"data\":\"unterminated", &b, &r));
        QVERIFY(!htmlingest::splitScreencastData(u"{\"text\":\"say \\\"data\\\":\\\"x\\\"\"}", &b, &r)); // only inside a string
    }

    // PE-19: the digest tells identical payloads from any change (one byte, length); decode threads per streaming page.
    void ingestDigestAndDecodeThreads() {
        QByteArray a(2 << 20, 'A'); for (int i = 0; i < a.size(); i += 97) a[i] = char('a' + i % 26);
        QByteArray same = a; same.detach();
        QByteArray oneByte = a; oneByte[a.size() / 2] = 'Z';
        QByteArray longer = a + 'A';
        QVERIFY(htmlingest::digest(a) == htmlingest::digest(same));
        QVERIFY(htmlingest::digest(a) != htmlingest::digest(oneByte));
        QVERIFY(htmlingest::digest(a) != htmlingest::digest(longer));
        QVERIFY(htmlingest::Digest() != htmlingest::Digest());              // "none" never matches, not even itself
        QVERIFY(htmlingest::digest(QByteArray()) == htmlingest::digest(QByteArray("")));
        // the skip needs the bytes too: a forged collision (same digest, other bytes) is not "the same picture"
        const htmlingest::Digest da = htmlingest::digest(a);
        QVERIFY(htmlingest::sameBytes(da, a, htmlingest::digest(same), same));
        QVERIFY(!htmlingest::sameBytes(da, a, da, oneByte));
        QVERIFY(!htmlingest::sameBytes(htmlingest::Digest(), QByteArray(), htmlingest::Digest(), QByteArray()));
        QElapsedTimer t; t.start();
        for (int i = 0; i < 20; ++i) (void)htmlingest::digest(a);
        qInfo().noquote() << QString("PE-19 digest of a 2 MB payload: %1 ms").arg(double(t.nsecsElapsed()) / 20e6, 0, 'f', 3);
        if (qEnvironmentVariableIntValue("PLAYPLAY_HTML_DECODE_THREADS") <= 0) {
            QCOMPARE(htmlingest::decodeThreadsFor(0), 2);
            QCOMPARE(htmlingest::decodeThreadsFor(1), 2);
            QCOMPARE(htmlingest::decodeThreadsFor(3), 3);
            QCOMPARE(htmlingest::decodeThreadsFor(5), 5);
            QCOMPARE(htmlingest::decodeThreadsFor(40), 8);
        }
    }

    // PE-19 with the real browser: a page whose animation repaints the same pixels. With the skip (default) the identical
    // PNGs are counted as framesSkipped and not decoded; with PLAYPLAY_HTML_SKIP_SAME=0 every one is decoded. Both pages
    // show the same picture.
    void identicalFramesSkipDecode() {
        struct Mode { const char* name; bool skip; std::unique_ptr<LayerSource> src; QJsonObject st; };
        Mode modes[2] = {{"skip", true, {}, {}}, {"noskip", false, {}, {}}};
        for (Mode& m : modes) {
            if (!m.skip) qputenv("PLAYPLAY_HTML_SKIP_SAME", "0"); else qunsetenv("PLAYPLAY_HTML_SKIP_SAME");
            QJsonObject spec; spec["type"] = "html"; spec["url"] = ghostPageUrl(); spec["width"] = 640; spec["height"] = 360;
            spec["firstFrameTimeoutMs"] = 10000; spec["label"] = QString("perftest-%1").arg(m.name);
            QString err;
            m.src = createHtmlSource(spec, &err);
            QVERIFY2(m.src, qPrintable(err));
            const bool opened = m.src->open(OutputFormat());
            qunsetenv("PLAYPLAY_HTML_SKIP_SAME");
            QVERIFY2(opened, qPrintable(m.src->lastError()));
        }
        QCOMPARE(modes[0].src->status().contains("skipSame"), false);   // default: on (key only when off)
        QCOMPARE(modes[1].src->status().value("skipSame").toBool(true), false);
        for (int i = 0; i < 40; ++i) { QTest::qWait(100); for (Mode& m : modes) m.src->video(0, 0); }   // keep both streaming (no idle pause)
        for (Mode& m : modes) {
            m.st = m.src->status();
            qInfo().noquote() << QString("PE-19 html %1: framesIn %2 decoded %3 skipped %4 dropped %5 decodeMs %6 base64Ms %7 decodeThreads %8")
                                     .arg(m.name).arg(m.st.value("framesIn").toDouble()).arg(m.st.value("frames").toDouble())
                                     .arg(m.st.value("framesSkipped").toDouble()).arg(m.st.value("framesDropped").toDouble())
                                     .arg(m.st.value("decodeMs").toDouble()).arg(m.st.value("base64Ms").toDouble())
                                     .arg(m.st.value("browser").toObject().value("decodeThreads").toInt());
        }
        const QJsonObject& on = modes[0].st; const QJsonObject& off = modes[1].st;
        QVERIFY2(on.value("framesIn").toDouble() >= 10, "the animated page should keep sending frames");
        QVERIFY2(on.value("framesSkipped").toDouble() >= on.value("framesIn").toDouble() / 2, "identical frames should be skipped");
        QVERIFY(on.value("frames").toDouble() < on.value("framesIn").toDouble() / 2);
        QCOMPARE(off.value("framesSkipped").toDouble(), 0.0);
        QVERIFY(off.value("frames").toDouble() + off.value("framesDropped").toDouble() >= off.value("framesIn").toDouble() - 2);   // one waiting + one in a decode
        const FramePtr a = modes[0].src->video(0, 0), b = modes[1].src->video(0, 0);
        QVERIFY(a && b);
        QCOMPARE(toImage(a), toImage(b));
        QCOMPARE(qGreen(toImage(a).pixel(100, 100)), 0xaa);
        for (Mode& m : modes) { m.src->close(); m.src.reset(); }
    }

    void transparentFrameAndLiveFieldUpdate() {
        QJsonObject spec; spec["type"] = "html"; spec["url"] = pageUrl(); spec["firstFrameTimeoutMs"] = 10000;
        QString err;
        std::unique_ptr<LayerSource> src = createHtmlSource(spec, &err);
        QVERIFY2(src, qPrintable(err));
        OutputFormat fmt;
        QVERIFY2(src->open(fmt), qPrintable(src->lastError()));
        FramePtr f = src->video(0, 0);
        QVERIFY(f);
        QCOMPARE(f->width, 1920);
        QImage a = toImage(f);
        QCOMPARE(qAlpha(a.pixel(5, 5)), 0);                       // transparent corner
        QCOMPARE(qAlpha(a.pixel(1915, 1075)), 0);
        QCOMPARE(qAlpha(a.pixel(110, 110)), 255);                 // opaque box
        QCOMPARE(qRed(a.pixel(110, 110)), 255);

        QJsonObject up; QJsonObject fields; fields["name"] = "CHANGED TEXT WITH MANY LETTERS"; up["fields"] = fields;
        src->update(up);
        bool changed = false;
        for (int i = 0; i < 50 && !changed; ++i) {
            QTest::qWait(100);
            FramePtr g = src->video(0, 0);
            if (g && g.get() != f.get() && toImage(g) != a) changed = true;
        }
        if (!changed) qInfo().noquote() << "status:" << QJsonDocument(src->status()).toJson(QJsonDocument::Compact);
        QVERIFY(changed);

        QVERIFY(BrowserHost::instance()->pageCount() >= 1);
        src->close();
        src.reset();
        for (int i = 0; i < 50 && BrowserHost::instance()->targetCount() > 0; ++i) QTest::qWait(100);
        QCOMPARE(BrowserHost::instance()->targetCount(), 0);      // the target was released
    }

    // PE-08, four ways to take the same translucent page (the switches are read when the source is created):
    //   straight      (nothing set)                      today: the PNG read as straight alpha
    //   premul        PLAYPLAY_PREMUL=1                  premultiplied once on the decode thread: the same look as today
    //   premul+png    + PLAYPLAY_HTML_PNG_PREMULTIPLIED=1 the PNG bytes taken as premultiplied: no conversion at all
    //   png           PLAYPLAY_HTML_PNG_PREMULTIPLIED=1  the PNG bytes un-premultiplied once into straight alpha
    // Headless Chromium writes premultiplied colours into its screencast PNGs (a 50 % red box decodes as 128,0,0,128),
    // so "straight" and "premul" composite that box at about 25 % red over black, "premul+png" and "png" at the 50 % the
    // page asked for. The bytes of each mode are checked against the straight frame converted by QImage.
    void premultipliedDecode() {
        struct Mode { const char* name; bool premul, png; std::unique_ptr<LayerSource> src; FramePtr f; };
        Mode modes[4] = {{"straight", false, false, {}, {}}, {"premul", true, false, {}, {}}, {"premul+png", true, true, {}, {}}, {"png", false, true, {}, {}}};
        for (Mode& m : modes) {
            if (m.premul) qputenv("PLAYPLAY_PREMUL", "1"); else qunsetenv("PLAYPLAY_PREMUL");
            if (m.png) qputenv("PLAYPLAY_HTML_PNG_PREMULTIPLIED", "1"); else qunsetenv("PLAYPLAY_HTML_PNG_PREMULTIPLIED");
            QJsonObject spec; spec["type"] = "html"; spec["url"] = translucentPageUrl(); spec["width"] = 640; spec["height"] = 360;
            spec["firstFrameTimeoutMs"] = 10000; spec["label"] = QString("perftest-%1").arg(m.name);
            QString err;
            m.src = createHtmlSource(spec, &err);
            qunsetenv("PLAYPLAY_PREMUL"); qunsetenv("PLAYPLAY_HTML_PNG_PREMULTIPLIED");
            QVERIFY2(m.src, qPrintable(err));
            QVERIFY2(m.src->open(OutputFormat()), qPrintable(m.src->lastError()));
            QCOMPARE(m.src->deliversPremultiplied(), m.premul);
            const QJsonObject st = m.src->status();
            QCOMPARE(st.value("premultiplied").toBool(), m.premul);           // keys only when on: default status unchanged
            QCOMPARE(st.value("pngPremultiplied").toBool(), m.png);
            QCOMPARE(st.contains("premultiplied"), m.premul);
        }
        // A static page: let every page settle (layout, fonts), then take the newest pictures.
        for (int i = 0; i < 50; ++i) {
            QTest::qWait(100);
            bool settled = i >= 10;
            for (Mode& m : modes) { const FramePtr f = m.src->video(0, 0); settled = settled && f && f.get() == m.f.get(); m.f = f; }
            if (settled) break;
        }
        for (const Mode& m : modes) { QVERIFY(m.f); QCOMPARE(m.f->width, 640); QCOMPARE(m.f->height, 360); }
        const QImage raw = toImage(modes[0].f);                                   // the PNG as decoded (read as straight)
        const QImage rawAsPremul = QImage(raw.constBits(), raw.width(), raw.height(), raw.bytesPerLine(), QImage::Format_ARGB32_Premultiplied);
        const QImage expect[4] = {
            raw,                                                                     // straight: unchanged
            QImage(raw.convertToFormat(QImage::Format_ARGB32_Premultiplied)),        // premul: premultiplied once more
            rawAsPremul.copy(),                                                      // premul+png: the bytes as they are
            rawAsPremul.convertToFormat(QImage::Format_ARGB32)};                     // png: un-premultiplied once
        auto bytesOf = [](const QImage& img) { return QImage(img.constBits(), img.width(), img.height(), img.bytesPerLine(), QImage::Format_ARGB32).copy(); };
        auto rgba = [](QRgb c) { return QString("%1,%2,%3,%4").arg(qRed(c)).arg(qGreen(c)).arg(qBlue(c)).arg(qAlpha(c)); };
        int translucent = 0;
        for (int y = 0; y < raw.height(); ++y) for (int x = 0; x < raw.width(); ++x) { const int a = qAlpha(raw.pixel(x, y)); if (a > 0 && a < 255) ++translucent; }
        QVERIFY2(translucent > 5000, "the page should have many translucent pixels");
        for (int k = 0; k < 4; ++k) {
            const QImage got = toImage(modes[k].f), want = bytesOf(expect[k]);
            int differing = 0;
            for (int y = 0; y < got.height(); ++y) for (int x = 0; x < got.width(); ++x) if (got.pixel(x, y) != want.pixel(x, y)) ++differing;
            qInfo().noquote() << QString("PE-08 html %1: %2 of %3 pixels differ from the expected bytes; 50 % red box %4, 25 % white panel %5, gradient mid %6; decodeMs %7 (%8 translucent pixels)")
                                     .arg(modes[k].name).arg(differing).arg(got.width() * got.height()).arg(rgba(got.pixel(120, 80)), rgba(got.pixel(500, 300)), rgba(got.pixel(430, 80)))
                                     .arg(modes[k].src->status().value("decodeMs").toDouble()).arg(translucent);
            QCOMPARE(differing, 0);
        }
        // What airs: each source as a layer over black. The 50 % red box at about 64 (premultiplied twice) or 128 (once).
        OutputFormat fmt; fmt.width = 1280; fmt.height = 720;
        const int airs[4] = {64, 64, 128, 128};
        for (int k = 0; k < 4; ++k) {
            Compositor c(fmt);
            c.addLayer(Compositor::LayerSpec::fromJson(QJsonObject{{"id", 50}}), std::move(modes[k].src), 0);
            const MediaFrame mf = c.render(0, 0.0);
            QVERIFY(mf.video);
            const QRgb box = reinterpret_cast<const QRgb*>(mf.video->data[0] + 80 * mf.video->linesize[0])[120];
            qInfo().noquote() << QString("PE-08 html %1 over black: 50 % red box airs as %2").arg(modes[k].name, rgba(box));
            QVERIFY2(std::abs(qRed(box) - airs[k]) <= 2 && qGreen(box) == 0 && qAlpha(box) == 255, qPrintable(QString("%1: %2").arg(modes[k].name, rgba(box))));
        }
        qInfo().noquote() << "PE-08 html browser:" << BrowserHost::instance()->browserPath() << BrowserHost::instance()->browserVersion();
    }

    void httpErrorFailsOpenWithReason() {
        QJsonObject spec; spec["type"] = "html"; spec["url"] = "http://127.0.0.1:1/none.html"; spec["firstFrameTimeoutMs"] = 6000;
        QString err;
        std::unique_ptr<LayerSource> src = createHtmlSource(spec, &err);
        QVERIFY(src);
        QVERIFY(!src->open(OutputFormat()));
        QVERIFY(!src->lastError().isEmpty());
    }
};

QTEST_MAIN(TestBrowserHost)
#include "test_browserhost.moc"
