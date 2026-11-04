// CG Designer pure helpers (docs/cg-designer.md): ids, model reply parsing, page normalisation, param derivation, prompt.
#include <QtTest>
#include <QJsonArray>
#include <QJsonObject>
#include "../playout/supervisor/cgdesign.h"

using namespace cgdesign;

static QJsonObject paramNamed(const QJsonArray& a, const QString& n) {
    for (const QJsonValue& v : a) if (v.toObject().value("name").toString() == n) return v.toObject();
    return QJsonObject();
}

class TestCgDesigner : public QObject {
    Q_OBJECT
private slots:
    void slugAndId() {
        QCOMPARE(slugify("Breaking News Bar!"), QString("breaking_news_bar"));
        QCOMPARE(slugify(QString::fromUtf8("  Caf\xC3\xA9 \xE2\x80\x94 \xC3\x9Cn\xC3\xAF" "code  ")), QString("cafe_unicode"));
        QCOMPARE(makeId("Breaking News Bar", {}), QString("cgd_breaking_news_bar"));
        QCOMPARE(makeId("Breaking News Bar", {"cgd_breaking_news_bar"}), QString("cgd_breaking_news_bar_2"));
        QCOMPARE(makeId("!!!", {}), QString("cgd_graphic"));
        QVERIFY(validId("cgd_abc_1"));
        QVERIFY(!validId("lower_third"));
        QVERIFY(!validId("cgd_../x"));
        QVERIFY(validId(makeId(QString(200, 'a'), {})));
        QCOMPARE(humanize("anchorName"), QString("Anchor name"));
        QCOMPARE(humanize("team_a_score"), QString("Team a score"));
    }

    void parseFenced() {
        const QString c = "Here you go:\n```json\n{\"name\":\"Strap\",\"role\":\"lower_third\",\"params\":[{\"name\":\"anchorName\",\"label\":\"Anchor\"}]}\n```\n"
                          "```html\n<!DOCTYPE html>\n<html><body><div data-cg-root>x</div></body></html>\n```\nDone.";
        const Reply r = parseModelReply(c);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(r.meta.value("name").toString(), QString("Strap"));
        QVERIFY(r.html.startsWith("<!DOCTYPE html>"));
        QVERIFY(r.html.endsWith("</html>"));
        QVERIFY(!r.truncated);
    }

    void parseBareAndTruncated() {
        const Reply bare = parseModelReply("{\"name\":\"Bug\"}\n<!DOCTYPE html><html><body><div>x</div></body></html>\ntrailing words");
        QVERIFY2(bare.error.isEmpty(), qPrintable(bare.error));
        QCOMPARE(bare.meta.value("name").toString(), QString("Bug"));
        QVERIFY(bare.html.endsWith("</html>"));

        const Reply cut = parseModelReply("```json\n{\"name\":\"X\"}\n```\n```html\n<!DOCTYPE html><html><body><div style=\"color:");
        QVERIFY(cut.truncated);
        QVERIFY(!cut.error.isEmpty());

        QVERIFY(!parseModelReply("Sorry, I cannot help with that.").error.isEmpty());
        QVERIFY(!parseModelReply("").error.isEmpty());
    }

    void normalizeAddsContract() {
        QStringList w;
        const QString h = normalizeHtml("<style>.a{color:red}</style><div class=\"a\"><span data-field=\"name\">Jane</span></div>", &w);
        QVERIFY(h.startsWith("<!DOCTYPE html>"));
        QVERIFY(h.contains("<body>"));
        QVERIFY(h.contains("<style>.a{color:red}</style>"));
        QVERIFY(h.contains("<div data-cg-root class=\"a\">"));
        QVERIFY(h.contains("data-cg-frame"));
        const int a = h.indexOf("js/cg-animations.js"), f = h.indexOf("js/cg-fields.js");
        QVERIFY(a > 0 && f > a);
        QVERIFY(f < h.indexOf("</body>"));
        // idempotent
        QStringList w2;
        QCOMPARE(normalizeHtml(h, &w2), h);
        QVERIFY(w2.isEmpty());
    }

    void normalizeStripsRemoteAndEditor() {
        QStringList w;
        const QString in = "<!DOCTYPE html><html><head><link rel=\"stylesheet\" href=\"https://fonts.googleapis.com/css2?family=Inter\">"
                           "<script src=\"https://cdn.example.com/gsap.js\"></script><style>@import url('https://x.y/z.css'); body{width:1920px}</style>"
                           "<base href=\"/cg/\" data-cge-base></head><body><div data-cg-root data-cge=\"3\" contenteditable=\"true\">"
                           "<img data-field=\"logo\" src=\"https://example.com/logo.png\"></div>"
                           "<script>CGFields.onChange(function(){});</script></body></html>";
        const QString h = normalizeHtml(in, &w);
        QVERIFY(!h.contains("googleapis"));
        QVERIFY(!h.contains("gsap"));
        QVERIFY(!h.contains("@import"));
        QVERIFY(!h.contains("data-cge"));
        QVERIFY(!h.contains("contenteditable"));
        QVERIFY(!h.contains("<base"));
        QVERIFY(!h.contains("data-cg-frame"));               // the page sets its own 1920px frame
        QVERIFY(h.contains("https://example.com/logo.png"));  // kept, but warned about
        QCOMPARE(w.filter("remote file").size(), 1);
        QVERIFY(w.size() >= 4);
        // runtime scripts go before the page's own inline script
        QVERIFY(h.indexOf("js/cg-fields.js") < h.indexOf("CGFields.onChange"));
    }

    void normalizeRespectsOwnInit() {
        const QString in = "<html><body><div class=\"lt\" id=\"x\">a</div><script src=\"js/cg-animations.js\"></script>"
                           "<script>CGAnimations.init('.lt');</script></body></html>";
        const QString h = normalizeHtml(in);
        QVERIFY(!h.contains("data-cg-root"));               // the page animates itself: no second root
        QVERIFY(h.indexOf("js/cg-fields.js") > h.indexOf("js/cg-animations.js"));
        QVERIFY(h.indexOf("js/cg-fields.js") < h.indexOf("CGAnimations.init"));
    }

    void deriveFromPage() {
        const QString h =
            "<html><head><style>:root{ --accent: #e11d48; --plate:rgba(0,0,0,.6); --gap: 24px; /* --old: red; */ }</style></head><body>"
            "<div data-cg-root><div class=\"n\" data-field=\"anchorName\" data-label=\"Anchor\">Jane &amp; Smith</div>"
            "<div data-field=\"anchorTitle\" data-hide-empty>Chief <b>Editor</b></div>"
            "<img data-field=\"logo\" data-type=\"image\" src=\"\">"
            "<p data-field=\"body\" data-type=\"textarea\">Line one<br>Line two</p>"
            "<span data-field=\"anchorName\">dup</span></div>"
            "<script>var d = CGFields.get('duration', 60); document.body.innerHTML += '<i data-field=\"fake\">';</script></body></html>";
        const QJsonArray meta = {QJsonObject{{"name", "anchorTitle"}, {"label", "Job title"}}, QJsonObject{{"name", "duration"}, {"type", "number"}, {"default", "60"}},
                                 QJsonObject{{"name", "unused"}, {"label", "Nothing reads me"}}};
        const QJsonArray p = deriveParams(h, meta);
        QStringList names; for (const QJsonValue& v : p) names << v.toObject().value("name").toString();
        QCOMPARE(names, QStringList({"anchorName", "anchorTitle", "logo", "body", "accent", "plate", "gap", "duration"}));
        QCOMPARE(paramNamed(p, "anchorName").value("label").toString(), QString("Anchor"));
        QCOMPARE(paramNamed(p, "anchorName").value("default").toString(), QString("Jane & Smith"));
        QCOMPARE(paramNamed(p, "anchorTitle").value("label").toString(), QString("Job title"));
        QCOMPARE(paramNamed(p, "anchorTitle").value("default").toString(), QString("Chief Editor"));
        QCOMPARE(paramNamed(p, "logo").value("type").toString(), QString("image"));
        QCOMPARE(paramNamed(p, "body").value("type").toString(), QString("textarea"));
        QCOMPARE(paramNamed(p, "body").value("default").toString(), QString("Line one\nLine two"));
        QCOMPARE(paramNamed(p, "accent").value("type").toString(), QString("color"));
        QCOMPARE(paramNamed(p, "accent").value("default").toString(), QString("#e11d48"));
        QCOMPARE(paramNamed(p, "plate").value("type").toString(), QString("color"));
        QCOMPARE(paramNamed(p, "gap").value("type").toString(), QString("text"));
        QCOMPARE(paramNamed(p, "duration").value("type").toString(), QString("number"));
        const QJsonArray tf = textFields(p);
        QCOMPARE(tf.size(), 3);
        QCOMPARE(tf.at(0).toString(), QString("anchorName"));
        QCOMPARE(tf.at(1).toString(), QString("anchorTitle"));
        QCOMPARE(tf.at(2).toString(), QString("body"));
    }

    void promptAndMessages() {
        const QString sp = systemPrompt();
        QVERIFY(sp.contains("data-cg-root"));
        QVERIFY(sp.contains("js/cg-fields.js"));
        QVERIFY(sp.contains("lower_third | bug"));
        QVERIFY(!sp.contains("%ROLES%"));
        Request r;
        r.prompt = "A red breaking news strap";
        const QString tricky = "<html>ref 100%4 width</html>";
        r.examples << Example{"template", "lower_third_modern", tricky, "", "the shine %4"};
        r.examples << Example{"image", "", "", "data:image/png;base64,AAAA", "our current look"};
        r.variation = 2; r.variations = 3;
        const QJsonArray parts = userContent(r);
        QCOMPARE(parts.size(), 2);
        const QString t = parts.at(0).toObject().value("text").toString();
        QVERIFY(t.contains("BRIEF:\nA red breaking news strap"));
        QVERIFY(t.contains("REFERENCE 1 (lower_third_modern)"));
        QVERIFY(t.contains(tricky));
        QVERIFY(t.contains("the shine %4"));
        QVERIFY(t.contains("REFERENCE IMAGE 1"));
        QVERIFY(t.contains("variation 2 of 3"));
        QCOMPARE(parts.at(1).toObject().value("image_url").toObject().value("url").toString(), QString("data:image/png;base64,AAAA"));

        Request f; f.mode = "refine"; f.prompt = "make it blue"; f.baseHtml = "<html>cur</html>";
        const QString ft = userContent(f).at(0).toObject().value("text").toString();
        QVERIFY(ft.startsWith("REFINE"));
        QVERIFY(ft.contains("<html>cur</html>"));
        const QJsonObject body = chatBody(f, "m/x", 0.3, 9000);
        QCOMPARE(body.value("messages").toArray().size(), 2);
        QCOMPARE(body.value("max_tokens").toInt(), 9000);
    }

    void houseStyleAndDryRun() {
        QVERIFY(!systemPrompt().contains("Station house style"));
        QVERIFY(!systemPrompt("   ").contains("Station house style"));
        const QString sp = systemPrompt("Navy plates, gold rule, Bahnschrift.");
        QVERIFY(sp.contains("Station house style"));
        QVERIFY(sp.endsWith("Navy plates, gold rule, Bahnschrift."));
        Request r; r.prompt = "p"; r.houseStyle = "Gold rule";
        r.examples << Example{"image", "", "", "data:image/png;base64,AAAAAAAA", ""};
        r.examples << Example{"image", "", "", "data:image/jpeg;base64,BBBB", ""};
        const QJsonObject body = chatBody(r, "m", 0.7, 100);
        QVERIFY(body.value("messages").toArray().at(0).toObject().value("content").toString().contains("Gold rule"));
        const QJsonArray parts = redactImages(body).value("messages").toArray().at(1).toObject().value("content").toArray();
        QCOMPARE(parts.size(), 3);
        QCOMPARE(parts.at(1).toObject().value("image_url").toObject().value("url").toString(), QString("<image 1: 30 bytes>"));
        QCOMPARE(parts.at(2).toObject().value("image_url").toObject().value("url").toString(), QString("<image 2: 27 bytes>"));
        // the original body is untouched
        QVERIFY(body.value("messages").toArray().at(1).toObject().value("content").toArray().at(1).toObject().value("image_url").toObject().value("url").toString().startsWith("data:image/png"));
    }
};

QTEST_GUILESS_MAIN(TestCgDesigner)
#include "test_cgdesigner.moc"
