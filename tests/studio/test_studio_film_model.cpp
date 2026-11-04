// ST-21 film model (docs/plans/starmedia-studio.md §4.6, §4.8): the film tree with validated operations (a port of
// StarMedia src/studio/ops.ts applyOps), the bible, input-hash stale propagation and run locks (a port of
// src/studio/runlocks.ts), and how a film is stored in studio.db.
//
// Ported cases: the "operations" block of starmedia test/studio.ts (update / insert / delete / move / lock / style /
// unknown ids) and every block of test/liveedit-locks.ts but the last two (landInPlace and overlayState keep the
// browser's live object and undo history; there is no such object here). test/liveedit-fuzz.ts becomes a seeded
// shuffle of edits, bible edits, steps starting and finishing, with the invariant that a held node's inputs never
// change under its step.
//
// The fixture is a 2 sequences x 3 scenes x 4 shots film with characters Ann and Bob, two locations, a prop, two voices
// and a style, built through applyOps itself. The headline check: editing Ann's description makes exactly the 14 shots
// that show her stale (directly or through their scene), and nothing else.
//
// Hermetic: databases live in a QTemporaryDir; no network, no child processes.
#include <QtTest>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <random>

#include "core/clock.h"
#include "film/bible.h"
#include "film/locks.h"
#include "film/model.h"
#include "film/stale.h"
#include "store/db.h"

using namespace studio;
using namespace studio::film;

namespace {

const QString kAnn = QStringLiteral("ch_ann"), kBob = QStringLiteral("ch_bob");
const QString kSha1 = QString(64, QLatin1Char('a')), kSha2 = QString(64, QLatin1Char('b'));

QJsonObject O(const char* op, QJsonObject args = {}) {
    args.insert(QStringLiteral("op"), QString::fromLatin1(op));
    return args;
}
QJsonArray A(std::initializer_list<QJsonValue> v) { return QJsonArray(v); }
QString S(const char* s) { return QString::fromUtf8(s); }

QString shotId(int q, int s, int k) { return QStringLiteral("sh%1_%2_%3").arg(q).arg(s).arg(k); }
QString sceneId(int q, int s) { return QStringLiteral("sc%1_%2").arg(q).arg(s); }

// The fixture's own table of who is where (the expectations are computed from this, not from stale.cpp).
QStringList sceneCast(int s) {
    if (s == 1) return {kAnn, kBob};
    if (s == 2) return {kAnn};
    return {kBob};
}
QStringList shotOwnCast(int k) {
    if (k == 1) return {kAnn};
    if (k == 3) return {kBob};
    return {};
}
bool shows(int s, int k, const QString& who) {
    const QStringList own = shotOwnCast(k);
    return (own.isEmpty() ? sceneCast(s) : own).contains(who);
}

Film buildFixture(QStringList* errors) {
    QJsonArray ops;
    ops << O("add_bible", {{"kind", "voice"}, {"id", "vo_alto"}, {"name", "Alto"}, {"model", "kokoro"}, {"voice", "af_heart"}});
    ops << O("add_bible", {{"kind", "voice"}, {"id", "vo_bass"}, {"name", "Bass"}, {"model", "kokoro"}, {"voice", "am_adam"}});
    ops << O("add_character", {{"id", kAnn}, {"name", "Ann"}, {"description", "a woman in a red coat"}, {"voice", "vo_alto"},
                               {"refs", A({kSha1})}});
    ops << O("add_character", {{"id", kBob}, {"name", "Bob"}, {"description", "a tall man"}, {"voice", "Bass"}});
    ops << O("add_location", {{"id", "lo_cafe"}, {"name", "Cafe"}, {"description", "a small corner cafe"}});
    ops << O("add_location", {{"id", "lo_park"}, {"name", "Park"}, {"description", "a park in autumn"}});
    ops << O("add_prop", {{"id", "pr_cup"}, {"name", "Cup"}, {"description", "a blue paper cup"}});
    ops << O("add_bible", {{"kind", "style"}, {"id", "st_1"}, {"name", "House style"}, {"preset", "cinematic"}});
    for (int q = 1; q <= 2; ++q) {
        ops << O("add_sequence", {{"id", QStringLiteral("sq%1").arg(q)}, {"title", QStringLiteral("Sequence %1").arg(q)},
                                  {"video_model", "vid-a"}});
        ops << O("add_cue", {{"id", QStringLiteral("cu%1").arg(q)}, {"parent", QStringLiteral("sq%1").arg(q)},
                             {"cue_kind", "music"}, {"description", "warm strings"}, {"duration", 30}});
        for (int s = 1; s <= 3; ++s) {
            ops << O("add_scene", {{"id", sceneId(q, s)}, {"parent", QStringLiteral("sq%1").arg(q)},
                                   {"title", QStringLiteral("Scene %1.%2").arg(q).arg(s)},
                                   {"location", s == 2 ? "lo_park" : "lo_cafe"},
                                   {"characters", QJsonArray::fromStringList(sceneCast(s))}});
            for (int k = 1; k <= 4; ++k) {
                QJsonObject a{{"id", shotId(q, s, k)}, {"scene_id", sceneId(q, s)},
                              {"title", QStringLiteral("Shot %1.%2.%3").arg(q).arg(s).arg(k)},
                              {"visual", QStringLiteral("shot %1.%2.%3").arg(q).arg(s).arg(k)}};
                if (!shotOwnCast(k).isEmpty()) a.insert(QStringLiteral("characters"), QJsonArray::fromStringList(shotOwnCast(k)));
                if (k == 4) {
                    a.insert(QStringLiteral("location"), QStringLiteral("lo_park"));
                    a.insert(QStringLiteral("props"), A({"Cup"}));
                }
                ops << O("add_shot", a);
            }
        }
        ops << O("add_beat", {{"id", QStringLiteral("bt%1").arg(q)}, {"parent", sceneId(q, 1)}, {"summary", "they meet"}});
        ops << O("add_line", {{"id", QStringLiteral("ln%1_a").arg(q)}, {"parent", sceneId(q, 1)}, {"speaker", "Ann"},
                              {"text", "Hello."}});
        ops << O("add_line", {{"id", QStringLiteral("ln%1_b").arg(q)}, {"parent", sceneId(q, 1)}, {"speaker", "Bob"},
                              {"text", "Hi."}});
        ops << O("add_line", {{"id", QStringLiteral("ln%1_x").arg(q)}, {"parent", shotId(q, 2, 1)}, {"speaker", "Ann"},
                              {"voice", "vo_bass"}, {"text", "Psst."}});
    }
    const OpResult r = applyOps(Film(QStringLiteral("p1")), ops);
    *errors = r.errors;
    Film f = r.film;
    for (const QString& id : f.allIds()) markFresh(f, id, inputHash(f, *f.node(id)));
    return f;
}

QSet<QString> staleSet(const Film& f) {
    QSet<QString> out;
    for (const QString& id : f.allIds())
        if (f.node(id)->stale) out.insert(id);
    return out;
}
QSet<QString> setOf(const QStringList& l) { return QSet<QString>(l.begin(), l.end()); }

Film patched(const Film& f, const QString& id, const char* field, const QJsonValue& v) {
    Film c = f;
    c.nodeMut(id)->data.insert(QString::fromLatin1(field), v);
    return c;
}
Film entryPatched(const Film& f, const QString& id, const char* field, const QJsonValue& v) {
    Film c = f;
    BibleEntry e = *c.entry(id);
    e.data.insert(QString::fromLatin1(field), v);
    c.putEntry(e);
    return c;
}

LockSpec spec(const QString& owner, const QStringList& nodes, const Film& f, const QString& project = QStringLiteral("p1")) {
    LockSpec s;
    s.projectId = project;
    s.owner = owner;
    s.kind = LockKind::Run;
    s.phase = QStringLiteral("assets");
    s.label = QStringLiteral("generating shots");
    s.scope = generationScope(f, nodes);
    return s;
}
LockSpec filmSpec(const QString& owner, const QString& label = QStringLiteral("writing the script")) {
    LockSpec s;
    s.projectId = QStringLiteral("p1");
    s.owner = owner;
    s.phase = QStringLiteral("writing");
    s.label = label;
    s.scope.film = true;
    return s;
}

bool matches(const QString& s, const char* re) { return QRegularExpression(QString::fromUtf8(re)).match(s).hasMatch(); }
bool anyMatches(const QStringList& l, const char* re) {
    for (const QString& s : l)
        if (matches(s, re)) return true;
    return false;
}

} // namespace

class TestStudioFilmModel : public QObject {
    Q_OBJECT

private:
    Film m_fx;
    FakeClock m_clock;

    // A write through a real DbWriter, waited for on the main thread.
    store::WriteResult submitAndWait(store::Db& db, store::Write w) {
        store::WriteResult out;
        bool done = false;
        db.writer().submit(std::move(w), this, [&](const store::WriteResult& r) {
            out = r;
            done = true;
        });
        db.writer().flush();
        QElapsedTimer t;
        t.start();
        while (!done && t.elapsed() < 10000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        return out;
    }
    static qint64 scalar(const QSqlDatabase& db, const QString& sql, const QVariantList& binds = {}) {
        QSqlQuery q(db);
        q.prepare(sql);
        for (int i = 0; i < binds.size(); ++i) q.bindValue(i, binds[i]);
        if (!q.exec() || !q.next()) return -1;
        return q.value(0).toLongLong();
    }
    static QString text(const QSqlDatabase& db, const QString& sql, const QVariantList& binds = {}) {
        QSqlQuery q(db);
        q.prepare(sql);
        for (int i = 0; i < binds.size(); ++i) q.bindValue(i, binds[i]);
        if (!q.exec() || !q.next()) return QStringLiteral("<none>");
        return q.value(0).toString();
    }
    static store::Write raw(const QString& sql, const QVariantList& binds = {}) {
        store::Write w;
        w.tag = QStringLiteral("test.raw");
        w.fn = [sql, binds](store::Tx& tx) { tx.exec(sql, binds); };
        return w;
    }

private slots:
    void initTestCase() {
        QStringList errors;
        m_fx = buildFixture(&errors);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(m_fx.size(), 2 * (1 + 1 + 3 * (1 + 4) + 1 + 3));   // seq, cue, scenes+shots, beat, 3 lines = 42
        QCOMPARE(m_fx.bibleSize(), 8);
        QVERIFY2(m_fx.problems().isEmpty(), qPrintable(m_fx.problems().join(QLatin1Char('\n'))));
        QVERIFY(staleSet(m_fx).isEmpty());
    }

    // ------------------------------------------------------------ schema, names, order keys

    void kindsAndSchema() {
        QCOMPARE(kindName(NodeKind::Shot), QStringLiteral("shot"));
        QCOMPARE(parseKind(u"Shots"), std::optional<NodeKind>(NodeKind::Shot));
        QVERIFY(!parseKind(u"take"));
        QCOMPARE(parseBibleKind(u"characters"), std::optional<BibleKind>(BibleKind::Character));
        // containment matrix
        QVERIFY(canContain(std::nullopt, NodeKind::Sequence));
        QVERIFY(!canContain(std::nullopt, NodeKind::Scene));
        QVERIFY(canContain(NodeKind::Sequence, NodeKind::Scene));
        QVERIFY(!canContain(NodeKind::Sequence, NodeKind::Shot));
        QVERIFY(canContain(NodeKind::Scene, NodeKind::Shot) && canContain(NodeKind::Scene, NodeKind::Beat));
        QVERIFY(canContain(NodeKind::Shot, NodeKind::Line) && canContain(NodeKind::Scene, NodeKind::Line));
        QVERIFY(!canContain(NodeKind::Sequence, NodeKind::Line));
        QVERIFY(canContain(NodeKind::Shot, NodeKind::Cue) && !canContain(std::nullopt, NodeKind::Cue));
        QVERIFY(!canContain(NodeKind::Shot, NodeKind::Shot));
        // the shared schema
        QCOMPARE(normaliseKey(u"afterSceneId"), QStringLiteral("after_scene_id"));
        QCOMPARE(normaliseKey(u"On-Screen Text"), QStringLiteral("on_screen_text"));
        QCOMPARE(cleanText(u"  a\tb\n\nc  ", 200), QStringLiteral("a b c"));
        QCOMPARE(cleanText(u"line 1\r\nline 2\u0001", 4000), QStringLiteral("line 1\nline 2"));
        QVERIFY(findField(nodeFields(NodeKind::Shot), u"description")->name == QLatin1String("visual"));
        QVERIFY(findField(nodeFields(NodeKind::Line), u"dialogue")->name == QLatin1String("text"));
        QCOMPARE(stylePresets().size(), 37);
        QVERIFY(stylePresets().contains(QStringLiteral("noir")) && stylePresets().first() == QLatin1String("cinematic"));
        // names and positions
        // sequence 1 holds [cue, scene 1.1, scene 1.2, scene 1.3]: positions and titles differ on purpose
        QCOMPARE(m_fx.pathOf(shotId(1, 2, 3)), QStringLiteral("1.3.3"));
        QCOMPARE(m_fx.nameOf(shotId(1, 2, 3)), S("shot 1.3.3 “Shot 1.2.3”"));
        QCOMPARE(m_fx.nameOf(QStringLiteral("ln1_x")), QStringLiteral("line 1.3.1.1"));
        QCOMPARE(m_fx.nameOf(QStringLiteral("zz")), QStringLiteral("node zz"));
        QCOMPARE(m_fx.pathOf(QStringLiteral("cu1")), QStringLiteral("1.1"));        // the cue was added first
        QCOMPARE(m_fx.pathOf(sceneId(2, 1)), QStringLiteral("2.2"));
        QCOMPARE(m_fx.entryName(kAnn), S("the character “Ann”"));
        // JSON for the API
        const QJsonObject j = m_fx.toJson(2);
        QCOMPARE(j.value(QStringLiteral("nodes")).toInt(), 42);
        QCOMPARE(j.value(QStringLiteral("bible")).toArray().size(), 8);
        QCOMPARE(j.value(QStringLiteral("sequences")).toArray().size(), 2);
        QVERIFY(j.value(QStringLiteral("sequences")).toArray().at(0).toObject().contains(QStringLiteral("children")));
    }

    void references() {
        const Film& f = m_fx;
        QCOMPARE(f.resolve(u"sh1_2_3").node->id, shotId(1, 2, 3));
        QCOMPARE(f.resolve(u"SH1_2_3").node->id, shotId(1, 2, 3));
        QCOMPARE(f.resolve(u"1.3.2").node->id, shotId(1, 2, 2));       // sequence 1 = [cue, scene 1.1, 1.2, 1.3]
        QCOMPARE(f.resolve(u"shot 1.3.2").node->id, shotId(1, 2, 2));
        QCOMPARE(f.resolve(u"#2").node->id, QStringLiteral("sq2"));
        // a position wins over a title that looks like one ("Shot 2.3.4" is the title of shot 2.4.4)
        QCOMPARE(f.resolve(u"Shot 2.3.4").node->id, shotId(2, 2, 4));
        // "scene 1.1" is a cue by position, so the title is used
        QCOMPARE(f.resolve(u"Scene 1.1").node->id, sceneId(1, 1));
        Film titled = f;
        titled.nodeMut(shotId(2, 3, 4))->data.insert(QStringLiteral("title"), QStringLiteral("Close-up"));
        QCOMPARE(titled.resolve(u"close-up").node->id, shotId(2, 3, 4));
        QVERIFY(!f.resolve(u"scene 2").node);                           // two sequences: no single choice
        QVERIFY(matches(f.resolve(u"zz").error, "^no node \"zz\" \\(nodes: 1=sq1, 1\\.1=cu1"));
        QVERIFY(matches(f.resolve(u"").error, "a node is required"));
        // with one sequence, "scene 2" is scene 2 of it; with one scene, "shot 3" is shot 3 of it
        Film one(QStringLiteral("p9"));
        Node sq; sq.id = QStringLiteral("a"); sq.kind = NodeKind::Sequence;
        Node sc; sc.id = QStringLiteral("b"); sc.kind = NodeKind::Scene; sc.parentId = QStringLiteral("a");
        Node s1; s1.id = QStringLiteral("c1"); s1.kind = NodeKind::Shot; s1.parentId = QStringLiteral("b");
        Node s2 = s1; s2.id = QStringLiteral("c2");
        one.insertNode(sq); one.insertNode(sc); one.insertNode(s1); one.insertNode(s2);
        QCOMPARE(one.resolve(u"scene 1").node->id, QStringLiteral("b"));
        QCOMPARE(one.resolve(u"shot 2").node->id, QStringLiteral("c2"));
        // titles must be unique to be used
        Film twin = f;
        twin.nodeMut(shotId(1, 1, 1))->data.insert(QStringLiteral("title"), QStringLiteral("Twin"));
        twin.nodeMut(shotId(2, 1, 1))->data.insert(QStringLiteral("title"), QStringLiteral("twin"));
        const Film::Found amb = twin.resolve(u"Twin");
        QVERIFY(!amb.node);
        QVERIFY2(matches(amb.error, "several nodes are called “Twin”: shot 1\\.2\\.1 “Twin” \\(sh1_1_1\\), shot 2\\.2\\.1 “twin” \\(sh2_1_1\\)"),
                 qPrintable(amb.error));
        // bible references
        QString err;
        QCOMPARE(f.resolveEntry(QStringLiteral("ann"), {BibleKind::Character}, &err), kAnn);
        QCOMPARE(f.resolveEntry(QStringLiteral("CH_BOB"), {BibleKind::Character}, &err), kBob);
        QVERIFY(f.resolveEntry(QStringLiteral("Cafe"), {BibleKind::Character}, &err).isEmpty());
        QCOMPARE(err, S("“Cafe” is a location, not a character"));
        QVERIFY(f.resolveEntry(QStringLiteral("Zed"), {BibleKind::Character}, &err).isEmpty());
        QCOMPARE(err, S("no character \"Zed\" in the bible (it has: Ann, Bob)"));
    }

    void orderKeys() {
        QCOMPARE(order::key(1048576), QStringLiteral("000001048576"));
        QCOMPARE(order::value(QStringLiteral("000001048576")), qint64(1048576));
        QCOMPARE(order::value(QStringLiteral("12")), qint64(-1));
        Film f(QStringLiteral("p1"));
        Node sq; sq.id = QStringLiteral("sq"); sq.kind = NodeKind::Sequence;
        f.insertNode(sq);
        Node sc; sc.id = QStringLiteral("sc"); sc.kind = NodeKind::Scene; sc.parentId = QStringLiteral("sq");
        f.insertNode(sc);
        QStringList expect;
        auto shot = [&](const QString& id, int at) {
            Node n; n.id = id; n.kind = NodeKind::Shot; n.parentId = QStringLiteral("sc");
            f.insertNode(n, at);
            expect.insert(at < 0 || at > expect.size() ? expect.size() : at, id);
        };
        for (int i = 0; i < 100; ++i) shot(QStringLiteral("a%1").arg(i), -1);                  // appends
        for (int i = 0; i < 40; ++i) shot(QStringLiteral("m%1").arg(i), 1);                    // the same spot: gaps run out
        for (int i = 0; i < 10; ++i) shot(QStringLiteral("s%1").arg(i), 0);                    // the front
        QCOMPARE(f.children(QStringLiteral("sc")), expect);
        // the keys sort the same way (that is what the DB orders by) and all have the fixed width
        QStringList keys;
        for (const QString& id : expect) keys << f.node(id)->orderKey;
        for (int i = 0; i < keys.size(); ++i) {
            QCOMPARE(keys[i].size(), order::kWidth);
            if (i > 0) QVERIFY2(keys[i - 1] < keys[i], qPrintable(keys[i - 1] + QLatin1String(" !< ") + keys[i]));
        }
        // a move keeps the order consistent too
        f.moveNode(QStringLiteral("a99"), QStringLiteral("sc"), 0);
        expect.removeAll(QStringLiteral("a99"));
        expect.prepend(QStringLiteral("a99"));
        QCOMPARE(f.children(QStringLiteral("sc")), expect);
        QVERIFY(f.node(QStringLiteral("a99"))->orderKey < f.node(expect[1])->orderKey);
        QVERIFY(f.problems().isEmpty());
    }

    // ------------------------------------------------------------ operations (starmedia test/studio.ts "operations")

    void opsPortedFromStudioTs() {
        const Film& sb0 = m_fx;
        const QString s2 = shotId(1, 1, 2);
        const OpResult upd = applyOps(sb0, A({O("update_node", {{"node", s2}, {"visual", "new visual"}, {"duration", 6},
                                                                {"note", "x"}})}));
        QVERIFY2(upd.errors.isEmpty(), qPrintable(upd.errors.join('\n')));
        const Node* u = upd.film.node(s2);
        QVERIFY2(u->data.value(QStringLiteral("visual")).toString() == QLatin1String("new visual")
                     && u->data.value(QStringLiteral("title")).toString() == QLatin1String("Shot 1.1.2")
                     && u->data.value(QStringLiteral("duration")).toDouble() == 6,
                 "update patches only the given fields");
        QVERIFY2(u->stale && upd.stale == QStringList{s2} && !upd.film.node(shotId(1, 1, 1))->stale,
                 "changing the visual marks the shot stale and queues regeneration");
        QCOMPARE(upd.applied, QStringList{S("shot 1.2.2 “Shot 1.1.2”: x")});
        QCOMPARE(upd.touched, QStringList{s2});
        QVERIFY2(sb0.node(s2)->data.value(QStringLiteral("visual")).toString() == QLatin1String("shot 1.1.2"),
                 "applyOps does not mutate its input");
        // a title is not an input: nothing becomes stale
        const OpResult title = applyOps(sb0, A({O("update_node", {{"node", s2}, {"title", "Renamed"}})}));
        QVERIFY(title.errors.isEmpty() && title.stale.isEmpty() && staleSet(title.film).isEmpty());
        QCOMPARE(title.applied, QStringList{S("shot 1.2.2 “Shot 1.1.2”: title changed")});
        // insert after an id, at the start ("") and at the end ("END", or no after)
        auto titles = [](const Film& f, const QString& p) {
            QStringList t;
            for (const QString& id : f.children(p)) t << f.node(id)->title();
            return t;
        };
        const QString sc = sceneId(1, 1);
        const OpResult ins = applyOps(sb0, A({O("insert_node", {{"kind", "shot"}, {"parent", sc}, {"after", shotId(1, 1, 1)},
                                                                {"title", "New"}, {"visual", "v"}, {"duration", 3}})}));
        QVERIFY2(ins.errors.isEmpty(), qPrintable(ins.errors.join('\n')));
        // a scene holds its shots, its beat ("New beat": the default title) and two lines (no title)
        QCOMPARE(titles(ins.film, sc), (QStringList{"Shot 1.1.1", "New", "Shot 1.1.2", "Shot 1.1.3", "Shot 1.1.4", "New beat", "", ""}));
        QCOMPARE(ins.stale.size(), 1);                                    // the new shot needs generating
        QVERIFY(ins.film.node(ins.stale[0])->stale && ins.touched == ins.stale);
        QCOMPARE(titles(applyOps(sb0, A({O("insert_node", {{"kind", "shot"}, {"parent", sc}, {"after", ""}, {"title", "F"}})})).film, sc).first(),
                 QStringLiteral("F"));
        QCOMPARE(titles(applyOps(sb0, A({O("add_shot", {{"scene_id", sc}, {"after", "END"}, {"title", "L"}})})).film, sc).last(),
                 QStringLiteral("L"));
        QCOMPARE(titles(applyOps(sb0, A({O("add_shot", {{"scene", sc}, {"title", "L2"}})})).film, sc).last(), QStringLiteral("L2"));
        // delete
        const OpResult del = applyOps(sb0, A({O("delete_node", {{"node", s2}})}));
        QVERIFY(!del.film.node(s2) && del.film.children(sc).size() == sb0.children(sc).size() - 1);
        QCOMPARE(del.applied, QStringList{S("removed shot 1.2.2 “Shot 1.1.2”")});
        const OpResult delScene = applyOps(sb0, A({O("delete_scene", {{"scene", sceneId(1, 3)}})}));
        QCOMPARE(delScene.applied, QStringList{S("removed scene 1.4 “Scene 1.3” and the 4 nodes in it")});
        // locked nodes cannot be deleted
        const OpResult locked = applyOps(applyOps(sb0, A({O("lock", {{"node", s2}})})).film, A({O("delete_node", {{"node", s2}})}));
        QVERIFY(locked.film.node(s2) && locked.errors.size() == 1 && locked.applied.isEmpty());
        QCOMPARE(locked.errors[0], S("Shot 1.2.2 “Shot 1.1.2” is locked; unlock it first"));
        // move reorders; no position = the start (ops.ts)
        auto ids = [](const Film& f, const QString& p) { return f.children(p).mid(0, 4).join(QLatin1Char(',')); };
        QCOMPARE(ids(applyOps(sb0, A({O("move_node", {{"node", shotId(1, 1, 3)}, {"after", ""}})})).film, sc),
                 QStringLiteral("sh1_1_3,sh1_1_1,sh1_1_2,sh1_1_4"));
        QCOMPARE(ids(applyOps(sb0, A({O("move_shot", {{"shot", shotId(1, 1, 3)}})})).film, sc), QStringLiteral("sh1_1_3,sh1_1_1,sh1_1_2,sh1_1_4"));
        QCOMPARE(ids(applyOps(sb0, A({O("move", {{"node", shotId(1, 1, 1)}, {"after", shotId(1, 1, 4)}})})).film, sc),
                 QStringLiteral("sh1_1_2,sh1_1_3,sh1_1_4,sh1_1_1"));
        // a style change stales every unlocked shot (and the sequences and cues that use the style); locked keeps its take
        const QString s1 = shotId(1, 1, 1);
        const OpResult styled = applyOps(applyOps(sb0, A({O("lock", {{"node", s1}})})).film, A({O("set_style", {{"style_id", "noir"}})}));
        QVERIFY2(styled.errors.isEmpty(), qPrintable(styled.errors.join('\n')));
        QCOMPARE(styled.film.entry(QStringLiteral("st_1"))->data.value(QStringLiteral("preset")).toString(), QStringLiteral("noir"));
        QVERIFY(!styled.film.node(s1)->stale);
        QCOMPARE(styled.lockedDrift, QStringList{s1});
        QSet<QString> expect;
        for (int q = 1; q <= 2; ++q) {
            expect << QStringLiteral("sq%1").arg(q) << QStringLiteral("cu%1").arg(q);
            for (int s = 1; s <= 3; ++s)
                for (int k = 1; k <= 4; ++k) expect << shotId(q, s, k);
        }
        expect.remove(s1);
        QCOMPARE(setOf(styled.stale), expect);
        QCOMPARE(staleSet(styled.film), expect);
        QCOMPARE(styled.applied, QStringList{S("style → noir")});
        // unknown ids and styles are reported, not applied
        const OpResult bad = applyOps(sb0, A({O("update_node", {{"node", "zz"}, {"visual", "x"}}), O("set_style", {{"style_id", "nope"}})}));
        QCOMPARE(bad.errors.size(), 2);
        QVERIFY(bad.applied.isEmpty() && staleSet(bad.film).isEmpty());
        // ops.ts: an op without "op" but with a node is an update; a style alone is set_style
        QCOMPARE(applyOps(sb0, A({QJsonObject{{"node", s2}, {"camera", "low"}}})).applied.size(), 1);
        QCOMPARE(applyOps(sb0, A({QJsonObject{{"style_id", "anime"}}})).applied, QStringList{S("style → anime")});
        // unchanged and lock / unlock
        QCOMPARE(applyOps(sb0, A({O("update_node", {{"node", s2}, {"visual", "shot 1.1.2"}})})).unchanged,
                 QStringList{S("Shot 1.2.2 “Shot 1.1.2” (sh1_1_2) already has that")});
        const OpResult lk = applyOps(sb0, A({O("lock_shot", {{"shot", s2}}), O("lock", {{"node", s2}}), O("unlock", {{"node", s2}})}));
        QCOMPARE(lk.applied, (QStringList{S("locked shot 1.2.2 “Shot 1.1.2”"), S("unlocked shot 1.2.2 “Shot 1.1.2”")}));
        QCOMPARE(lk.unchanged, QStringList{S("Shot 1.2.2 “Shot 1.1.2” is already locked")});
        // regenerate marks the node (and an unlocked subtree) stale
        const OpResult rg = applyOps(sb0, A({O("regenerate", {{"node", sceneId(1, 2)}})}));
        QCOMPARE(setOf(rg.stale), (QSet<QString>{sceneId(1, 2), shotId(1, 2, 1), shotId(1, 2, 2), shotId(1, 2, 3), shotId(1, 2, 4),
                                                 QStringLiteral("ln1_x")}));
        // film settings
        const OpResult st = applyOps(sb0, A({O("set_language", {{"language", "es"}}), O("set_aspect", {{"aspect", "9:16"}}),
                                             O("set_length", {{"target_seconds", 90}})}));
        QVERIFY2(st.errors.isEmpty(), qPrintable(st.errors.join('\n')));
        QCOMPARE(st.film.settings().language, QStringLiteral("es"));
        QCOMPARE(st.film.settings().aspect, QStringLiteral("9:16"));
        QCOMPARE(st.film.settings().targetSeconds, 90.0);
        QCOMPARE(st.applied.last(), S("length → ~90 s"));
    }

    void invalidOpsRejectedWithReasons() {
        const Film& f = m_fx;
        struct Case {
            QJsonValue op;
            const char* reason;
        };
        const QString sh = shotId(1, 1, 1);
        Film locked = applyOps(f, A({O("lock", {{"node", sceneId(1, 2)}}), O("lock", {{"node", shotId(1, 3, 3)}})})).film;
        const QList<Case> cases{
            {O("frobnicate"), "^unknown op \"frobnicate\" \\(use one of: update_node, insert_node"},
            {QJsonValue(5), "^operation 1 is not an object"},
            {O("update_node", {{"visual", "x"}}), "^update_node needs the node to change"},
            {O("update_node", {{"node", "zz"}, {"visual", "x"}}), "^no node \"zz\""},
            {O("update_node", {{"node", sh}}), "^nothing to change in shot 1\\.2\\.1 “Shot 1\\.1\\.1” \\(sh1_1_1\\): give at least one of title, visual, camera"},
            {O("update_node", {{"node", sh}, {"visual", ""}}), "^nothing to change"},
            {O("update_node", {{"node", sh}, {"colour", "red"}}), "^unknown field \"colour\" for a shot \\(fields: title, visual"},
            {O("update_node", {{"node", sh}, {"colour", "red"}, {"mood", "x"}}), "^unknown fields \"colour\", \"mood\" for a shot"},
            {O("update_node", {{"node", "sq1"}, {"anchor_asset", kSha1}}), "^\"anchor_asset\" is written by generation, not by an edit"},
            {O("update_node", {{"node", sh}, {"duration", 25}}), "^duration of a shot must be a number from 1\\.5 to 20$"},
            {O("update_node", {{"node", sh}, {"duration", "soon"}}), "^duration of a shot must be a number"},
            {O("update_node", {{"node", sh}, {"transition", "spin"}}), "^transition \"spin\" is not one of: cut, fade, dissolve, dip, slide, wipe, zoom$"},
            {O("update_node", {{"node", sh}, {"characters", A({"Zed"})}}), "^no character \"Zed\" in the bible \\(it has: Ann, Bob\\)$"},
            {O("update_node", {{"node", sh}, {"location", "Ann"}}), "^“Ann” is a character, not a location$"},
            {O("update_node", {{"node", sh}, {"title", QString(201, QLatin1Char('t'))}}), "^title of a shot is too long \\(201 characters; at most 200\\)$"},
            {O("update_shot", {{"node", sceneId(1, 1)}, {"title", "x"}}), "^Scene 1\\.2 “Scene 1\\.1” is a scene, not a shot$"},
            {O("insert_node", {{"parent", sceneId(1, 1)}}), "^insert_node needs a kind \\(sequence, scene, shot, beat, line or cue\\)$"},
            {O("add_shot", {{"title", "x"}}), "^A shot goes in a scene: give parent"},
            {O("add_shot", {{"parent", "sq1"}}), "^A shot goes in a scene, not in sequence 1 “Sequence 1”$"},
            {O("add_sequence", {{"parent", sceneId(1, 1)}}), "^A sequence is top level, not in scene 1\\.2 “Scene 1\\.1”$"},
            {O("add_line", {{"parent", "sq1"}, {"text", "x"}}), "^A line goes in a scene or a shot, not in sequence 1"},
            {O("add_shot", {{"parent", sceneId(1, 1)}, {"id", sh}}), "^there is already a node with the id \"sh1_1_1\"$"},
            {O("add_shot", {{"parent", sceneId(1, 1)}, {"id", kAnn}}), "^there is already a bible entry with the id \"ch_ann\"$"},
            {O("add_shot", {{"parent", sceneId(1, 1)}, {"id", "../x"}}), "^\"\\.\\./x\" is not a usable id"},
            {O("add_shot", {{"parent", sceneId(1, 1)}, {"after", shotId(1, 2, 1)}}), "^Shot 1\\.3\\.1 “Shot 1\\.2\\.1” is not in scene 1\\.2 “Scene 1\\.1”: after must name one of its children$"},
            {O("move_node", {{"node", "sq1"}, {"parent", sceneId(1, 1)}}), "^cannot move sequence 1 “Sequence 1” into itself$"},
            {O("move_node", {{"node", sh}, {"parent", "sq2"}}), "^A shot goes in a scene, not in sequence 2 “Sequence 2”$"},
            {O("move_node", {{"node", sh}, {"after", sh}}), "^Shot 1\\.2\\.1 “Shot 1\\.1\\.1” cannot go after itself$"},
            {O("move_node", {{"node", sh}, {"after", "x"}, {"speed", 2}}), "^move_node takes node, parent and after \\(not \"speed\"\\)$"},
            {O("add_bible", {{"name", "X"}}), "^add_bible needs a kind \\(character, location, prop, style or voice\\)$"},
            {O("add_character", {{"description", "x"}}), "^a character needs a name$"},
            {O("add_character", {{"name", "ann"}}), "^there is already a character called “Ann”$"},
            {O("add_character", {{"name", "Cy"}, {"voice", "Ann"}}), "^“Ann” is a character, not a voice$"},
            {O("add_character", {{"name", "Cy"}, {"refs", A({"abc"})}}), "^refs: \"abc\" is not an artifact sha256"},
            {O("add_bible", {{"kind", "style"}, {"name", "Two"}, {"preset", "anime"}}), "^the film already has a style \\(“House style”\\); change it with set_style$"},
            {O("update_bible", {{"entry", "Nobody"}, {"description", "x"}}), "^no character or location or prop or style or voice \"Nobody\""},
            {O("update_character", {{"character", "Ann"}}), "^nothing to change in the character “Ann”: give at least one of name, refs, description"},
            {O("update_character", {{"character", "Ann"}, {"name", "Bob"}}), "^there is already a character called “Bob”$"},
            {O("update_voice", {{"voice", "Alto"}, {"speed", 3}}), "^speed of a voice must be a number from 0\\.5 to 2$"},
            {O("remove_character", {{"character", "Ann"}}), "^The character “Ann” is used by scene 1\\.2 “Scene 1\\.1”, shot 1\\.2\\.1 “Shot 1\\.1\\.1”, line 1\\.2\\.6 and \\d+ more; remove it from them first$"},
            {O("remove_bible", {{"entry", "Alto"}}), "^The voice “Alto” is used by character “Ann”; remove it from it first$"},
            {O("set_style", {{"style_id", "nope"}}), "^unknown style \"nope\" \\(styles: cinematic, realistic"},
            {O("set_style", {}), "^set_style needs a style \\(one of: cinematic"},
            {O("set_style", {{"style_id", "custom"}}), "^a custom style needs a description \\(custom\\)$"},
            {O("set_length", {{"target_seconds", 3}}), "^target length must be at least 5 s$"},
            {O("set_length", {{"target_seconds", 99999}}), "^target length must be at most 14400 s"},
            {O("set_aspect", {{"aspect", "21:9"}}), "^aspect must be one of: 9:16, 16:9, 1:1, 4:5$"},
            {O("set_language", {{"language", "english please"}}), "^\"english please\" is not a language code"},
            {O("set_language", {}), "^set_language needs a language code"},
        };
        for (const Case& c : cases) {
            const OpResult r = applyOps(f, QJsonArray{c.op});
            const QByteArray what = QJsonDocument(QJsonArray{c.op}).toJson(QJsonDocument::Compact);
            QVERIFY2(r.applied.isEmpty(), what.constData());
            QVERIFY2(r.errors.size() == 1, (what + " -> " + r.errors.join(" | ").toUtf8()).constData());
            QVERIFY2(matches(r.errors[0], c.reason), (what + " -> " + r.errors[0].toUtf8()).constData());
            QCOMPARE(r.film.size(), f.size());
            QVERIFY(staleSet(r.film).isEmpty());
        }
        // locked nodes freeze their subtree
        const QList<Case> lockCases{
            {O("update_node", {{"node", shotId(1, 2, 1)}, {"visual", "x"}}), "^Shot 1\\.3\\.1 “Shot 1\\.2\\.1” is inside locked scene 1\\.3 “Scene 1\\.2”; unlock that first$"},
            {O("add_shot", {{"parent", sceneId(1, 2)}}), "^Scene 1\\.3 “Scene 1\\.2” is locked; unlock it first$"},
            {O("delete_node", {{"node", sceneId(1, 3)}}), "^Scene 1\\.4 “Scene 1\\.3” holds locked shot 1\\.4\\.3 “Shot 1\\.3\\.3”; unlock it first$"},
            {O("delete_node", {{"node", "sq1"}}), "^Sequence 1 “Sequence 1” holds locked scene 1\\.3 “Scene 1\\.2”; unlock it first$"},
            {O("regenerate", {{"node", shotId(1, 3, 3)}}), "^Shot 1\\.4\\.3 “Shot 1\\.3\\.3” is locked; unlock it first$"},
            {O("move_node", {{"node", shotId(1, 2, 2)}, {"after", ""}}), "^Scene 1\\.3 “Scene 1\\.2” is locked; unlock it first$"},
            {O("move_node", {{"node", shotId(1, 3, 3)}, {"parent", sceneId(1, 1)}}), "^Shot 1\\.4\\.3 “Shot 1\\.3\\.3” is locked; unlock it first$"},
        };
        for (const Case& c : lockCases) {
            const OpResult r = applyOps(locked, QJsonArray{c.op});
            const QByteArray what = QJsonDocument(QJsonArray{c.op}).toJson(QJsonDocument::Compact);
            QVERIFY2(r.applied.isEmpty() && r.errors.size() == 1, (what + " -> " + r.errors.join(" | ").toUtf8()).constData());
            QVERIFY2(matches(r.errors[0], c.reason), (what + " -> " + r.errors[0].toUtf8()).constData());
        }
        // a locked node may still move among its siblings (ops.ts), and be unlocked
        QCOMPARE(applyOps(locked, A({O("move_node", {{"node", shotId(1, 3, 3)}, {"after", ""}})})).applied.size(), 1);
        // too many ops, and atomic
        QJsonArray many;
        for (int i = 0; i < 201; ++i) many << O("lock", {{"node", sh}});
        const OpResult tooMany = applyOps(f, many);
        QCOMPARE(tooMany.errors, QStringList{QStringLiteral("too many operations (201); send at most 200 per call")});
        QVERIFY(tooMany.rolledBack && tooMany.applied.isEmpty());
        ApplyOptions atomic;
        atomic.atomic = true;
        const OpResult at = applyOps(f, A({O("update_node", {{"node", sh}, {"visual", "x"}}), O("frobnicate")}), atomic);
        QVERIFY(at.rolledBack && at.applied.isEmpty() && at.errors.size() == 1);
        QCOMPARE(at.film.node(sh)->data.value(QStringLiteral("visual")).toString(), QStringLiteral("shot 1.1.1"));
        // the limits are enforced
        ApplyOptions tight;
        tight.limits.maxChildren = 7;
        QVERIFY(matches(applyOps(f, A({O("add_shot", {{"parent", sceneId(1, 1)}})}), tight).errors.value(0),
                        "^Scene 1\\.2 “Scene 1\\.1” already holds 7 nodes, the most one may hold$"));
        tight.limits = Limits();
        tight.limits.maxNodes = 42;
        QVERIFY(matches(applyOps(f, A({O("add_sequence", {})}), tight).errors.value(0), "^the film has 42 nodes, the most it may have$"));
    }

    void aliasesAndNormalisation() {
        const Op a = normalizeOp(QJsonObject{{"action", "Add-Shot"}, {"sceneId", "1.2"}, {"changes", QJsonObject{{"Description", "x"}}}});
        QCOMPARE(a.name, QStringLiteral("insert_node"));
        QCOMPARE(a.kind, std::optional<NodeKind>(NodeKind::Shot));
        QCOMPARE(a.args.value(QStringLiteral("scene_id")).toString(), QStringLiteral("1.2"));
        QCOMPARE(a.args.value(QStringLiteral("description")).toString(), QStringLiteral("x"));
        QCOMPARE(normalizeOp(QJsonObject{{"op", "edit_scene"}}).name, QStringLiteral("update_node"));
        QCOMPARE(normalizeOp(QJsonObject{{"op", "rewrite"}}).name, QStringLiteral("update_node"));
        QCOMPARE(normalizeOp(QJsonObject{{"op", "add_character"}}).bibleKind, std::optional<BibleKind>(BibleKind::Character));
        QCOMPARE(normalizeOp(QJsonObject{{"op", "add_character"}}).name, QStringLiteral("add_bible"));
        QCOMPARE(normalizeOp(QJsonObject{{"op", "remove_voice"}}).name, QStringLiteral("remove_bible"));
        QCOMPARE(normalizeOp(QJsonObject{{"op", "change_style"}}).name, QStringLiteral("set_style"));
        QCOMPARE(normalizeOp(QJsonObject{{"op", "update_style"}}).name, QStringLiteral("set_style"));
        QCOMPARE(normalizeOp(QJsonObject{{"op", "regenerate_shot"}}).name, QStringLiteral("regenerate"));
        QCOMPARE(normalizeOp(QJsonObject{{"op", "unlock_scene"}}).name, QStringLiteral("unlock"));
        QCOMPARE(normalizeOp(QJsonObject{{"op", "move_character"}}).name, QString());
        QCOMPARE(normalizeOp(QJsonObject{{"type", "move"}, {"node", "x"}}).name, QStringLiteral("move_node"));
        const Op ins = normalizeOp(QJsonObject{{"op", "insert"}, {"type", "beat"}});
        QVERIFY(ins.name == QLatin1String("insert_node") && ins.kind == NodeKind::Beat);
        // aliases resolve against the node's kind: "description" is a shot's visual, a scene's summary
        const OpResult r = applyOps(m_fx, A({O("edit_shot", {{"shot", "shot 1.2.2"}, {"description", "wide"}, {"seconds", "5"}}),
                                             O("edit", {{"node", "Scene 1.1"}, {"description", "they meet"}, {"cast", "Bob"}}),
                                             O("update_line", {{"line_id", "ln1_a"}, {"dialogue", "Hello there."}, {"character", "Bob"}})}));
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join('\n')));
        const Node* shot = r.film.node(shotId(1, 1, 2));
        QCOMPARE(shot->data.value(QStringLiteral("visual")).toString(), QStringLiteral("wide"));
        QCOMPARE(shot->data.value(QStringLiteral("duration")).toDouble(), 5.0);
        const Node* scene = r.film.node(sceneId(1, 1));
        QCOMPARE(scene->data.value(QStringLiteral("summary")).toString(), QStringLiteral("they meet"));
        QCOMPARE(scene->data.value(QStringLiteral("characters")).toArray(), A({kBob}));
        QCOMPARE(r.film.node(QStringLiteral("ln1_a"))->data.value(QStringLiteral("speaker")).toString(), kBob);
        // "-" clears a field, a comma list is a list, refs keep their ids
        const OpResult c = applyOps(m_fx, A({O("update_node", {{"node", shotId(1, 1, 4)}, {"props", "-"}, {"characters", "Ann, Bob"}})}));
        QVERIFY2(c.errors.isEmpty(), qPrintable(c.errors.join('\n')));
        QVERIFY(!c.film.node(shotId(1, 1, 4))->data.contains(QStringLiteral("props")));
        QCOMPARE(c.film.node(shotId(1, 1, 4))->data.value(QStringLiteral("characters")).toArray(), A({kAnn, kBob}));
    }

    void bibleOps() {
        const OpResult add = applyOps(m_fx, A({O("add_character", {{"name", "  Cy  "}, {"description", "a child"}, {"wardrobe", "yellow raincoat"},
                                                                  {"refs", A({kSha2.toUpper(), kSha2, kSha1})}}),
                                               O("add_voice", {{"name", "Narrator"}, {"model", "kokoro"}, {"speed", 1.234}})}));
        QVERIFY2(add.errors.isEmpty(), qPrintable(add.errors.join('\n')));
        QCOMPARE(add.bibleTouched.size(), 2);
        const BibleEntry* cy = add.film.entry(add.bibleTouched[0]);
        QVERIFY(cy && cy->name == QLatin1String("Cy") && cy->id.startsWith(QLatin1String("ch_")));
        QCOMPARE(cy->refs, (QStringList{kSha2, kSha1}));                    // lower-cased, duplicates dropped
        QCOMPARE(add.film.entry(add.bibleTouched[1])->data.value(QStringLiteral("speed")).toDouble(), 1.23);
        QVERIFY(add.stale.isEmpty());                                        // nobody uses them yet
        // 16 reference images at most
        QJsonArray seventeen;
        for (int i = 0; i < 17; ++i) seventeen << QStringLiteral("%1").arg(i, 64, 16, QLatin1Char('0'));
        QCOMPARE(applyOps(m_fx, A({O("add_prop", {{"name", "Bag"}, {"refs", seventeen}})})).errors,
                 QStringList{QStringLiteral("refs has 17 reference images; at most 16")});
        // rename by name, update refs; an update naming the entry by "name" when nothing else names it
        const OpResult ren = applyOps(m_fx, A({O("update_character", {{"character", "Ann"}, {"name", "Anna"}, {"refs", A({kSha2})}})}));
        QVERIFY2(ren.errors.isEmpty(), qPrintable(ren.errors.join('\n')));
        QCOMPARE(ren.film.entry(kAnn)->name, QStringLiteral("Anna"));
        QCOMPARE(ren.applied, QStringList{S("the character “Ann”: name, refs changed")});
        const OpResult byName = applyOps(m_fx, A({O("update_location", {{"name", "Park"}, {"description", "a park in spring"}})}));
        QVERIFY2(byName.errors.isEmpty() && byName.applied.size() == 1, qPrintable(byName.errors.join('\n')));
        // the kind's own key is a field once the entry is named otherwise
        const OpResult vf = applyOps(m_fx, A({O("update_voice", {{"id", "vo_alto"}, {"voice", "af_bella"}})}));
        QVERIFY2(vf.errors.isEmpty(), qPrintable(vf.errors.join('\n')));
        QCOMPARE(vf.film.entry(QStringLiteral("vo_alto"))->data.value(QStringLiteral("voice")).toString(), QStringLiteral("af_bella"));
        // a character's voice field is a field of the character, not the entry to change
        const OpResult cv = applyOps(m_fx, A({O("update_character", {{"character", "Bob"}, {"voice", "Alto"}})}));
        QVERIFY2(cv.errors.isEmpty(), qPrintable(cv.errors.join('\n')));
        QCOMPARE(cv.film.entry(kBob)->data.value(QStringLiteral("voice")).toString(), QStringLiteral("vo_alto"));
        // remove an unused entry; a custom style
        const OpResult rm = applyOps(add.film, A({O("remove_character", {{"character", "Cy"}})}));
        QVERIFY(rm.errors.isEmpty() && !rm.film.entry(cy->id));
        const OpResult cs = applyOps(m_fx, A({O("set_style", {{"style", "custom"}, {"note", "ink wash, two colours"}})}));
        QVERIFY2(cs.errors.isEmpty(), qPrintable(cs.errors.join('\n')));
        QCOMPARE(cs.film.entry(QStringLiteral("st_1"))->data.value(QStringLiteral("custom")).toString(), QStringLiteral("ink wash, two colours"));
        QCOMPARE(applyOps(m_fx, A({O("set_style", {{"style", "Cinematic"}})})).unchanged, QStringList{QStringLiteral("the style is already cinematic")});
        // a film without a style gets one
        const OpResult first = applyOps(Film(QStringLiteral("px")), A({O("set_style", {{"style", "anime"}})}));
        QVERIFY(first.errors.isEmpty() && first.film.entries(BibleKind::Style).size() == 1);
        // entry hashes: notes are not inputs, the description and the refs are
        BibleEntry e = *m_fx.entry(kAnn);
        const QString h0 = entryHash(e);
        e.data.insert(QStringLiteral("notes"), QStringLiteral("likes tea"));
        e.data.insert(QStringLiteral("voice"), QStringLiteral("vo_bass"));
        QCOMPARE(entryHash(e), h0);
        e.refs << kSha2;
        QVERIFY(entryHash(e) != h0);
        // usersOf / affectedBy
        const Users u = usersOf(m_fx, QStringLiteral("vo_bass"));
        QCOMPARE(u.entries, QStringList{kBob});
        QCOMPARE(setOf(u.nodes), (QSet<QString>{QStringLiteral("ln1_x"), QStringLiteral("ln2_x")}));
        QCOMPARE(setOf(affectedBy(m_fx, QStringLiteral("vo_bass"))),
                 (QSet<QString>{QStringLiteral("ln1_x"), QStringLiteral("ln2_x"), QStringLiteral("ln1_b"), QStringLiteral("ln2_b")}));
    }

    // ------------------------------------------------------------ stale propagation

    void staleEditCharacterOnlyItsShots() {
        const OpResult r = applyOps(m_fx, A({O("update_character", {{"character", "Ann"}, {"description", "a woman in a green coat"}})}));
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join('\n')));
        QSet<QString> expect;
        for (int q = 1; q <= 2; ++q)
            for (int s = 1; s <= 3; ++s)
                for (int k = 1; k <= 4; ++k)
                    if (shows(s, k, kAnn)) expect << shotId(q, s, k);
        QCOMPARE(expect.size(), 14);
        QCOMPARE(setOf(r.stale), expect);
        QCOMPARE(staleSet(r.film), expect);                                 // no scene, line, cue or Bob-only shot
        QVERIFY(r.touched.isEmpty() && r.bibleTouched == QStringList{kAnn});
        // Bob: the complement among shots that show him
        const OpResult b = applyOps(m_fx, A({O("update_character", {{"character", "Bob"}, {"wardrobe", "a grey suit"}})}));
        QSet<QString> bob;
        for (int q = 1; q <= 2; ++q)
            for (int s = 1; s <= 3; ++s)
                for (int k = 1; k <= 4; ++k)
                    if (shows(s, k, kBob)) bob << shotId(q, s, k);
        QCOMPARE(setOf(b.stale), bob);
        // a reference image counts too; notes do not
        QCOMPARE(setOf(applyOps(m_fx, A({O("update_character", {{"character", "Ann"}, {"refs", A({kSha2})}})})).stale), expect);
        QVERIFY(applyOps(m_fx, A({O("update_character", {{"character", "Ann"}, {"notes", "likes tea"}})})).stale.isEmpty());
        // the character's voice: only her lines that use it (not the one with its own voice), no shot
        const OpResult v = applyOps(m_fx, A({O("update_character", {{"character", "Ann"}, {"voice", "Bass"}})}));
        QCOMPARE(setOf(v.stale), (QSet<QString>{QStringLiteral("ln1_a"), QStringLiteral("ln2_a")}));
        // the voice entry: every line spoken with it
        const OpResult vb = applyOps(m_fx, A({O("update_voice", {{"voice", "Bass"}, {"speed", 1.1}})}));
        QCOMPARE(setOf(vb.stale), (QSet<QString>{QStringLiteral("ln1_b"), QStringLiteral("ln2_b"), QStringLiteral("ln1_x"),
                                                 QStringLiteral("ln2_x")}));
        // a location: the scenes are not stale (they read where, not how it looks); shots showing it are
        const OpResult park = applyOps(m_fx, A({O("update_location", {{"location", "Park"}, {"description", "a park in spring"}})}));
        QSet<QString> parkShots;
        for (int q = 1; q <= 2; ++q)
            for (int s = 1; s <= 3; ++s)
                for (int k = 1; k <= 4; ++k)
                    if (k == 4 || s == 2) parkShots << shotId(q, s, k);
        QCOMPARE(setOf(park.stale), parkShots);
        // a prop
        QCOMPARE(setOf(applyOps(m_fx, A({O("update_prop", {{"prop", "Cup"}, {"description", "a red cup"}})})).stale).size(), 6);
        // the scene's location (by id): the scene and the shots inheriting it, not the shot with its own
        const OpResult loc = applyOps(m_fx, A({O("update_scene", {{"scene", sceneId(1, 1)}, {"location", "Park"}})}));
        QCOMPARE(setOf(loc.stale), (QSet<QString>{sceneId(1, 1), shotId(1, 1, 1), shotId(1, 1, 2), shotId(1, 1, 3)}));
        // the scene's cast: the scene and the shots without a cast of their own
        const OpResult cast = applyOps(m_fx, A({O("update_scene", {{"scene", sceneId(1, 3)}, {"characters", A({"Ann"})}})}));
        QCOMPARE(setOf(cast.stale), (QSet<QString>{sceneId(1, 3), shotId(1, 3, 2), shotId(1, 3, 4)}));
        // a sequence's video model: it and all its shots, none of the other sequence
        const OpResult vm = applyOps(m_fx, A({O("update_sequence", {{"sequence", "sq2"}, {"video_model", "vid-b"}})}));
        QSet<QString> sq2{QStringLiteral("sq2")};
        for (int s = 1; s <= 3; ++s)
            for (int k = 1; k <= 4; ++k) sq2 << shotId(2, s, k);
        QCOMPARE(setOf(vm.stale), sq2);
        // the aspect: sequences and shots; the language: lines
        QCOMPARE(applyOps(m_fx, A({O("set_aspect", {{"aspect", "1:1"}})})).stale.size(), 2 + 24);
        QCOMPARE(applyOps(m_fx, A({O("set_language", {{"language", "de"}})})).stale.size(), 6);
        QVERIFY(applyOps(m_fx, A({O("set_length", {{"target_seconds", 600}})})).stale.isEmpty());
        // titles, notes, transitions, on-screen text and positions are not inputs
        const OpResult cosmetic = applyOps(m_fx, A({O("update_node", {{"node", shotId(1, 1, 1)}, {"title", "T"}, {"transition", "dip"}, {"on_screen", "Hi"}}),
                                                    O("update_node", {{"node", "sq1"}, {"title", "Act one"}}),
                                                    O("move_node", {{"node", shotId(1, 1, 4)}, {"after", ""}})}));
        QVERIFY2(cosmetic.errors.isEmpty() && cosmetic.stale.isEmpty(), qPrintable(cosmetic.errors.join('\n')));
        // a line's text: that line only; a shot's camera: that shot only
        QCOMPARE(applyOps(m_fx, A({O("update_line", {{"line", "ln1_b"}, {"text", "Hey."}})})).stale, QStringList{QStringLiteral("ln1_b")});
        QCOMPARE(applyOps(m_fx, A({O("update_shot", {{"shot", shotId(2, 3, 3)}, {"camera", "crane up"}})})).stale, QStringList{shotId(2, 3, 3)});
        // inputsOf says what a hash covers
        const QJsonObject in = inputsOf(m_fx, *m_fx.node(shotId(1, 2, 2)));
        QCOMPARE(in.value(QStringLiteral("characters")).toArray().size(), 1);         // scene 1.2: Ann only
        QCOMPARE(in.value(QStringLiteral("characters")).toArray().at(0).toArray().at(0).toString(), kAnn);
        QCOMPARE(in.value(QStringLiteral("location")).toArray().at(0).toString(), QStringLiteral("lo_park"));
        QCOMPARE(in.value(QStringLiteral("video_model")).toString(), QStringLiteral("vid-a"));
        QVERIFY(!in.value(QStringLiteral("f")).toObject().contains(QStringLiteral("title")));
        QCOMPARE(entriesUsed(m_fx, *m_fx.node(QStringLiteral("ln1_x"))), QStringList{QStringLiteral("vo_bass")});
        QCOMPARE(keysUsed(m_fx, *m_fx.node(QStringLiteral("ln1_a"))),
                 (QStringList{QStringLiteral("bible:vo_alto"), QStringLiteral("bible:ch_ann"), QStringLiteral("language")}));
    }

    // applyOps rehashes only what an edit can reach; after any mix of ops a full pass must find nothing more
    void incrementalMatchesFullPass() {
        int applied = 0, refused = 0;
        for (quint32 seed = 11; seed <= 13; ++seed) {
            std::mt19937 rng(seed);
            auto pick = [&](int n) { return n <= 0 ? 0 : int(rng() % quint32(n)); };
            Film film = m_fx;
            auto ofKind = [&](NodeKind k) {
                QStringList out;
                for (const QString& id : film.allIds())
                    if (film.node(id)->kind == k) out << id;
                return out;
            };
            auto any = [&](const QStringList& l) { return l.isEmpty() ? QString() : l[pick(l.size())]; };
            const QStringList chars{"Ann", "Bob"}, places{"Cafe", "Park"}, voices{"Alto", "Bass"};
            for (int i = 0; i < 400; ++i) {
                const QString v = QStringLiteral("v%1").arg(i);
                const QString shot = any(ofKind(NodeKind::Shot)), scene = any(ofKind(NodeKind::Scene));
                const QString seq = any(ofKind(NodeKind::Sequence)), line = any(ofKind(NodeKind::Line));
                QJsonObject op;
                switch (pick(16)) {
                case 0: op = O("update_node", {{"node", shot}, {"visual", v}}); break;
                case 1: op = O("update_node", {{"node", shot}, {pick(2) ? "characters" : "location", pick(3) == 0 ? QJsonValue("-") :
                                                pick(2) ? QJsonValue(chars[pick(2)]) : QJsonValue(places[pick(2)])}}); break;
                case 2: op = O("update_node", {{"node", shot}, {pick(2) ? "title" : "props", pick(2) ? QJsonValue(v) : QJsonValue("Cup")}}); break;
                case 3: op = O("update_node", {{"node", scene}, {pick(2) ? "location" : "characters", pick(2) ? places[pick(2)] : chars[pick(2)]}}); break;
                case 4: op = O("update_node", {{"node", seq}, {pick(2) ? "video_model" : "title", v}}); break;
                case 5: op = O("update_node", {{"node", line}, {pick(2) ? "speaker" : "voice", pick(2) ? chars[pick(2)] : voices[pick(2)]}}); break;
                case 6: op = O("update_character", {{"character", chars[pick(2)]}, {pick(3) == 0 ? "notes" : "description", v}}); break;
                case 7: op = O("update_character", {{"character", chars[pick(2)]}, {"voice", voices[pick(2)]}}); break;
                case 8: op = O("update_bible", {{"entry", pick(2) ? voices[pick(2)] : places[pick(2)]}, {pick(2) ? "notes" : "model", v}}); break;
                case 9: op = pick(3) == 0 ? O("set_aspect", {{"aspect", aspects()[pick(4)]}})
                           : pick(2) ? O("set_language", {{"language", pick(2) ? "en" : "fr"}}) : O("set_style", {{"style", stylePresets()[pick(5)]}}); break;
                case 10: op = O("move_node", {{"node", shot}, {"parent", scene}, {"after", "end"}}); break;
                case 11: op = O("add_shot", {{"parent", scene}, {"visual", v}}); break;
                case 12: op = O(pick(2) ? "delete_node" : "add_line", pick(2) ? QJsonObject{{"node", shot}} : QJsonObject{{"parent", shot}, {"speaker", chars[pick(2)]}, {"text", v}}); break;
                case 13: case 14: op = O(pick(2) ? "lock" : "unlock", {{"node", pick(3) == 0 ? scene : shot}}); break;
                default: op = O("regenerate", {{"node", pick(2) ? shot : line}}); break;
                }
                const OpResult r = applyOps(film, A({op}));
                (r.applied.isEmpty() ? refused : applied) += 1;
                film = r.film;
                Film full = film;
                const StaleReport missed = propagate(full);
                QVERIFY2(missed.changed.isEmpty(), qPrintable(QStringLiteral("seed %1 op %2 %3: a full pass still finds %4")
                                                                   .arg(seed).arg(i).arg(QString::fromUtf8(QJsonDocument(op).toJson(QJsonDocument::Compact)),
                                                                                         missed.changed.join(','))));
                QVERIFY2(film.problems().isEmpty(), qPrintable(film.problems().join('\n')));
            }
        }
        qInfo("incremental vs full: %d ops applied, %d refused", applied, refused);
        QVERIFY(applied > 600 && refused > 20);
    }

    void lockedDriftAndMarkFresh() {
        const QString s = shotId(1, 1, 1);
        const OpResult l = applyOps(m_fx, A({O("lock", {{"node", s}}), O("lock", {{"node", sceneId(2, 2)}})}));
        QVERIFY(l.stale.isEmpty());
        const OpResult e = applyOps(l.film, A({O("update_character", {{"character", "Ann"}, {"description", "older"}})}));
        // the locked shot and the shots of the locked scene keep their take; they are reported as drift
        QVERIFY(!e.film.node(s)->stale && !e.film.node(shotId(2, 2, 1))->stale);
        QCOMPARE(setOf(e.lockedDrift), (QSet<QString>{s, shotId(2, 2, 1), shotId(2, 2, 2), shotId(2, 2, 4)}));
        QCOMPARE(e.stale.size(), 14 - 4);
        QCOMPARE(e.film.node(s)->inputHash, m_fx.node(s)->inputHash);    // never touched
        // unlocking shows them stale
        const OpResult u = applyOps(e.film, A({O("unlock", {{"node", s}})}));
        QCOMPARE(u.stale, QStringList{s});
        QVERIFY(u.film.node(s)->stale);
        // markFresh: only with the hash the inputs have now, never on a locked node
        Film f = u.film;
        const QString oldHash = m_fx.node(s)->inputHash;
        QVERIFY(!markFresh(f, s, oldHash));                                  // made from the old description: stays stale
        QVERIFY(f.node(s)->stale);
        QVERIFY(markFresh(f, s, inputHash(f, *f.node(s))));
        QVERIFY(!f.node(s)->stale);
        QVERIFY(!markFresh(f, shotId(2, 2, 1), inputHash(f, *f.node(shotId(2, 2, 1)))));   // under a locked scene
        QVERIFY(!markFresh(f, QStringLiteral("zz"), oldHash));
        // outputs are not inputs
        QString err;
        QVERIFY2(f.setOutputs(QStringLiteral("sq1"), {{"anchor_asset", kSha1.toUpper()}}, &err), qPrintable(err));
        QCOMPARE(f.node(QStringLiteral("sq1"))->data.value(QStringLiteral("anchor_asset")).toString(), kSha1);
        QVERIFY(f.setOutputs(QStringLiteral("ln1_a"), {{"voice_asset", kSha2}, {"voice_duration", 1.25}}, &err));
        QVERIFY(propagate(f).changed.isEmpty());
        QVERIFY(!f.setOutputs(QStringLiteral("sq1"), {{"title", "x"}}, &err));
        QCOMPARE(err, QStringLiteral("\"title\" is not an output of a sequence"));
        QVERIFY(!f.setOutputs(QStringLiteral("sq1"), {{"anchor_asset", "nope"}}, &err));
        QVERIFY(f.setChosenTake(s, QStringLiteral("tk_1")) && !f.setChosenTake(s, QStringLiteral("../x")));
        QVERIFY(f.setOutputs(QStringLiteral("sq1"), {{"anchor_asset", ""}}, &err));
        QVERIFY(!f.node(QStringLiteral("sq1"))->data.contains(QStringLiteral("anchor_asset")));
    }

    // ------------------------------------------------------------ persistence

    void persistence() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FakeClock clock;
        store::DbOptions o;
        o.path = dir.filePath(QStringLiteral("studio.db"));
        o.clock = &clock;
        store::Db db(o);
        QString err;
        QVERIFY2(db.open(&err), qPrintable(err));
        // a project row with a brief that has other keys
        QVERIFY(submitAndWait(db, raw(QStringLiteral("INSERT INTO projects (id, kind, title, brief_json) VALUES ('p1', 'film', 'Coffee', ?)"),
                                      {QStringLiteral("{\"logline\":\"two strangers share a table\"}")})).ok);
        // the whole fixture
        const store::WriteResult w1 = submitAndWait(db, saveWrite(Film(QStringLiteral("p1")), m_fx));
        QVERIFY2(w1.ok, qPrintable(w1.error));
        QCOMPARE(scalar(db.reader(), QStringLiteral("SELECT COUNT(*) FROM film_nodes WHERE project_id = 'p1'")), qint64(42));
        QCOMPARE(scalar(db.reader(), QStringLiteral("SELECT COUNT(*) FROM bible WHERE project_id = 'p1'")), qint64(8));
        std::optional<Film> loaded = Film::load(db.reader(), QStringLiteral("p1"), &err);
        QVERIFY2(loaded, qPrintable(err));
        auto same = [&](const Film& a, const Film& b) {
            QCOMPARE(a.allIds(), b.allIds());
            for (const QString& id : a.allIds()) QVERIFY2(sameRow(*a.node(id), *b.node(id)), qPrintable(id));
            QCOMPARE(a.bibleSize(), b.bibleSize());
            for (const BibleEntry* e : a.entries()) QVERIFY2(b.entry(e->id) && sameEntry(*e, *b.entry(e->id)), qPrintable(e->id));
            QCOMPARE(a.settings().aspect, b.settings().aspect);
            QCOMPARE(a.settings().language, b.settings().language);
            QCOMPARE(a.settings().targetSeconds, b.settings().targetSeconds);
        };
        same(*loaded, m_fx);
        if (QTest::currentTestFailed()) return;
        QVERIFY(!loaded->node(shotId(1, 1, 1))->stale && !loaded->node(shotId(1, 1, 1))->inputHash.isEmpty());
        const QString brief0 = text(db.reader(), QStringLiteral("SELECT brief_json FROM projects WHERE id = 'p1'"));
        QVERIFY2(brief0.contains(QLatin1String("logline")) && brief0.contains(QLatin1String("\"aspect\":\"16:9\"")), qPrintable(brief0));
        // only what changed is written: one shot edit = one row (plus nothing else)
        clock.advance(60000);
        const qint64 t1 = clock.nowMs();
        const OpResult e = applyOps(*loaded, A({O("update_node", {{"node", shotId(1, 2, 3)}, {"camera", "push in"}}),
                                                O("set_aspect", {{"aspect", "9:16"}})}));
        QVERIFY(e.errors.isEmpty());
        const store::WriteResult w2 = submitAndWait(db, saveWrite(*loaded, e.film));
        QVERIFY2(w2.ok, qPrintable(w2.error));
        // the shot, plus the 2 sequences and 24 shots the aspect made stale
        QCOMPARE(scalar(db.reader(), QStringLiteral("SELECT COUNT(*) FROM film_nodes WHERE updated_ms = ?"), {t1}), qint64(26));
        const QString brief1 = text(db.reader(), QStringLiteral("SELECT brief_json FROM projects WHERE id = 'p1'"));
        QVERIFY2(brief1.contains(QLatin1String("logline")) && brief1.contains(QLatin1String("\"aspect\":\"9:16\"")), qPrintable(brief1));
        loaded = Film::load(db.reader(), QStringLiteral("p1"), &err);
        QVERIFY2(loaded, qPrintable(err));
        same(*loaded, e.film);
        if (QTest::currentTestFailed()) return;
        // a title edit writes one row
        clock.advance(60000);
        const OpResult t = applyOps(*loaded, A({O("update_node", {{"node", sceneId(2, 3)}, {"title", "Night"}})}));
        QVERIFY(submitAndWait(db, saveWrite(*loaded, t.film)).ok);
        QCOMPARE(scalar(db.reader(), QStringLiteral("SELECT COUNT(*) FROM film_nodes WHERE updated_ms = ?"), {clock.nowMs()}), qint64(1));
        // removing a scene deletes its subtree and the takes of the removed nodes, not other takes
        for (const QString& id : {shotId(1, 3, 1), shotId(1, 3, 2), shotId(1, 1, 1)})
            QVERIFY(submitAndWait(db, raw(QStringLiteral("INSERT INTO takes (id, film_node_id, kind) VALUES (?, ?, 'keyframe')"),
                                          {QStringLiteral("tk_") + id, id})).ok);
        loaded = Film::load(db.reader(), QStringLiteral("p1"), &err);
        const OpResult d = applyOps(*loaded, A({O("delete_node", {{"node", sceneId(1, 3)}})}));
        QVERIFY(submitAndWait(db, saveWrite(*loaded, d.film)).ok);
        QCOMPARE(scalar(db.reader(), QStringLiteral("SELECT COUNT(*) FROM film_nodes WHERE project_id = 'p1'")), qint64(42 - 5));
        QCOMPARE(scalar(db.reader(), QStringLiteral("SELECT COUNT(*) FROM takes")), qint64(1));
        loaded = Film::load(db.reader(), QStringLiteral("p1"), &err);
        QVERIFY2(loaded, qPrintable(err));
        same(*loaded, d.film);
        if (QTest::currentTestFailed()) return;
        // sibling order survives a reload after a renumber
        Film many = *loaded;
        for (int i = 0; i < 30; ++i) {
            const OpResult r = applyOps(many, A({O("add_shot", {{"parent", sceneId(2, 1)}, {"after", shotId(2, 1, 1)}, {"id", QStringLiteral("n%1").arg(i)}})}));
            QVERIFY(r.errors.isEmpty());
            many = r.film;
        }
        QVERIFY(submitAndWait(db, saveWrite(*loaded, many)).ok);
        loaded = Film::load(db.reader(), QStringLiteral("p1"), &err);
        QCOMPARE(loaded->children(sceneId(2, 1)), many.children(sceneId(2, 1)));
        // an id of another project is never overwritten: the whole write fails, nothing of it lands
        Film other(QStringLiteral("p2"));
        const OpResult p2 = applyOps(other, A({O("add_sequence", {{"id", "sq_p2"}}), O("add_sequence", {{"id", "sq1"}})}));
        QVERIFY(p2.errors.isEmpty());
        const store::WriteResult clash = submitAndWait(db, saveWrite(other, p2.film));
        QVERIFY(!clash.ok);
        QVERIFY2(clash.error.contains(QLatin1String("film node id \"sq1\" belongs to another project")), qPrintable(clash.error));
        QCOMPARE(scalar(db.reader(), QStringLiteral("SELECT COUNT(*) FROM film_nodes WHERE project_id = 'p2'")), qint64(0));
        QCOMPARE(text(db.reader(), QStringLiteral("SELECT project_id FROM film_nodes WHERE id = 'sq1'")), QStringLiteral("p1"));
        const OpResult p2b = applyOps(other, A({O("add_character", {{"id", kAnn}, {"name", "Other Ann"}})}));
        const store::WriteResult clashB = submitAndWait(db, saveWrite(other, p2b.film));
        QVERIFY(!clashB.ok && clashB.error.contains(QLatin1String("bible entry id \"ch_ann\" belongs to another project")));
        // a project row is created when missing (p3), and a malformed brief fails the write instead of being replaced
        Film p3(QStringLiteral("p3"));
        QVERIFY(submitAndWait(db, saveWrite(p3, applyOps(p3, A({O("add_sequence", {{"id", "p3_sq"}})})).film)).ok);
        QCOMPARE(text(db.reader(), QStringLiteral("SELECT kind FROM projects WHERE id = 'p3'")), QStringLiteral("film"));
        QVERIFY(submitAndWait(db, raw(QStringLiteral("UPDATE projects SET brief_json = '{broken' WHERE id = 'p3'"))).ok);
        std::optional<Film> f3 = Film::load(db.reader(), QStringLiteral("p3"), &err);
        QVERIFY2(f3, qPrintable(err));
        const store::WriteResult bad = submitAndWait(db, saveWrite(*f3, applyOps(*f3, A({O("set_language", {{"language", "fr"}})})).film));
        QVERIFY(!bad.ok);
        QCOMPARE(text(db.reader(), QStringLiteral("SELECT brief_json FROM projects WHERE id = 'p3'")), QStringLiteral("{broken"));
        // a damaged tree is refused at load, with the reason
        QVERIFY(submitAndWait(db, raw(QStringLiteral("INSERT INTO film_nodes (id, project_id, parent_id, kind) VALUES ('orphan', 'p1', 'gone', 'shot')"))).ok);
        QVERIFY(!Film::load(db.reader(), QStringLiteral("p1"), &err));
        QVERIFY2(err.contains(QLatin1String("is damaged: shot orphan has no parent (gone is missing)")), qPrintable(err));
        QVERIFY(submitAndWait(db, raw(QStringLiteral("UPDATE film_nodes SET parent_id = 'sq2' WHERE id = 'orphan'"))).ok);
        QVERIFY(!Film::load(db.reader(), QStringLiteral("p1"), &err));
        QVERIFY2(err.contains(QLatin1String("shot orphan is in sequence sq2: a shot goes in a scene")), qPrintable(err));
        QVERIFY(submitAndWait(db, raw(QStringLiteral("UPDATE film_nodes SET kind = 'take' WHERE id = 'orphan'"))).ok);
        QVERIFY(!Film::load(db.reader(), QStringLiteral("p1"), &err));
        QVERIFY2(err.contains(QLatin1String("unknown kind \"take\"")), qPrintable(err));
        db.close();
    }

    // ------------------------------------------------------------ run locks (starmedia test/liveedit-locks.ts)

    void locksComparingFilms() {
        const Film& a = m_fx;
        QCOMPARE(canon(QJsonObject{{"b", 1}, {"a", A({QJsonObject{{"y", 3}, {"x", 2}}})}}),
                 canon(QJsonObject{{"a", A({QJsonObject{{"x", 2}, {"y", 3}}})}, {"b", 1}}));
        const QString s3 = shotId(1, 1, 3);
        const FilmDiff d = diffFilm(a, patched(a, s3, "visual", QStringLiteral("new")));
        QVERIFY2(d.nodes == QSet<QString>{s3} && d.fields.isEmpty() && d.reorderedParents.isEmpty(),
                 "one changed node is the only one reported");
        Film e = a;
        e.removeSubtree(shotId(1, 3, 4));
        e.settingsMut().aspect = QStringLiteral("9:16");
        const FilmDiff d2 = diffFilm(a, e);
        QVERIFY2(d2.nodes == QSet<QString>{shotId(1, 3, 4)} && d2.fields == QSet<QString>{QStringLiteral("aspect")},
                 "a removed node and a changed setting are reported");
        Film g = a;
        Node n;
        n.id = QStringLiteral("new1");
        n.kind = NodeKind::Shot;
        n.parentId = sceneId(1, 1);
        g.insertNode(n, 1);
        QVERIFY2(diffFilm(a, g).nodes == QSet<QString>{QStringLiteral("new1")} && diffFilm(a, g).reorderedParents.isEmpty(),
                 "an added node is reported by its id");
        Film h = a;
        h.moveNode(shotId(1, 1, 4), sceneId(1, 1), 0);
        const FilmDiff d3 = diffFilm(a, h);
        QVERIFY2(d3.reorderedParents == QSet<QString>{sceneId(1, 1)} && d3.nodes.isEmpty(), "moving nodes is a reorder, not a content change");
        Film bib = entryPatched(a, kAnn, "description", QStringLiteral("x"));
        QCOMPARE(diffFilm(a, bib).fields, QSet<QString>{QStringLiteral("bible:ch_ann")});
        Film sty = entryPatched(a, QStringLiteral("st_1"), "preset", QStringLiteral("noir"));
        QCOMPARE(diffFilm(a, sty).fields, (QSet<QString>{QStringLiteral("bible:st_1"), QStringLiteral("style")}));
        Film anc = a;
        QVERIFY(anc.setOutputs(QStringLiteral("sq1"), {{"anchor_asset", kSha1}}));
        QCOMPARE(diffFilm(a, anc).fields, QSet<QString>{QStringLiteral("anchor:sq1")});
        // nodeInputs drops what a generation writes, keeps what it reads
        Film out = a;
        out.setOutputs(QStringLiteral("ln1_a"), {{"voice_asset", kSha1}});
        out.setChosenTake(QStringLiteral("ln1_a"), QStringLiteral("tk1"));
        out.nodeMut(QStringLiteral("ln1_a"))->stale = true;
        QVERIFY(!nodeInputs(*out.node(QStringLiteral("ln1_a"))).value(QStringLiteral("data")).toObject().contains(QStringLiteral("voice_asset")));
        QVERIFY(sameInputs(*a.node(QStringLiteral("ln1_a")), *out.node(QStringLiteral("ln1_a"))));
        QVERIFY(!sameInputs(*a.node(s3), *patched(a, s3, "visual", QStringLiteral("other")).node(s3)));
        QCOMPARE(namesForSentence(a, {s3}), S("Shot 1.2.3 “Shot 1.1.3”"));
        QCOMPARE(namesForSentence(a, {shotId(1, 1, 1), s3}), S("Shots 1.2.1 “Shot 1.1.1”, 1.2.3 “Shot 1.1.3”"));
        QCOMPARE(namesForSentence(a, {sceneId(1, 1), s3}), S("Scene 1.2 “Scene 1.1”, shot 1.2.3 “Shot 1.1.3”"));
        QCOMPARE(namesForSentence(a, {shotId(1, 1, 1), shotId(1, 1, 2), s3, shotId(1, 1, 4), shotId(1, 2, 1)}),
                 S("Shots 1.2.1 “Shot 1.1.1”, 1.2.2 “Shot 1.1.2”, 1.2.3 “Shot 1.1.3” and 2 more"));
        // the scope of a shot step
        const LockScope sc = generationScope(a, {shotId(1, 1, 1), shotId(1, 2, 4)});
        QCOMPARE(sc.writes, QStringList{QStringLiteral("anchor:sq1")});
        QCOMPARE(sc.reads, (QStringList{QStringLiteral("aspect"), QStringLiteral("bible:ch_ann"), QStringLiteral("bible:lo_cafe"),
                                        QStringLiteral("bible:lo_park"), QStringLiteral("bible:pr_cup"), QStringLiteral("bible:st_1"),
                                        QStringLiteral("style")}));
    }

    void locksMatrix() {
        const Film& f = m_fx;
        RunLocks L(&m_clock);
        int changes = 0;
        L.onChange([&] { ++changes; });
        const Acquired a = L.acquire(spec(QStringLiteral("main"), {shotId(1, 1, 1), shotId(1, 1, 2)}, f), &f);
        QVERIFY2(a.ok, "a shot step on two shots is granted");
        const Acquired b = L.acquire(spec(QStringLiteral("main"), {shotId(1, 1, 3), shotId(1, 1, 4)}, f), &f);
        QVERIFY2(b.ok && L.runCount(QStringLiteral("p1")) == 2, "another on other shots runs at the same time (they share only the anchor)");
        const Acquired c = L.acquire(spec(QStringLiteral("user"), {shotId(1, 1, 2), shotId(1, 2, 1)}, f), &f);
        QVERIFY2(!c.ok && c.conflict.kind == Conflict::Kind::Node && c.conflict.nodeIds == QStringList{shotId(1, 1, 2)}
                     && matches(c.conflict.message, "^Shot 1\\.2\\.2 “Shot 1\\.1\\.2” is busy \\(generating shots\\)"),
                 qPrintable(c.conflict.message));
        const Acquired d = L.acquire(filmSpec(QStringLiteral("user")), &f);
        QVERIFY2(!d.ok && d.conflict.kind == Conflict::Kind::Film
                     && d.conflict.message == QLatin1String("Writing the script rewrites the whole film, so it can't start while "
                                                            "generating shots is running. Wait for it to finish, or press Stop."),
                 qPrintable(d.conflict.message));
        LockSpec reader = spec(QStringLiteral("user"), {}, f);
        reader.label = QStringLiteral("rendering");
        reader.scope = LockScope();
        const Acquired e = L.acquire(reader, &f);
        QVERIFY2(e.ok, "a step that only reads (a render of a frozen copy) is never in the way");
        LockSpec otherProject = filmSpec(QStringLiteral("user"));
        otherProject.projectId = QStringLiteral("p2");
        const Acquired g = L.acquire(otherProject, &f);
        QVERIFY2(g.ok, "another film is another world");
        // the tree: a step on a scene holds its shots, and a shot step blocks a step on its scene
        const Acquired scn = L.acquire(spec(QStringLiteral("user"), {sceneId(1, 1)}, f), &f);
        QVERIFY2(!scn.ok && scn.conflict.kind == Conflict::Kind::Node && scn.conflict.nodeIds == QStringList{sceneId(1, 1)},
                 qPrintable(scn.conflict.message));
        const Acquired seq = L.acquire(spec(QStringLiteral("user"), {QStringLiteral("sq1")}, f), &f);
        QVERIFY(!seq.ok);
        QVERIFY(a.ok && L.release(a.id));
        const Acquired again = L.acquire(spec(QStringLiteral("user"), {shotId(1, 1, 1), shotId(1, 1, 2)}, f), &f);
        QVERIFY2(again.ok, "after the release the shots can be taken");
        for (const QString& id : {again.id, b.id, e.id, g.id}) QVERIFY(L.release(id));
        QVERIFY2(L.size() == 0, "everything released leaves nothing behind");
        QVERIFY(!L.release(QStringLiteral("lk999")));
        const Acquired w = L.acquire(filmSpec(QStringLiteral("main")), &f);
        const Acquired x = L.acquire(spec(QStringLiteral("user"), {shotId(1, 1, 1)}, f), &f);
        QVERIFY2(w.ok && !x.ok && x.conflict.kind == Conflict::Kind::Film
                     && matches(x.conflict.message, "^Generating shots can't start while writing the script is rewriting the whole film"),
                 qPrintable(x.conflict.message));
        QVERIFY(L.filmLock(QStringLiteral("p1")).has_value());
        QVERIFY(L.release(w.id));
        // a key one writes and another reads
        LockSpec sa;
        sa.projectId = QStringLiteral("p1");
        sa.owner = QStringLiteral("main");
        sa.label = QStringLiteral("restyling");
        sa.scope.writes = {QStringLiteral("style")};
        const Acquired sA = L.acquire(sa, &f);
        const Acquired sB = L.acquire(spec(QStringLiteral("user"), {shotId(1, 1, 1)}, f), &f);
        QVERIFY2(sA.ok && !sB.ok && sB.conflict.kind == Conflict::Kind::Field && sB.conflict.fields == QStringList{QStringLiteral("style")}
                     && matches(sB.conflict.message, "^The style can't change while restyling is running"),
                 qPrintable(sB.conflict.message));
        L.release(sA.id);
        // without the tree only equal ids overlap (a caller that has no film at hand)
        const Acquired t1 = L.acquire(spec(QStringLiteral("main"), {shotId(2, 1, 1)}, f), nullptr);
        const Acquired t2 = L.acquire(spec(QStringLiteral("user"), {sceneId(2, 1)}, f), nullptr);
        QVERIFY(t1.ok && t2.ok);
        QVERIFY(changes > 8);
        // ids, info and JSON
        const std::optional<LockInfo> li = L.get(t1.id);
        QVERIFY(li && li->since == m_clock.nowMs() && li->nodes == QStringList{shotId(2, 1, 1)});
        QCOMPARE(li->toJson().value(QStringLiteral("kind")).toString(), QStringLiteral("run"));
        QCOMPARE(L.list(QStringLiteral("p1")).size(), 2);
    }

    void locksReservationsAndOwner() {
        const Film& b = m_fx;
        RunLocks L(&m_clock);
        const QString n1 = shotId(1, 1, 1), n2 = shotId(1, 1, 2), n3 = shotId(1, 1, 3);
        LockSpec res;
        res.projectId = QStringLiteral("p1");
        res.owner = QStringLiteral("br_1");
        res.kind = LockKind::Reserve;
        res.phase = QStringLiteral("branch");
        res.label = S("the side task “Shots 1–2”");
        res.scope.nodes = {n1, n2};
        const Acquired r = L.acquire(res, &b);
        QVERIFY2(r.ok, "a side task reserves its nodes");
        QVERIFY2(!L.checkEdit(QStringLiteral("p1"), b, patched(b, n1, "visual", QStringLiteral("mine")), QStringLiteral("br_1")),
                 "the owner may edit its reserved nodes");
        LockSpec own = spec(QStringLiteral("br_1"), {n1}, b);
        const Acquired ownRun = L.acquire(own, &b);
        QVERIFY2(ownRun.ok, "a step the side task starts on its own nodes is not blocked by its own reservation");
        LockSpec other = res;
        other.owner = QStringLiteral("br_2");
        other.label = S("the side task “X”");
        other.scope.nodes = {n2, n3};
        const Acquired o = L.acquire(other, &b);
        QVERIFY2(!o.ok && o.conflict.nodeIds == QStringList{n2} && o.conflict.message.contains(S("side task “Shots 1–2”")),
                 qPrintable(o.conflict.message));
        const std::optional<Conflict> bad = L.checkEdit(QStringLiteral("p1"), b, patched(b, n2, "visual", QStringLiteral("user typed")), QStringLiteral("user"));
        QVERIFY2(bad && bad->kind == Conflict::Kind::Node && bad->message.contains(S("1.2.2 “Shot 1.1.2”"))
                     && bad->message.contains(S("side task “Shots 1–2”")),
                 bad ? qPrintable(bad->message) : "no conflict");
        const std::optional<Conflict> sr = L.checkEdit(QStringLiteral("p1"), b, patched(b, n1, "visual", QStringLiteral("x")), QStringLiteral("br_1"));
        QVERIFY2(sr && sr->message.contains(QLatin1String("generating shots")),
                 "but while its step runs on a node the step holds it against the starter too");
        QVERIFY2(!L.checkEdit(QStringLiteral("p1"), b, patched(b, shotId(1, 2, 1), "visual", QStringLiteral("x")), QStringLiteral("user")),
                 "the same edit is fine for a node nobody holds");
        const QHash<QString, QList<LockInfo>> all = L.heldNodes(QStringLiteral("p1")), forOwner = L.heldNodes(QStringLiteral("p1"), QStringLiteral("br_1"));
        QVERIFY2(all.contains(n2) && !forOwner.contains(n2) && forOwner.contains(n1),
                 "heldNodes leaves out what the owner's own reservation holds, but not what its running step holds");
        const std::optional<Conflict> ck = L.checkNodes(QStringLiteral("p1"), {shotId(1, 2, 1), n2}, QStringLiteral("br_2"), &b);
        QVERIFY2(ck && ck->nodeIds == QStringList{n2}, "checkNodes says which node is somebody else's");
        const std::optional<Conflict> up = L.checkNodes(QStringLiteral("p1"), {sceneId(1, 1)}, QStringLiteral("br_2"), &b);
        QVERIFY2(up && up->nodeIds == QStringList{sceneId(1, 1)}, "and a node above held ones is not free either");
        QVERIFY(!L.checkNodes(QStringLiteral("p1"), {sceneId(1, 2)}, QStringLiteral("br_2"), &b));
    }

    void locksEditsWhileRunning() {
        const Film& b = m_fx;
        RunLocks L(&m_clock);
        const QStringList held{shotId(1, 1, 1), shotId(1, 1, 2), shotId(1, 1, 3), shotId(1, 2, 1)};
        const Acquired r = L.acquire(spec(QStringLiteral("main"), held, b), &b);
        QVERIFY(r.ok);
        auto t = [&](const Film& next, const QString& owner = QStringLiteral("user")) {
            return L.checkEdit(QStringLiteral("p1"), b, next, owner);
        };
        QVERIFY2(!t(b), "nothing changed is fine");
        QVERIFY2(!t(patched(b, shotId(1, 3, 1), "visual", QStringLiteral("x"))), "another node of the same film: fine");
        const std::optional<Conflict> same = t(patched(b, shotId(1, 1, 2), "visual", QStringLiteral("new")));
        QVERIFY2(same && same->kind == Conflict::Kind::Node && same->nodeIds == QStringList{shotId(1, 1, 2)}
                     && same->message == S("Shot 1.2.2 “Shot 1.1.2” is busy (generating shots). Wait for it to finish, or press "
                                           "Stop; everything else can still be edited."),
                 same ? qPrintable(same->message) : "no conflict");
        Film two = patched(patched(b, shotId(1, 1, 1), "visual", QStringLiteral("a")), shotId(1, 1, 3), "visual", QStringLiteral("b"));
        const std::optional<Conflict> tw = t(two);
        QVERIFY2(tw && matches(tw->message, "^Shots 1\\.2\\.1 “Shot 1\\.1\\.1”, 1\\.2\\.3 “Shot 1\\.1\\.3” are busy"),
                 tw ? qPrintable(tw->message) : "no conflict");
        Film cos = b;
        cos.nodeMut(QStringLiteral("sq1"))->data.insert(QStringLiteral("title"), QStringLiteral("Act one"));
        cos.settingsMut().targetSeconds = 600;
        QVERIFY2(!t(cos), "a sequence title and the target length are not read by a shot step: free");
        const std::optional<Conflict> st = t(entryPatched(b, QStringLiteral("st_1"), "preset", QStringLiteral("noir")));
        QVERIFY2(st && st->kind == Conflict::Kind::Field && st->fields.contains(QStringLiteral("style"))
                     && matches(st->message, "^The style can't change while generating shots is running: it uses it for the shots being made"),
                 st ? qPrintable(st->message) : "no conflict");
        Film an = b;
        an.setOutputs(QStringLiteral("sq1"), {{"anchor_asset", kSha1}});
        const std::optional<Conflict> anc = t(an);
        QVERIFY2(anc && anc->fields == QStringList{QStringLiteral("anchor:sq1")}
                     && anc->message.startsWith(S("The anchor picture of sequence 1 “Sequence 1” can't change while")),
                 anc ? qPrintable(anc->message) : "no conflict");
        Film del = b;
        del.removeSubtree(shotId(1, 1, 3));
        QVERIFY2(t(del), "deleting a held node is refused");
        Film delScene = b;
        delScene.removeSubtree(sceneId(1, 1));
        QVERIFY2(t(delScene), "and so is deleting the scene around it");
        Film delFree = b;
        delFree.removeSubtree(shotId(1, 3, 4));
        QVERIFY2(!t(delFree), "deleting a free node is not");
        Film ins = b;
        Node n;
        n.id = QStringLiteral("new");
        n.kind = NodeKind::Shot;
        n.parentId = sceneId(1, 1);
        n.data = b.node(shotId(1, 1, 2))->data;                             // a copy of a held shot's content
        ins.insertNode(n, 2);
        QVERIFY2(!t(ins), "inserting a node next to held ones (a copy of one, even) is fine");
        Film mv = b;
        mv.moveNode(shotId(1, 1, 4), sceneId(1, 1), 0);
        QVERIFY2(!t(mv), "moving a free node past held ones leaves them untouched: fine");
        Film mvHeld = b;
        mvHeld.moveNode(shotId(1, 1, 2), sceneId(1, 1), 3);
        QVERIFY2(!t(mvHeld), "a held node may move among its siblings (its content is untouched)");
        Film away = b;
        away.moveNode(shotId(1, 1, 2), sceneId(1, 3), 0);
        QVERIFY2(t(away), "moving a held node to another scene changes it: refused");
        // the tree rule: a held node's inputs, listed or not
        LockSpec bare = spec(QStringLiteral("main"), {shotId(2, 1, 1)}, b);
        bare.scope.reads.clear();
        bare.scope.writes.clear();
        bare.label = QStringLiteral("drawing a keyframe");
        const Acquired k = L.acquire(bare, &b);
        QVERIFY(k.ok);
        const std::optional<Conflict> ann = t(entryPatched(b, kAnn, "description", QStringLiteral("older")));
        QVERIFY2(ann && ann->kind == Conflict::Kind::Field && ann->fields == QStringList{QStringLiteral("bible:ch_ann")}
                     && ann->message.contains(S("The character “Ann” can't change while")),
                 ann ? qPrintable(ann->message) : "no conflict");
        L.release(r.id);
        const std::optional<Conflict> ann2 = t(entryPatched(b, kAnn, "description", QStringLiteral("older")));
        QVERIFY2(ann2 && ann2->message.contains(S("(shot 2.2.1 “Shot 2.1.1” uses it.)")), ann2 ? qPrintable(ann2->message) : "no conflict");
        QVERIFY2(!t(entryPatched(b, QStringLiteral("pr_cup"), "description", QStringLiteral("red"))), "a prop the held shot does not show: free");
        QVERIFY2(!t(entryPatched(b, kAnn, "notes", QStringLiteral("x"))), "a note is not an input: free");
        const std::optional<Conflict> inh = t(patched(b, sceneId(2, 1), "location", QStringLiteral("lo_park")));
        QVERIFY2(inh && inh->kind == Conflict::Kind::Node
                     && inh->message == S("Shot 2.2.1 “Shot 2.1.1” is busy (drawing a keyframe), and this edit changes what it is made "
                                          "from (scene 2.2 “Scene 2.1”). Wait for it to finish, or press Stop; everything else can still be edited."),
                 inh ? qPrintable(inh->message) : "no conflict");
        QVERIFY2(!t(patched(b, sceneId(2, 1), "title", QStringLiteral("Night"))), "the scene title is not inherited: free");
        QVERIFY2(t(patched(b, QStringLiteral("sq2"), "video_model", QStringLiteral("vid-b"))), "the sequence's video model is an input");
        QVERIFY2(!t(patched(b, QStringLiteral("sq1"), "video_model", QStringLiteral("vid-b"))), "the other sequence's is not");
        Film asp = b;
        asp.settingsMut().aspect = QStringLiteral("1:1");
        const std::optional<Conflict> as = t(asp);
        QVERIFY2(as && as->fields == QStringList{QStringLiteral("aspect")}, as ? qPrintable(as->message) : "no conflict");
        L.release(k.id);
        // releasing and narrowing
        const Acquired r2 = L.acquire(spec(QStringLiteral("main"), held, b), &b);
        QVERIFY(r2.ok);
        L.releaseNodes(r2.id, {shotId(1, 1, 1), shotId(1, 1, 2)});
        QVERIFY2(!t(patched(b, shotId(1, 1, 2), "visual", QStringLiteral("now free")))
                     && t(patched(b, shotId(1, 1, 3), "visual", QStringLiteral("still busy"))),
                 "nodes the step finished are free at once while it goes on");
        L.narrowNodes(r2.id, {shotId(1, 2, 1)});
        QVERIFY2(!t(patched(b, shotId(1, 1, 3), "visual", QStringLiteral("free"))) && t(patched(b, shotId(1, 2, 1), "visual", QStringLiteral("busy"))),
                 "narrowing keeps only the nodes that still have work");
        L.release(r2.id);
        QVERIFY2(!t(patched(b, shotId(1, 2, 1), "visual", QStringLiteral("free again"))), "after the release anything goes");
        const Acquired w = L.acquire(filmSpec(QStringLiteral("main")), &b);
        const std::optional<Conflict> wb = t(patched(b, shotId(2, 3, 4), "visual", QStringLiteral("x")));
        QVERIFY2(wb && wb->kind == Conflict::Kind::Film
                     && wb->message == QLatin1String("The film can't be changed while writing the script runs: it rewrites the "
                                                     "whole film. Wait for it to finish, or press Stop."),
                 wb ? qPrintable(wb->message) : "no conflict");
        QVERIFY2(!t(b), "but not a no-op");
        QVERIFY(L.writerActive(QStringLiteral("p1")));
        L.release(w.id);
        QVERIFY(!L.writerActive(QStringLiteral("p1")));
    }

    void applyOpsUnderLocks() {
        RunLocks L(&m_clock);
        const Acquired r = L.acquire(spec(QStringLiteral("main"), {shotId(1, 1, 2)}, m_fx), &m_fx);
        QVERIFY(r.ok);
        ApplyOptions o;
        o.locks = &L;
        const OpResult res = applyOps(m_fx, A({O("update_node", {{"node", shotId(1, 1, 1)}, {"visual", "a"}}),
                                               O("update_node", {{"node", shotId(1, 1, 2)}, {"visual", "b"}}),
                                               O("update_node", {{"node", shotId(1, 2, 1)}, {"visual", "c"}}),
                                               O("update_character", {{"character", "Ann"}, {"description", "older"}}),
                                               O("update_character", {{"character", "Bob"}, {"notes", "fine"}})}),
                                      o);
        QCOMPARE(res.applied.size(), 3);
        QCOMPARE(res.errors.size(), 2);
        QVERIFY(matches(res.errors[0], "^Shot 1\\.2\\.2 “Shot 1\\.1\\.2” is busy \\(generating shots\\)"));
        QVERIFY2(matches(res.errors[1], "^The character “Ann” can't change while generating shots is running"), qPrintable(res.errors[1]));
        QCOMPARE(res.film.node(shotId(1, 1, 2))->data.value(QStringLiteral("visual")).toString(), QStringLiteral("shot 1.1.2"));
        QCOMPARE(res.film.node(shotId(1, 1, 1))->data.value(QStringLiteral("visual")).toString(), QStringLiteral("a"));
        QCOMPARE(setOf(res.stale), (QSet<QString>{shotId(1, 1, 1), shotId(1, 2, 1)}));
        // the holder itself (a side task's reservation is the exception) is held off too while its step runs
        o.owner = QStringLiteral("main");
        QCOMPARE(applyOps(m_fx, A({O("update_node", {{"node", shotId(1, 1, 2)}, {"visual", "b"}})}), o).errors.size(), 1);
        // atomic under locks: one refusal, nothing applied
        o.owner = QStringLiteral("user");
        o.atomic = true;
        const OpResult at = applyOps(m_fx, A({O("update_node", {{"node", shotId(1, 1, 1)}, {"visual", "a"}}),
                                              O("update_node", {{"node", shotId(1, 1, 2)}, {"visual", "b"}})}),
                                     o);
        QVERIFY(at.rolledBack && at.applied.isEmpty());
        QCOMPARE(at.film.node(shotId(1, 1, 1))->data.value(QStringLiteral("visual")).toString(), QStringLiteral("shot 1.1.1"));
        L.release(r.id);
        // a side task: its own nodes only, and no structure
        ApplyOptions side;
        side.owner = QStringLiteral("br_1");
        side.scope = QStringList{sceneId(1, 2)};
        const OpResult s = applyOps(m_fx, A({O("update_node", {{"node", shotId(1, 2, 3)}, {"visual", "mine"}}),
                                             O("update_node", {{"node", shotId(1, 3, 1)}, {"visual", "not mine"}}),
                                             O("add_shot", {{"parent", sceneId(1, 2)}}),
                                             O("set_aspect", {{"aspect", "1:1"}}),
                                             O("move_node", {{"node", shotId(1, 2, 3)}, {"after", ""}})}),
                                    side);
        QCOMPARE(s.applied.size(), 1);
        QCOMPARE(s.errors.size(), 4);
        QCOMPARE(s.errors[0], S("Shot 1.4.1 “Shot 1.3.1” is not one of this side task's nodes (sc1_2)."));
        QVERIFY(s.errors[1].startsWith(QLatin1String("A side task may only change its own nodes")));
        QCOMPARE(s.film.settings().aspect, QStringLiteral("16:9"));
    }

    // ------------------------------------------------------------ a seeded shuffle (starmedia test/liveedit-fuzz.ts)

    void lockFuzz() {
        int started = 0, refused = 0, editsApplied = 0, editsRefused = 0, finished = 0;
        for (quint32 seed = 1; seed <= 4; ++seed) {
            std::mt19937 rng(seed);
            auto pick = [&](int n) { return int(rng() % quint32(n)); };
            Film film = m_fx;
            RunLocks L(&m_clock);
            struct Step { QString id; QStringList roots; QHash<QString, QString> used; };
            QList<Step> steps;
            QStringList shots;
            for (int q = 1; q <= 2; ++q)
                for (int s = 1; s <= 3; ++s)
                    for (int k = 1; k <= 4; ++k) shots << shotId(q, s, k);
            const QStringList entries{kAnn, kBob, QStringLiteral("lo_cafe"), QStringLiteral("lo_park"), QStringLiteral("pr_cup")};
            auto heldBy = [&](const QString& id) {
                for (const Step& st : steps)
                    for (const QString& r : st.roots)
                        if (r == id || film.isAncestor(r, id)) return true;
                return false;
            };
            ApplyOptions o;
            o.locks = &L;
            for (int i = 0; i < 300; ++i) {
                const int roll = pick(100);
                QString action;
                if (roll < 30) {                                             // a person edits one shot
                    const QString id = shots[pick(shots.size())];
                    const bool held = heldBy(id);
                    const QString v = QStringLiteral("edit %1").arg(i);
                    const OpResult r = applyOps(film, A({O("update_node", {{"node", id}, {"visual", v}})}), o);
                    action = QStringLiteral("edit %1 (%2)").arg(id, held ? "held" : "free");
                    QVERIFY2(held != r.errors.isEmpty(), qPrintable(action + QLatin1String(": ") + r.errors.join('|')));
                    if (!held) QCOMPARE(r.film.node(id)->data.value(QStringLiteral("visual")).toString(), v);
                    (held ? editsRefused : editsApplied) += 1;
                    film = r.film;
                } else if (roll < 45) {                                      // a bible edit
                    const QString e = entries[pick(entries.size())];
                    bool used = false;                               // what the step read when it started
                    for (const Step& st : steps)
                        for (const QString& r : st.roots)
                            for (const QString& x : film.subtree(r))
                                if (keysUsed(film, *film.node(x)).contains(QStringLiteral("bible:") + e)) used = true;
                    const OpResult r = applyOps(film, A({O("update_bible", {{"entry", e}, {"description", QStringLiteral("d%1").arg(i)}})}), o);
                    action = QStringLiteral("bible %1 (%2)").arg(e, used ? "used by a held node" : "free");
                    QVERIFY2(used != r.errors.isEmpty(), qPrintable(action + QLatin1String(": ") + r.errors.join('|')));
                    (used ? editsRefused : editsApplied) += 1;
                    film = r.film;
                } else if (roll < 70) {                                      // a step starts on 1-3 shots or a scene
                    QStringList roots;
                    if (pick(4) == 0) roots << sceneId(1 + pick(2), 1 + pick(3));
                    else
                        for (int n = 1 + pick(3); n > 0; --n) roots << shots[pick(shots.size())];
                    roots.removeDuplicates();
                    bool overlaps = false;
                    for (const QString& x : roots)
                        for (const Step& st : steps)
                            for (const QString& y : st.roots)
                                if (x == y || film.isAncestor(x, y) || film.isAncestor(y, x)) overlaps = true;
                    const Acquired a = L.acquire(spec(pick(2) ? QStringLiteral("main") : QStringLiteral("user"), roots, film), &film);
                    action = QStringLiteral("step on %1").arg(roots.join(','));
                    QVERIFY2(a.ok == !overlaps, qPrintable(action + QLatin1String(": ") + a.conflict.message));
                    if (a.ok) {
                        Step st{a.id, roots, {}};
                        for (const QString& r : roots)
                            for (const QString& x : film.subtree(r)) st.used.insert(x, inputHash(film, *film.node(x)));
                        steps << st;
                        ++started;
                    } else {
                        ++refused;
                    }
                } else if (roll < 88 && !steps.isEmpty()) {                  // a step finishes: its results land
                    const Step st = steps.takeAt(pick(steps.size()));
                    for (auto it = st.used.constBegin(); it != st.used.constEnd(); ++it)
                        QVERIFY2(markFresh(film, it.key(), it.value()), qPrintable(QStringLiteral("finished %1: %2 changed under its step").arg(st.id, it.key())));
                    QVERIFY(L.release(st.id));
                    ++finished;
                    action = QStringLiteral("finish %1").arg(st.id);
                } else if (!steps.isEmpty()) {                               // a step is stopped
                    const Step st = steps.takeAt(pick(steps.size()));
                    QVERIFY(L.release(st.id));
                    action = QStringLiteral("stop %1").arg(st.id);
                }
                // the incremental pass of applyOps left nothing for a full pass to find
                Film full = film;
                const StaleReport missed = propagate(full);
                QVERIFY2(missed.changed.isEmpty() && missed.lockedDrift.isEmpty(),
                         qPrintable(QStringLiteral("seed %1 after %2: a full pass still finds %3").arg(seed).arg(action, missed.changed.join(','))));
                // invariant: the inputs of every held node are what they were when its step started
                for (const Step& st : steps)
                    for (auto it = st.used.constBegin(); it != st.used.constEnd(); ++it) {
                        const Node* n = film.node(it.key());
                        QVERIFY2(n && inputHash(film, *n) == it.value(),
                                 qPrintable(QStringLiteral("seed %1 after %2: %3 changed under its step").arg(seed).arg(action, it.key())));
                    }
                QVERIFY(L.size() == steps.size());
            }
            for (const Step& st : steps) L.release(st.id);
            QCOMPARE(L.size(), 0);
            QVERIFY(film.problems().isEmpty());
        }
        qInfo("fuzz: %d steps started, %d refused, %d finished; %d edits applied, %d refused", started, refused, finished,
              editsApplied, editsRefused);
        QVERIFY2(started > 20 && refused > 0 && editsApplied > 50 && editsRefused > 10, "the shuffles exercised both sides");
    }

    // ------------------------------------------------------------ a feature-length tree

    void scale() {
        // ~1,600 shots per film is the design's 100-minute estimate; this film has 8,000 shots, 1,600 lines
        Film f(QStringLiteral("big"));
        const QJsonArray bib{O("add_voice", {{"id", "v1"}, {"name", "V"}}),
                             O("add_character", {{"id", "c1"}, {"name", "One"}, {"voice", "v1"}}),
                             O("add_character", {{"id", "c2"}, {"name", "Two"}}), O("add_location", {{"id", "l1"}, {"name", "Here"}}),
                             O("set_style", {{"style", "noir"}})};
        const OpResult b = applyOps(f, bib);
        QVERIFY2(b.errors.isEmpty(), qPrintable(b.errors.join('\n')));
        f = b.film;
        for (int q = 0; q < 20; ++q) {
            Node sq;
            sq.id = QStringLiteral("q%1").arg(q);
            sq.kind = NodeKind::Sequence;
            f.insertNode(sq);
            for (int s = 0; s < 10; ++s) {
                Node sc;
                sc.id = QStringLiteral("q%1s%2").arg(q).arg(s);
                sc.kind = NodeKind::Scene;
                sc.parentId = sq.id;
                sc.data = {{"location", "l1"}, {"characters", A({s % 2 ? "c1" : "c2"})}};
                f.insertNode(sc);
                for (int k = 0; k < 40; ++k) {
                    Node sh;
                    sh.id = QStringLiteral("%1k%2").arg(sc.id).arg(k);
                    sh.kind = NodeKind::Shot;
                    sh.parentId = sc.id;
                    sh.data = {{"visual", QStringLiteral("shot %1").arg(k)}, {"duration", 4}};
                    f.insertNode(sh);
                    if (k % 5 == 0) {
                        Node ln;
                        ln.id = sh.id + QStringLiteral("l");
                        ln.kind = NodeKind::Line;
                        ln.parentId = sh.id;
                        ln.data = {{"speaker", "c1"}, {"text", "Line."}};
                        f.insertNode(ln);
                    }
                }
            }
        }
        QCOMPARE(f.size(), 20 + 200 + 8000 + 1600);
        QVERIFY(f.problems().isEmpty());
        QElapsedTimer t;
        t.start();
        const StaleReport first = propagate(f);
        const qint64 full = t.elapsed();
        QCOMPARE(first.changed.size(), f.size());
        for (const QString& id : f.allIds()) f.nodeMut(id)->stale = false;
        t.restart();
        const OpResult one = applyOps(f, A({O("update_node", {{"node", "q7s3k21"}, {"camera", "tilt"}})}));
        const qint64 oneOp = t.elapsed();
        QCOMPARE(one.stale, QStringList{QStringLiteral("q7s3k21")});
        t.restart();
        const OpResult ch = applyOps(f, A({O("update_character", {{"character", "One"}, {"description", "older"}})}));
        const qint64 charOp = t.elapsed();
        QCOMPARE(ch.stale.size(), 20 * 5 * 40);                              // the shots of the odd scenes
        RunLocks L(&m_clock);
        QStringList held;
        for (int k = 0; k < 40; ++k) held << QStringLiteral("q3s1k%1").arg(k);
        QVERIFY(L.acquire(spec(QStringLiteral("main"), held, f, QStringLiteral("big")), &f).ok);
        // checkEdit compares films before propagation (as applyOps does)
        const Film edited = entryPatched(f, QStringLiteral("c1"), "description", QStringLiteral("older"));
        t.restart();
        const std::optional<Conflict> c = L.checkEdit(QStringLiteral("big"), f, edited, QStringLiteral("user"));
        const qint64 check = t.elapsed();
        QVERIFY(c && c->kind == Conflict::Kind::Field && c->fields == QStringList{QStringLiteral("bible:c1")});
        t.restart();
        QVERIFY(!L.checkEdit(QStringLiteral("big"), f, one.film, QStringLiteral("user")));   // propagated, far away: still fine
        const qint64 checkFar = t.elapsed();
        t.restart();
        const store::Write w = saveWrite(f, one.film);
        const qint64 diff = t.elapsed();
        QVERIFY(bool(w.fn));
        qInfo("9,820 nodes: full propagate %lld ms, one shot edit %lld ms, a character edit %lld ms, checkEdit %lld / %lld ms, "
              "saveWrite diff %lld ms",
              full, oneOp, charOp, check, checkFar, diff);
        // generous bounds: these run on the service's event loop, so a gross regression must fail
        QVERIFY2(full < 5000 && oneOp < 5000 && charOp < 5000 && check < 5000 && checkFar < 5000 && diff < 5000,
                 "too slow for the event loop");
    }
};

QTEST_GUILESS_MAIN(TestStudioFilmModel)
#include "test_studio_film_model.moc"
