// ST-03 store (docs/plans/starmedia-studio.md §4.8, §8): studio.db on SQLite WAL, the schema and its migrations, the
// single DbWriter thread (~50 ms batches, one savepoint per write, money rows and provider-accepted markers committed
// with synchronous=FULL before their callback, which is where the paid request is sent) and the typed repos.
//
// Crash safety is tested with a real process: this executable re-runs itself as "--writer-child <db> <mode>", floods
// the writer, and is killed mid-batch (deterministically from inside the transaction, and by TerminateProcess at random
// moments). Every reopened database must pass integrity_check, hold whole writes only, in order, and hold every write
// whose commit callback had already been reported.
// Hermetic: QTemporaryDir databases only; the only processes started are copies of this test, killed by their own PID.
#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/money.h"
#include "store/db.h"
#include "store/dbwriter.h"
#include "store/migrations.h"
#include "store/repos.h"

using namespace studio;
using namespace studio::store;
using namespace Qt::Literals::StringLiterals;

namespace {

DbOptions opts(const QString& path, int windowMs = 50, const QList<MigrationModule>& modules = {}) {
    DbOptions o;
    o.path = path;
    o.batchWindowMs = windowMs;
    o.modules = modules;
    return o;
}
DbWriter::Options wopts(const QString& path) {
    DbWriter::Options o;
    o.path = path;
    return o;
}
spend::Filter filter(const QString& runId = {}, const QString& kind = {}, qint64 fromMs = 0, qint64 toMs = 0,
                     const QString& projectId = {}) {
    spend::Filter f;
    f.runId = runId;
    f.kind = kind;
    f.fromMs = fromMs;
    f.toMs = toMs;
    f.projectId = projectId;
    return f;
}

qint64 scalar(const QSqlDatabase& db, const QString& sql, const QVariantList& binds = {}) {
    QSqlQuery q(db);
    q.prepare(sql);
    for (int i = 0; i < binds.size(); ++i) q.bindValue(i, binds[i]);
    if (!q.exec() || !q.next()) return -999;
    return q.value(0).toLongLong();
}
QString text(const QSqlDatabase& db, const QString& sql) {
    QSqlQuery q(db);
    if (!q.exec(sql) || !q.next()) return QStringLiteral("<error: %1>").arg(q.lastError().text());
    return q.value(0).toString();
}
bool tableExists(const QSqlDatabase& db, const QString& name) {
    return scalar(db, QStringLiteral("SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name = ?"), {name}) == 1;
}

JobRow job(const QString& id, const QString& run = QStringLiteral("run_t"), const QStringList& deps = {}) {
    JobRow j;
    j.id = id;
    j.runId = run;
    j.kind = QStringLiteral("image");
    j.deps = deps;
    j.createdMs = 1000;
    return j;
}

SpendRow spendRow(const QString& id, const QString& kind, money::Micro micro, qint64 ts = 1793016000000LL) {
    SpendRow s;
    s.id = id;
    s.tsMs = ts;
    s.projectId = QStringLiteral("prj_a");
    s.runId = QStringLiteral("run_a");
    s.kind = kind;
    s.micro = micro;
    return s;
}

// A write that does nothing but succeed (fills batches).
Write noop() { return Write{[](Tx&) {}, Durability::Batched, QStringLiteral("noop")}; }

bool onMainThread() { return QThread::currentThread() == QCoreApplication::instance()->thread(); }

// ---------------- crash tests: the shared fixture and the child process ----------------

MigrationModule crashModule() {
    return MigrationModule{QStringLiteral("crashtest"),
                           {{1, QStringLiteral("counter"),
                             QStringLiteral("CREATE TABLE crash_counter (id INTEGER PRIMARY KEY, v INTEGER NOT NULL) STRICT;\n"
                                            "INSERT INTO crash_counter (id, v) VALUES (1, 0);")}}};
}

std::atomic<qint64> g_lastExecuted{0};

// Write n of the flood: a job with two edges, the counter, and every 50th one a durable money row. All or nothing.
Write crashWrite(qint64 n) {
    Write w;
    w.tag = QStringLiteral("crash");
    w.durability = n % 50 == 0 ? Durability::Durable : Durability::Batched;
    w.fn = [n](Tx& tx) {
        g_lastExecuted.store(n);
        JobRow j = job(QStringLiteral("crash_%1").arg(n), QStringLiteral("crash"),
                       {QStringLiteral("crash_root_a"), QStringLiteral("crash_root_b")});
        j.priority = int(n);
        jobs::insert({j}).fn(tx);
        if (tx.failed()) return;
        if (!tx.exec(QStringLiteral("UPDATE crash_counter SET v = v + 1 WHERE id = 1"))) return;
        if (n % 50 == 0) {
            SpendRow s = spendRow(QStringLiteral("crash_spend_%1").arg(n), QStringLiteral("charge"), 1);
            s.source = QStringLiteral("actual");
            spend::append(s).fn(tx);
        }
    };
    return w;
}

void childPrint(const QByteArray& line) {
    std::fputs(line.constData(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

[[noreturn]] void dieNow() {
#ifdef Q_OS_WIN
    TerminateProcess(GetCurrentProcess(), 9);
#endif
    _exit(9);
}

// "--writer-child <db> flood|die": opens the database, then keeps about 5,000 writes queued until it is killed (flood),
// or kills itself from inside the transaction at write 51 of the first batch >= 3 (die). Prints "READY <next n>",
// "ACK <n>" once the commit callback of write n ran (callbacks arrive in order, so every write <= n is committed) and
// "DYING <last executed n>". Quits by itself after 20 s in case nobody kills it.
int writerChild(int argc, char** argv, const QString& path, const QString& mode) {
    QCoreApplication app(argc, argv);
    DbOptions o;
    o.path = path;
    o.modules = {crashModule()};
    o.batchWindowMs = 20;
    o.maxBatchOps = 1000;
    if (mode == u"die") {
        o.afterWriteHook = [](quint64 batch, int index) {
            if (batch >= 3 && index == 50) {
                childPrint("DYING " + QByteArray::number(g_lastExecuted.load()));
                dieNow();
            }
        };
    }
    Db db(o);
    QString err;
    if (!db.open(&err)) {
        childPrint("ERROR " + err.toUtf8());
        return 2;
    }
    qint64 next = 1 + scalar(db.reader(), QStringLiteral("SELECT COALESCE(MAX(priority), 0) FROM jobs WHERE run_id = 'crash'"));
    childPrint("READY " + QByteArray::number(next));
    QObject ctx;
    qint64 maxAck = 0, printed = 0;
    QTimer pump;
    pump.setInterval(0);
    QObject::connect(&pump, &QTimer::timeout, &ctx, [&] {
        for (int i = 0; i < 2000 && db.writer().stats().queued < 5000; ++i, ++next) {
            const qint64 n = next;
            if (!db.writer().submit(crashWrite(n), &ctx, [&maxAck, n](const WriteResult& r) {
                    if (!r.ok) {
                        childPrint("FAIL " + QByteArray::number(n) + " " + r.error.toUtf8());
                        dieNow();
                    }
                    maxAck = qMax(maxAck, n);
                }))
                break;
        }
    });
    QTimer printer;
    printer.setInterval(5);
    QObject::connect(&printer, &QTimer::timeout, &ctx, [&] {
        if (maxAck != printed) {
            childPrint("ACK " + QByteArray::number(maxAck));
            printed = maxAck;
        }
    });
    pump.start();
    printer.start();
    QTimer::singleShot(20000, &app, [&] {
        childPrint("TIMEOUT");
        app.quit();
    });
    const int rc = app.exec();
    pump.stop();
    db.close();
    return rc;
}

struct ChildOut {
    QByteArray buf;
    qint64 ready = -1, lastAck = 0, dying = -1;
    QStringList problems;                  // ERROR / FAIL / TIMEOUT lines
};

void parseChild(ChildOut& o, const QByteArray& more) {
    o.buf += more;
    for (qsizetype nl; (nl = o.buf.indexOf('\n')) >= 0;) {
        const QByteArray line = o.buf.left(nl).trimmed();
        o.buf.remove(0, nl + 1);
        if (line.startsWith("READY ")) o.ready = line.mid(6).toLongLong();
        else if (line.startsWith("ACK ")) o.lastAck = qMax(o.lastAck, line.mid(4).toLongLong());
        else if (line.startsWith("DYING ")) o.dying = line.mid(6).toLongLong();
        else if (!line.isEmpty()) o.problems << QString::fromUtf8(line);
    }
}

bool readChildUntil(QProcess& p, ChildOut& o, const std::function<bool()>& done, int timeoutMs) {
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (p.state() == QProcess::NotRunning) {
            parseChild(o, p.readAllStandardOutput());
            return done();
        }
        if (t.elapsed() > timeoutMs) return false;
        p.waitForReadyRead(50);
        parseChild(o, p.readAllStandardOutput());
    }
    return true;
}

struct CrashState {
    QString openError, integrity;
    qint64 count = -1, minN = -1, maxN = -1, counter = -1, edges = -1, spends = -1;
};

// Opens the database after a crash (read-write, so SQLite rolls the WAL back) and reads the flood's invariants.
CrashState inspectCrash(const QString& path) {
    CrashState s;
    ScopedConnection c(path, ScopedConnection::Mode::ReadWrite);
    if (!c.isOpen()) {
        s.openError = c.error();
        return s;
    }
    const QSqlDatabase db = c.db();
    s.integrity = text(db, QStringLiteral("PRAGMA integrity_check"));
    s.count = scalar(db, QStringLiteral("SELECT count(*) FROM jobs WHERE run_id = 'crash'"));
    s.minN = scalar(db, QStringLiteral("SELECT COALESCE(MIN(priority), 0) FROM jobs WHERE run_id = 'crash'"));
    s.maxN = scalar(db, QStringLiteral("SELECT COALESCE(MAX(priority), 0) FROM jobs WHERE run_id = 'crash'"));
    s.counter = scalar(db, QStringLiteral("SELECT v FROM crash_counter WHERE id = 1"));
    s.edges = scalar(db, QStringLiteral("SELECT count(*) FROM job_edges WHERE to_id LIKE 'crash!_%' ESCAPE '!'"));
    s.spends = scalar(db, QStringLiteral("SELECT count(*) FROM spend WHERE id LIKE 'crash!_spend!_%' ESCAPE '!'"));
    return s;
}

QString describe(const CrashState& s) {
    return QStringLiteral("integrity=%1 count=%2 min=%3 max=%4 counter=%5 edges=%6 spends=%7 %8")
        .arg(s.integrity).arg(s.count).arg(s.minN).arg(s.maxN).arg(s.counter).arg(s.edges).arg(s.spends).arg(s.openError);
}

// Whole writes only, in order, with nothing missing in between.
void verifyConsistent(const CrashState& s) {
    QVERIFY2(s.openError.isEmpty(), qPrintable(s.openError));
    QCOMPARE(s.integrity, QStringLiteral("ok"));
    QVERIFY2(s.count > 0, qPrintable(describe(s)));
    QCOMPARE(s.minN, 1);
    QCOMPARE(s.count, s.maxN);             // contiguous 1..max: no write lost in the middle, none half-applied
    QCOMPARE(s.counter, s.count);          // the counter update of every write landed with its job
    QCOMPARE(s.edges, 2 * s.count);        // and both edges
    QCOMPARE(s.spends, s.maxN / 50);       // and every 50th write's durable money row
}

} // namespace

class TestStudioStore : public QObject {
    Q_OBJECT

private slots:
    void init() {
        // A connection removed while a query still uses it is a leak in the store or in this test.
        QTest::failOnWarning(QRegularExpression(QStringLiteral("removeDatabase|still in use|does not belong")));
    }

    void splitter();
    void migrationsFresh();
    void migrationsModules();
    void migrationsRefusals();
    void openGuards();
    void strictMoney();
    void repos();
    void batchedInserts10k();
    void writeIsolation();
    void txGuards();
    void commitBeforeSend();
    void callbackThreads();
    void batchWindowAndFlush();
    void lockedDatabase();
    void stuckWriterStop();
    void crashMidBatch();
    void crashKillRandom();
};

void TestStudioStore::splitter() {
    QStringList s;
    QString err;
    QVERIFY(splitStatements(u"a; b ;; c", &s));
    QCOMPARE(s, (QStringList{u"a"_s, u"b"_s, u"c"_s}));

    // ';' inside every quote kind stays, quotes are kept verbatim (doubled quotes are escapes).
    QVERIFY(splitStatements(u"INSERT INTO t VALUES ('x;  y', \"q;\", `b;`, [s;]);SELECT 'it''s; fine'", &s));
    QCOMPARE(s.size(), 2);
    QCOMPARE(s[0], u"INSERT INTO t VALUES ('x;  y', \"q;\", `b;`, [s;])"_s);
    QCOMPARE(s[1], u"SELECT 'it''s; fine'"_s);

    // Comments go, whitespace collapses.
    QVERIFY(splitStatements(u"SELECT 1; -- c; d\nSELECT /* x; */ 2;\n  CREATE  TABLE\n\tt (a)  ", &s));
    QCOMPARE(s, (QStringList{u"SELECT 1"_s, u"SELECT 2"_s, u"CREATE TABLE t (a)"_s}));

    // Trigger bodies hold statements; CASE ... END inside does not end the trigger.
    QVERIFY(splitStatements(u"CREATE TRIGGER tr AFTER INSERT ON t BEGIN\n"
                            "  UPDATE c SET v = CASE WHEN new.a > 0 THEN 1 ELSE 2 END;\n"
                            "  INSERT INTO l VALUES (1);\nEND;\nSELECT 1;"
                            "create temp trigger t2 before delete on t begin select 1; end",
                            &s));
    QCOMPARE(s.size(), 3);
    QVERIFY(s[0].startsWith(u"CREATE TRIGGER tr"));
    QVERIFY(s[0].endsWith(u"INSERT INTO l VALUES (1); END"));
    QCOMPARE(s[1], u"SELECT 1"_s);
    QCOMPARE(s[2], u"create temp trigger t2 before delete on t begin select 1; end"_s);

    QVERIFY(!splitStatements(u"SELECT 'open", &s, &err));
    QVERIFY2(err.contains(u"unterminated"), qPrintable(err));
    QVERIFY(!splitStatements(u"SELECT 1 /* open", &s, &err));
    QVERIFY(!splitStatements(u"CREATE TRIGGER x AFTER INSERT ON t BEGIN SELECT 1;", &s, &err));
    QVERIFY2(err.contains(u"TRIGGER"), qPrintable(err));

    // The hash ignores comments and spacing, never the statements.
    QStringList a, b, c;
    QVERIFY(splitStatements(u"CREATE TABLE t (a INTEGER); -- one", &a));
    QVERIFY(splitStatements(u"-- reworded comment\nCREATE   TABLE t\n(a INTEGER)", &b));
    QVERIFY(splitStatements(u"CREATE TABLE t (a INTEGER, b INTEGER)", &c));
    QCOMPARE(stepHash(a), stepHash(b));
    QVERIFY(stepHash(a) != stepHash(c));
    QCOMPARE(stepHash(a).size(), 64);

    // The real schema splits cleanly.
    const MigrationModule core = coreModule();
    QCOMPARE(core.name, u"core"_s);
    QCOMPARE(core.steps.size(), coreSchemaVersion());
    QVERIFY(splitStatements(core.steps[0].sql, &s, &err));
    QVERIFY2(s.size() > 40, qPrintable(QString::number(s.size())));
}

void TestStudioStore::migrationsFresh() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("sub/dir/studio.db"));     // the directory is created
    {
        Db db(opts(path));
        QString err;
        QVERIFY2(db.open(&err), qPrintable(err));
        QCOMPARE(db.migrations().applied, QStringList{u"core/1 base schema"_s});
        QCOMPARE(db.migrations().coreVersion, 1);
        const QSqlDatabase r = db.reader();
        for (const char* t : {"projects", "film_nodes", "bible", "runs", "jobs", "job_edges", "artifacts", "takes", "spend",
                              "approvals", "agents", "agent_messages", "models", "model_health", "voices", "voice_audit",
                              "api_keys", "idem", "events", "memory_notes", "audit", "schema_migrations"})
            QVERIFY2(tableExists(r, QString::fromLatin1(t)), t);
        QCOMPARE(scalar(r, QStringLiteral("PRAGMA user_version")), 1);
        QCOMPARE(text(r, QStringLiteral("PRAGMA journal_mode")), u"wal"_s);
        QStringList stmts;
        QVERIFY(splitStatements(coreModule().steps[0].sql, &stmts));
        QSqlQuery q(r);
        QVERIFY(q.exec(QStringLiteral("SELECT module, version, name, sha256, applied_ms FROM schema_migrations")));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), u"core"_s);
        QCOMPARE(q.value(1).toInt(), 1);
        QCOMPARE(q.value(3).toString(), stepHash(stmts));
        QVERIFY(q.value(4).toLongLong() > 1700000000000LL);
        QVERIFY(!q.next());
        q.finish();
        // The reader cannot write.
        QSqlQuery w(r);
        QVERIFY(!w.exec(QStringLiteral("INSERT INTO audit (ts_ms, action) VALUES (1, 'x')")));
        QVERIFY(!db.sqliteVersionText().isEmpty());
    }
    Db again(opts(path));
    QString err;
    QVERIFY2(again.open(&err), qPrintable(err));
    QVERIFY(again.migrations().applied.isEmpty());   // nothing re-applied
    QCOMPARE(again.migrations().coreVersion, 1);
}

void TestStudioStore::migrationsModules() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("studio.db"));
    const MigrationStep s1{1, QStringLiteral("film table"), QStringLiteral("CREATE TABLE film_x (id TEXT PRIMARY KEY NOT NULL) STRICT")};
    const MigrationStep s2{2, QStringLiteral("film index"), QStringLiteral("CREATE INDEX film_x_id ON film_x (id)")};
    const MigrationStep s3{3, QStringLiteral("film column"), QStringLiteral("ALTER TABLE film_x ADD COLUMN note TEXT NOT NULL DEFAULT ''")};
    QString err;
    {
        Db db(opts(path, 50, {MigrationModule{QStringLiteral("film_x"), {s1, s2}}}));
        QVERIFY2(db.open(&err), qPrintable(err));
        QCOMPARE(db.migrations().applied,
                 (QStringList{u"core/1 base schema"_s, u"film_x/1 film table"_s, u"film_x/2 film index"_s}));
    }
    {
        Db db(opts(path, 50, {MigrationModule{QStringLiteral("film_x"), {s1, s2, s3}}}));
        QVERIFY2(db.open(&err), qPrintable(err));
        QCOMPARE(db.migrations().applied, QStringList{u"film_x/3 film column"_s});
        QCOMPARE(scalar(db.reader(), QStringLiteral("SELECT count(*) FROM pragma_table_info('film_x') WHERE name = 'note'")), 1);
    }
    {
        // An older build of the module: the database is newer than it.
        Db db(opts(path, 50, {MigrationModule{QStringLiteral("film_x"), {s1, s2}}}));
        QVERIFY(!db.open(&err));
        QVERIFY2(err.contains(u"newer"), qPrintable(err));
        QVERIFY(!db.isOpen());
    }
    {
        // A tool that does not load the module still opens the database.
        Db db(opts(path));
        QVERIFY2(db.open(&err), qPrintable(err));
        QVERIFY(db.migrations().applied.isEmpty());
    }
}

void TestStudioStore::migrationsRefusals() {
    QTemporaryDir dir;
    QString err;
    const QString path = dir.filePath(QStringLiteral("studio.db"));

    // A failing step is rolled back whole and recorded nowhere; earlier steps stay applied.
    {
        const MigrationModule bad{QStringLiteral("bad"),
                                  {{1, QStringLiteral("two tables"),
                                    QStringLiteral("CREATE TABLE t1 (a INTEGER) STRICT; CREATE TABLE t1 (b INTEGER)")}}};
        Db db(opts(path, 50, {bad}));
        QVERIFY(!db.open(&err));
        QVERIFY2(err.contains(u"bad/1") && err.contains(u"rolled back"), qPrintable(err));
        ScopedConnection c(path, ScopedConnection::Mode::ReadOnly);
        QVERIFY(c.isOpen());
        QVERIFY(!tableExists(c.db(), QStringLiteral("t1")));
        QCOMPARE(scalar(c.db(), QStringLiteral("SELECT count(*) FROM schema_migrations WHERE module = 'core'")), 1);
        QCOMPARE(scalar(c.db(), QStringLiteral("SELECT count(*) FROM schema_migrations WHERE module = 'bad'")), 0);
    }
    // A released step that changed is refused; a comment-only change is not.
    {
        const MigrationModule v1{QStringLiteral("chg"), {{1, QStringLiteral("c1"), QStringLiteral("CREATE TABLE c1 (a INTEGER) STRICT")}}};
        const MigrationModule comment{QStringLiteral("chg"),
                                      {{1, QStringLiteral("c1"), QStringLiteral("-- reworded\nCREATE TABLE c1\n  (a INTEGER)   STRICT;")}}};
        const MigrationModule edited{QStringLiteral("chg"),
                                     {{1, QStringLiteral("c1"), QStringLiteral("CREATE TABLE c1 (a INTEGER, b INTEGER) STRICT")}}};
        { Db db(opts(path, 50, {v1})); QVERIFY2(db.open(&err), qPrintable(err)); }
        { Db db(opts(path, 50, {comment})); QVERIFY2(db.open(&err), qPrintable(err)); }
        Db db(opts(path, 50, {edited}));
        QVERIFY(!db.open(&err));
        QVERIFY2(err.contains(u"chg/1") && err.contains(u"changed"), qPrintable(err));
    }
    // A newer core schema (user_version or a recorded core/2) is refused; a gap in a module is refused.
    {
        {
            ScopedConnection c(path, ScopedConnection::Mode::ReadWrite);
            QSqlQuery q(c.db());
            QVERIFY(q.exec(QStringLiteral("PRAGMA user_version = 99")));
        }
        Db db(opts(path));
        QVERIFY(!db.open(&err));
        QVERIFY2(err.contains(u"newer"), qPrintable(err));
        {
            ScopedConnection c(path, ScopedConnection::Mode::ReadWrite);
            QSqlQuery q(c.db());
            QVERIFY(q.exec(QStringLiteral("PRAGMA user_version = 1")));
            QVERIFY(q.exec(QStringLiteral("INSERT INTO schema_migrations VALUES ('core', 2, 'future', 'x', 1)")));
        }
        QVERIFY(!db.open(&err));
        QVERIFY2(err.contains(u"core/2") && err.contains(u"newer"), qPrintable(err));
        {
            ScopedConnection c(path, ScopedConnection::Mode::ReadWrite);
            QSqlQuery q(c.db());
            QVERIFY(q.exec(QStringLiteral("DELETE FROM schema_migrations WHERE module = 'core' AND version = 2")));
            QVERIFY(q.exec(QStringLiteral("INSERT INTO schema_migrations VALUES ('gapm', 2, 'two', 'x', 1)")));
        }
        const MigrationModule gapm{QStringLiteral("gapm"),
                                   {{1, QStringLiteral("one"), QStringLiteral("SELECT 1")},
                                    {2, QStringLiteral("two"), QStringLiteral("SELECT 2")},
                                    {3, QStringLiteral("three"), QStringLiteral("SELECT 3")}}};
        Db g(opts(path, 50, {gapm}));
        QVERIFY(!g.open(&err));
        QVERIFY2(err.contains(u"gap"), qPrintable(err));
        QVERIFY2(db.open(&err), qPrintable(err));   // without that module the database is fine again
    }
    // Not a Studio database: refused before anything is written (the journal mode stays as it was).
    {
        const QString foreign = dir.filePath(QStringLiteral("foreign.db"));
        {
            ScopedConnection c(foreign, ScopedConnection::Mode::ReadWrite);
            QSqlQuery q(c.db());
            QVERIFY(q.exec(QStringLiteral("CREATE TABLE settings (k TEXT, v TEXT)")));
        }
        Db db(opts(foreign));
        QVERIFY(!db.open(&err));
        QVERIFY2(err.contains(u"not a PlayPlayStudio database"), qPrintable(err));
        ScopedConnection c(foreign, ScopedConnection::Mode::ReadOnly);
        QCOMPARE(text(c.db(), QStringLiteral("PRAGMA journal_mode")), u"delete"_s);
        QVERIFY(!tableExists(c.db(), QStringLiteral("schema_migrations")));
    }
    // Malformed module lists.
    {
        const QString p2 = dir.filePath(QStringLiteral("p2.db"));
        const MigrationStep one{1, QStringLiteral("x"), QStringLiteral("SELECT 1")};
        const MigrationStep two{2, QStringLiteral("y"), QStringLiteral("SELECT 2")};
        QVERIFY(!Db(opts(p2, 50, {MigrationModule{QStringLiteral("Bad-Name"), {one}}})).open(&err));
        QVERIFY(!Db(opts(p2, 50, {MigrationModule{QStringLiteral("core"), {one}}})).open(&err));
        QVERIFY2(err.contains(u"twice"), qPrintable(err));
        QVERIFY(!Db(opts(p2, 50, {MigrationModule{QStringLiteral("skip"), {two}}})).open(&err));
        QVERIFY(!Db(opts(p2, 50, {MigrationModule{QStringLiteral("empty"), {}}})).open(&err));
        QVERIFY(!Db(opts(p2, 50, {MigrationModule{QStringLiteral("q"), {{1, u"q"_s, u"SELECT 'open"_s}}}})).open(&err));
    }
}

void TestStudioStore::openGuards() {
    QTemporaryDir dir;
    QString err;
    QVERIFY(!Db(opts(QStringLiteral("relative/studio.db"))).open(&err));
    QVERIFY2(err.contains(u"absolute"), qPrintable(err));
    QVERIFY(!Db(opts(QStringLiteral(":memory:"))).open(&err));
    QVERIFY(!Db(opts(QString())).open(&err));
    QVERIFY(!Db(opts(dir.path())).open(&err));                        // a directory
    QVERIFY2(err.contains(u"not a file"), qPrintable(err));

    // Network paths are refused before anything touches them (lexical check; nothing is contacted).
    QVERIFY(isNetworkPath(QStringLiteral("\\\\studio-nohost.invalid\\share\\studio.db")));
    QVERIFY(isNetworkPath(QStringLiteral("//studio-nohost.invalid/share/studio.db")));
    QVERIFY(isNetworkPath(QStringLiteral("\\\\?\\UNC\\studio-nohost.invalid\\share\\studio.db")));
    QVERIFY(isNetworkPath(QStringLiteral("\\\\.\\UNC\\studio-nohost.invalid\\share\\studio.db")));
    QVERIFY(isNetworkPath(QStringLiteral("\\\\.\\unc\\studio-nohost.invalid\\share\\studio.db")));
    QVERIFY(!isNetworkPath(dir.filePath(QStringLiteral("studio.db"))));
    QVERIFY(!isNetworkPath(QStringLiteral("\\\\.\\") + QDir::toNativeSeparators(dir.filePath(QStringLiteral("x.db")))));
    QVERIFY(!isNetworkPath(QStringLiteral("\\\\?\\") + QDir::toNativeSeparators(dir.filePath(QStringLiteral("x.db")))));
    QVERIFY(!Db(opts(QStringLiteral("\\\\studio-nohost.invalid\\share\\studio.db"))).open(&err));
    QVERIFY2(err.contains(u"network"), qPrintable(err));

    // A writer started twice, and a writer on a path it cannot open.
    DbWriter w(wopts(dir.filePath(QStringLiteral("nodir/also/none/x.db"))));
    QVERIFY(!w.start(&err));
    QVERIFY(!w.running());
    QVERIFY(!w.submit(noop()));
}

void TestStudioStore::strictMoney() {
    QTemporaryDir dir;
    Db db(opts(dir.filePath(QStringLiteral("studio.db")), 1000));
    QString err;
    QVERIFY2(db.open(&err), qPrintable(err));
    QList<WriteResult> got;
    QObject ctx;
    auto collect = [&](const WriteResult& r) { got << r; };
    auto raw = [](const QString& sql, QVariantList binds) {
        return Write{[sql, binds](Tx& tx) { tx.exec(sql, binds); }, Durability::Batched, QStringLiteral("raw")};
    };
    const QString ins = QStringLiteral("INSERT INTO spend (id, ts_ms, kind, micro) VALUES (?, 1, 'charge', ?)");
    // The two spend::append writes are Durable and wake the writer at once, which could cut the batch before the last
    // writes are queued: hold the writer in a write of its own until all seven are queued, so they share the next one.
    std::atomic<bool> gateIn{false}, gateOpen{false};
    QVERIFY(db.writer().submit(Write{[&gateIn, &gateOpen](Tx&) {
                                         gateIn = true;
                                         QElapsedTimer t;
                                         t.start();
                                         while (!gateOpen.load() && t.elapsed() < 10000) QThread::msleep(1);
                                     },
                                     Durability::Durable, QStringLiteral("gate")}));
    QVERIFY(QTest::qWaitFor([&] { return gateIn.load(); }, 3000));
    QVERIFY(db.writer().submit(raw(ins, {u"s1"_s, 1.5}), &ctx, collect));           // a fractional dollar double
    QVERIFY(db.writer().submit(raw(ins, {u"s2"_s, u"12abc"_s}), &ctx, collect));     // text
    QVERIFY(db.writer().submit(raw(ins, {u"s3"_s, -5}), &ctx, collect));             // negative: CHECK
    QVERIFY(db.writer().submit(raw(QStringLiteral("INSERT INTO jobs (id, estimate_micro) VALUES ('j', -1)"), {}), &ctx, collect));
    QVERIFY(db.writer().submit(spend::append(spendRow(u"s4"_s, u"charge"_s, -5)), &ctx, collect));   // the repo refuses first
    QVERIFY(db.writer().submit(spend::append(spendRow(u"s5"_s, u"refund"_s, 5)), &ctx, collect));
    QVERIFY(db.writer().submit(raw(ins, {u"s6"_s, 250000}), &ctx, collect));         // a plain integer is fine
    gateOpen = true;
    QVERIFY(QTest::qWaitFor([&] { return got.size() == 7; }, 5000));
    QVERIFY2(!got[0].ok && got[0].error.contains(u"REAL", Qt::CaseInsensitive), qPrintable(got[0].error));
    QVERIFY2(!got[1].ok && got[1].error.contains(u"TEXT", Qt::CaseInsensitive), qPrintable(got[1].error));
    QVERIFY2(!got[2].ok && got[2].error.contains(u"CHECK", Qt::CaseInsensitive), qPrintable(got[2].error));
    QVERIFY2(!got[3].ok && got[3].error.contains(u"CHECK", Qt::CaseInsensitive), qPrintable(got[3].error));
    QVERIFY2(!got[4].ok && got[4].error.contains(u"negative"), qPrintable(got[4].error));
    QVERIFY2(!got[5].ok && got[5].error.contains(u"kind"), qPrintable(got[5].error));
    QVERIFY2(got[6].ok, qPrintable(got[6].error));
    for (const WriteResult& r : got) QCOMPARE(r.batch, got[0].batch);   // one batch; the failures did not sink s6
    QCOMPARE(scalar(db.reader(), QStringLiteral("SELECT count(*) FROM spend")), 1);
    QCOMPARE(scalar(db.reader(), QStringLiteral("SELECT typeof(micro) = 'integer' FROM spend")), 1);
}

void TestStudioStore::repos() {
    QTemporaryDir dir;
    Db db(opts(dir.filePath(QStringLiteral("studio.db")), 5));
    QString err;
    QVERIFY2(db.open(&err), qPrintable(err));
    DbWriter& w = db.writer();
    const QSqlDatabase r = db.reader();
    QObject ctx;
    QList<WriteResult> got;
    auto collect = [&](const WriteResult& res) { got << res; };
    auto run = [&](Write wr) {
        const qsizetype before = got.size();
        if (!w.submit(std::move(wr), &ctx, collect)) return WriteResult{};
        if (!QTest::qWaitFor([&] { return got.size() > before; }, 5000)) return WriteResult{};
        return got.last();
    };

    // projects: upsert keeps created_ms
    ProjectRow p;
    p.id = QStringLiteral("prj_a");
    p.title = QStringLiteral("Film A");
    p.brief = QJsonObject{{u"logline"_s, u"x"_s}};
    p.budget = money::usd(25);
    p.createdMs = 100;
    p.updatedMs = 100;
    QVERIFY(run(projects::upsert(p)).ok);
    p.title = QStringLiteral("Film A2");
    p.createdMs = 999;
    p.updatedMs = 200;
    QVERIFY(run(projects::upsert(p)).ok);
    auto gp = projects::get(r, u"prj_a"_s);
    QVERIFY(gp);
    QCOMPARE(gp->title, u"Film A2"_s);
    QCOMPARE(gp->createdMs, 100);
    QCOMPARE(gp->updatedMs, 200);
    QCOMPARE(gp->budget, money::usd(25));
    QCOMPARE(gp->brief.value(u"logline").toString(), u"x"_s);
    QCOMPARE(gp->status, u"draft"_s);
    QCOMPARE(projects::list(r).size(), 1);
    QVERIFY(!projects::get(r, u"nope"_s));
    QVERIFY(!run(projects::upsert(ProjectRow{})).ok);           // empty id

    // spent_micro is the booked-money counter: a rename upsert built from a snapshot read before a spend was booked
    // must not undo that spend (only spend writes move it).
    ProjectRow stale = *projects::get(r, u"prj_a"_s);
    QCOMPARE(stale.spent, 0);
    const Write book{[](Tx& tx) {
                         tx.exec(QStringLiteral("UPDATE projects SET spent_micro = spent_micro + ? WHERE id = ?"),
                                 {money::cents(7), u"prj_a"_s});
                     },
                     Durability::Durable, QStringLiteral("book")};
    QVERIFY(run(book).ok);
    stale.title = QStringLiteral("Film A3");
    stale.budget = money::usd(30);
    stale.spent = money::usd(99);                               // whatever the caller carries is ignored on update
    QVERIFY(run(projects::upsert(stale)).ok);
    gp = projects::get(r, u"prj_a"_s);
    QCOMPARE(gp->title, u"Film A3"_s);
    QCOMPARE(gp->budget, money::usd(30));
    QCOMPARE(gp->spent, money::cents(7));
    QCOMPARE(gp->createdMs, 100);

    // runs
    RunRow rr;
    rr.id = QStringLiteral("run_a");
    rr.projectId = QStringLiteral("prj_a");
    rr.budget = money::usd(10);
    rr.ceiling = money::scale(rr.budget, 115, 100);
    rr.plan = QJsonObject{{u"stages"_s, 3}};
    rr.startedMs = 300;
    QVERIFY(run(runs::insert(rr)).ok);
    QVERIFY(run(runs::setState(u"run_a"_s, u"running"_s)).ok);
    auto gr = runs::get(r, u"run_a"_s);
    QVERIFY(gr);
    QCOMPARE(gr->state, u"running"_s);
    QCOMPARE(gr->ceiling, 11500000);
    QCOMPARE(gr->plan.value(u"stages").toInt(), 3);
    QCOMPARE(gr->finishedMs, 0);
    QVERIFY(run(runs::setState(u"run_a"_s, u"done"_s, 900)).ok);
    QCOMPARE(runs::get(r, u"run_a"_s)->finishedMs, 900);
    QCOMPARE(runs::listByProject(r, u"prj_a"_s).size(), 1);
    const WriteResult missingRun = run(runs::setState(u"run_none"_s, u"done"_s));
    QVERIFY(!missingRun.ok && missingRun.error.contains(u"no run"));

    // jobs + edges
    JobRow a = job(u"job_a"_s, u"run_a"_s);
    a.fallbacks = {u"m2"_s, u"m3"_s};
    a.outputs = QJsonObject{{u"files"_s, 0}};
    a.estimate = money::cents(4);
    a.priority = 1;
    JobRow b = job(u"job_b"_s, u"run_a"_s, {u"job_a"_s});
    JobRow c = job(u"job_c"_s, u"run_a"_s, {u"job_a"_s, u"job_b"_s, u"job_a"_s});   // duplicate dep collapses
    QVERIFY(run(jobs::insert({a, b, c})).ok);
    auto gj = jobs::get(r, u"job_c"_s);
    QVERIFY(gj);
    QCOMPARE(gj->deps, (QStringList{u"job_a"_s, u"job_b"_s}));
    QCOMPARE(gj->indegree, 2);
    QCOMPARE(gj->state, u"pending"_s);
    auto ga = jobs::get(r, u"job_a"_s);
    QCOMPARE(ga->fallbacks, (QStringList{u"m2"_s, u"m3"_s}));
    QCOMPARE(ga->outputs.value(u"files").toInt(), 0);
    QCOMPARE(ga->estimate, 40000);
    QCOMPARE(ga->indegree, 0);
    QCOMPARE(jobs::dependents(r, u"job_a"_s), (QStringList{u"job_b"_s, u"job_c"_s}));
    QCOMPARE(jobs::count(r, u"run_a"_s), 3);
    QCOMPARE(jobs::list(r, u"run_a"_s).first().id, u"job_b"_s);      // priority 0 before 1, then id
    // all or nothing: the second row's bad dependency sinks the first row too
    QVERIFY(!run(jobs::insert({job(u"job_d"_s), job(u"job_e"_s, u"run_t"_s, {u"job_e"_s})})).ok);
    QVERIFY(!jobs::get(r, u"job_d"_s));
    QVERIFY(!run(jobs::insert({job(u"job_a"_s)})).ok);              // duplicate id

    QVERIFY(run(jobs::setState(u"job_a"_s, u"running"_s, 1000)).ok);
    QVERIFY(run(jobs::setState(u"job_a"_s, u"running"_s, 2000)).ok);
    QCOMPARE(jobs::get(r, u"job_a"_s)->startedMs, 1000);            // stamped once
    const WriteResult acc = run(jobs::markAccepted(u"job_a"_s, u"gen-123"_s));
    QVERIFY(acc.ok);
    QVERIFY(acc.durable);                                          // provider-accepted marker: synchronous=FULL
    ga = jobs::get(r, u"job_a"_s);
    QCOMPARE(ga->providerJobId, u"gen-123"_s);
    QCOMPARE(ga->state, u"polling"_s);
    QCOMPARE(ga->finishedMs, 0);
    QVERIFY(!run(jobs::markAccepted(u"job_none"_s, u"gen-1"_s)).ok);
    QVERIFY(!run(jobs::markAccepted(u"job_b"_s, QString())).ok);
    QVERIFY(run(jobs::setState(u"job_a"_s, u"done"_s, 3000)).ok);
    QCOMPARE(jobs::get(r, u"job_a"_s)->finishedMs, 3000);
    QVERIFY(run(jobs::setState(u"job_b"_s, u"failed"_s, 3100, u"boom"_s)).ok);
    QCOMPARE(jobs::get(r, u"job_b"_s)->error, u"boom"_s);
    QCOMPARE(jobs::list(r, u"run_a"_s, u"done"_s).size(), 1);
    QCOMPARE(jobs::count(r, QString(), u"pending"_s), 1);
    QVERIFY(jobs::isTerminal(u"unknown") && !jobs::isTerminal(u"polling"));

    // spend ledger
    const qint64 t0 = 1793016000000LL;
    const WriteResult s1 = run(spend::append(spendRow(u"spd_1"_s, u"reserve"_s, money::cents(50), t0)));
    QVERIFY(s1.ok);
    QVERIFY(s1.durable);
    QCOMPARE(s1.value.toString(), u"spd_1"_s);
    QVERIFY(run(spend::append(spendRow(u"spd_2"_s, u"charge"_s, money::cents(42), t0 + 10))).ok);
    SpendRow auto3 = spendRow(QString(), u"release"_s, money::cents(8), t0 + 20);
    const WriteResult s3 = run(spend::append(auto3));
    QVERIFY(s3.ok);
    QVERIFY(s3.value.toString().startsWith(u"spd_"));
    QVERIFY(!run(spend::append(spendRow(u"spd_4"_s, u"charge"_s, 1, 0))).ok);   // no timestamp
    SpendRow badSource = spendRow(u"spd_5"_s, u"charge"_s, 1);
    badSource.source = QStringLiteral("guess");
    QVERIFY(!run(spend::append(badSource)).ok);
    bool ok = false;
    QCOMPARE(spend::sum(r, filter(u"run_a"_s), &ok), 1000000);
    QVERIFY(ok);
    QCOMPARE(spend::sum(r, filter({}, u"charge"_s)), 420000);
    QCOMPARE(spend::sum(r, filter({}, {}, t0 + 10, t0 + 20)), 420000);
    QCOMPARE(spend::sum(r, filter({}, {}, 0, 0, u"prj_none"_s)), 0);
    QCOMPARE(spend::list(r, filter(u"run_a"_s)).size(), 3);
    QCOMPARE(spend::list(r, {}).first().id, u"spd_1"_s);

    // events: seq comes back as the write's value
    QList<qint64> seqs;
    for (int i = 0; i < 3; ++i) {
        const WriteResult e = run(events::append(u"job"_s, i == 2 ? u"prj_b"_s : u"prj_a"_s, QJsonObject{{u"i"_s, i}}, 5000 + i));
        QVERIFY(e.ok);
        seqs << e.value.toLongLong();
    }
    QVERIFY(seqs[0] > 0 && seqs[1] == seqs[0] + 1 && seqs[2] == seqs[1] + 1);
    QCOMPARE(events::since(r, 0).size(), 3);
    QCOMPARE(events::since(r, seqs[0]).size(), 2);
    QCOMPARE(events::since(r, 0, 500, u"prj_b"_s).size(), 1);
    QCOMPARE(events::since(r, 0, 500, u"prj_b"_s).first().payload.value(u"i").toInt(), 2);
    QCOMPARE(events::lastSeq(r), seqs[2]);
    QVERIFY(!run(events::append(QString(), {}, {}, 1)).ok);

    // audit
    QVERIFY(run(audit::append(u"operator"_s, u"budget.set"_s, u"prj_a"_s, QJsonObject{{u"usd"_s, 25}}, 6000)).ok);
    QCOMPARE(scalar(r, QStringLiteral("SELECT count(*) FROM audit WHERE action = 'budget.set'")), 1);

    // combine: one savepoint, durable when a part is
    const Write both = combine({jobs::insert({job(u"job_f"_s)}), spend::append(spendRow(u"spd_6"_s, u"charge"_s, 7))});
    QVERIFY(both.durability == Durability::Durable);
    QCOMPARE(both.tag, u"jobs.insert+spend.append"_s);
    QVERIFY(run(both).durable);
    QVERIFY(!run(combine({jobs::insert({job(u"job_g"_s)}), spend::append(spendRow(u"spd_6"_s, u"charge"_s, 7))})).ok);
    QVERIFY(!jobs::get(r, u"job_g"_s));                            // the duplicate spend id took job_g down with it
}

void TestStudioStore::batchedInserts10k() {
    QTemporaryDir dir;
    Db db(opts(dir.filePath(QStringLiteral("studio.db"))));    // default 50 ms window, 5,000 per batch
    QString err;
    QVERIFY2(db.open(&err), qPrintable(err));
    QObject ctx;
    int done = 0, failed = 0, outOfOrder = 0, last = -1;
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < 10000; ++i) {
        JobRow j = job(QStringLiteral("job_%1").arg(i, 5, 10, QChar(u'0')), u"run_big"_s, {u"job_root"_s});
        j.priority = i;
        QVERIFY(db.writer().submit(jobs::insert({j}), &ctx, [&, i](const WriteResult& r) {
            if (r.ok) ++done; else ++failed;
            if (i <= last) ++outOfOrder;
            last = i;
        }));
    }
    const qint64 submitMs = t.elapsed();
    // Readers keep working while the writer commits (WAL), and only ever see whole batches.
    qint64 reads = 0, prev = 0, regress = 0;
    while (done + failed < 10000 && t.elapsed() < 30000) {
        const qint64 n = jobs::count(db.reader(), u"run_big"_s);
        if (n < prev || n < 0) ++regress;
        prev = qMax(prev, n);
        ++reads;
        QTest::qWait(5);
    }
    const qint64 totalMs = t.elapsed();
    QCOMPARE(failed, 0);
    QCOMPARE(done, 10000);
    QCOMPARE(outOfOrder, 0);
    QCOMPARE(regress, 0);
    QCOMPARE(jobs::count(db.reader(), u"run_big"_s), 10000);
    QCOMPARE(scalar(db.reader(), QStringLiteral("SELECT count(*) FROM job_edges WHERE from_id = 'job_root'")), 10000);
    const DbWriter::Stats st = db.writer().stats();
    QCOMPARE(st.committed, 10000u);
    QVERIFY2(st.batches >= 2 && st.batches <= 200, qPrintable(QString::number(st.batches)));   // batched, not one each
    QCOMPARE(st.durableBatches, 0u);
    QVERIFY2(totalMs < 20000, qPrintable(QString::number(totalMs)));
    qInfo("10k single-job writes: submitted in %lld ms, committed in %lld ms, %llu batches (max %lld ms), %lld reads",
          submitMs, totalMs, st.batches, st.maxBatchMs, reads);

    // The same 10k rows as one write: one savepoint, one batch.
    QList<JobRow> bulk;
    for (int i = 0; i < 10000; ++i) bulk << job(QStringLiteral("bulk_%1").arg(i), u"run_bulk"_s, {u"job_root"_s});
    t.restart();
    bool bulkOk = false, bulkDone = false;
    QVERIFY(db.writer().submit(jobs::insert(bulk), &ctx, [&](const WriteResult& r) { bulkOk = r.ok; bulkDone = true; }));
    QVERIFY(QTest::qWaitFor([&] { return bulkDone; }, 20000));
    QVERIFY(bulkOk);
    QCOMPARE(jobs::count(db.reader(), u"run_bulk"_s), 10000);
    qInfo("10k rows in one write: %lld ms", t.elapsed());
}

void TestStudioStore::writeIsolation() {
    QTemporaryDir dir;
    Db db(opts(dir.filePath(QStringLiteral("studio.db")), 1000));
    QString err;
    QVERIFY2(db.open(&err), qPrintable(err));
    QObject ctx;
    QMap<QString, WriteResult> got;
    auto as = [&](const QString& name) { return [&got, name](const WriteResult& r) { got[name] = r; }; };
    DbWriter& w = db.writer();
    QVERIFY(w.submit(jobs::insert({job(u"good1"_s)}), &ctx, as(u"good1"_s)));
    QVERIFY(w.submit(jobs::insert({job(u"good1"_s)}), &ctx, as(u"dupe"_s)));          // PRIMARY KEY
    QVERIFY(w.submit(Write{[](Tx& tx) {
                               tx.exec(QStringLiteral("INSERT INTO audit (ts_ms, action) VALUES (1, 'before-throw')"));
                               throw std::runtime_error("boom");
                           }},
                     &ctx, as(u"throws"_s)));
    QVERIFY(w.submit(Write{[](Tx& tx) { tx.exec(QStringLiteral("COMMIT")); }}, &ctx, as(u"commit"_s)));
    QVERIFY(w.submit(Write{[](Tx& tx) { tx.exec(QStringLiteral("pragma synchronous = OFF")); }}, &ctx, as(u"pragma"_s)));
    QVERIFY(w.submit(Write{[](Tx& tx) {
                               tx.exec(QStringLiteral("INSERT INTO audit (ts_ms, action) VALUES (2, 'before-fail')"));
                               tx.fail(QStringLiteral("changed my mind"));
                               tx.exec(QStringLiteral("INSERT INTO audit (ts_ms, action) VALUES (3, 'after-fail')"));
                           }},
                     &ctx, as(u"fail"_s)));
    QVERIFY(w.submit(Write{[](Tx& tx) { tx.exec(QStringLiteral("INSERT INTO nowhere VALUES (1)")); }}, &ctx, as(u"badsql"_s)));
    // read-modify-write inside one write
    QVERIFY(w.submit(Write{[](Tx& tx) {
                               QSqlQuery q = tx.select(QStringLiteral("SELECT count(*) FROM jobs WHERE id = ?"), {u"good1"_s});
                               const qint64 n = q.next() ? q.value(0).toLongLong() : -1;
                               q.finish();
                               tx.setValue(n);
                               JobRow j = job(u"good2"_s);
                               j.priority = int(n);
                               jobs::insert({j}).fn(tx);
                           }},
                     &ctx, as(u"good2"_s)));
    QVERIFY(!w.submit(Write{}, &ctx, as(u"empty"_s)));                               // no function: refused
    QVERIFY(w.flush());
    QVERIFY(QTest::qWaitFor([&] { return got.size() == 8; }, 5000));
    QVERIFY(got[u"good1"_s].ok);
    QVERIFY2(!got[u"dupe"_s].ok && got[u"dupe"_s].error.contains(u"UNIQUE"), qPrintable(got[u"dupe"_s].error));
    QVERIFY2(!got[u"throws"_s].ok && got[u"throws"_s].error.contains(u"boom"), qPrintable(got[u"throws"_s].error));
    QVERIFY2(!got[u"commit"_s].ok && got[u"commit"_s].error.contains(u"not allowed"), qPrintable(got[u"commit"_s].error));
    QVERIFY(!got[u"pragma"_s].ok);
    QCOMPARE(got[u"fail"_s].error, u"changed my mind"_s);
    QVERIFY(!got[u"badsql"_s].ok);
    QVERIFY2(got[u"good2"_s].ok, qPrintable(got[u"good2"_s].error));
    QCOMPARE(got[u"good2"_s].value.toLongLong(), 1);               // saw good1 inside the same transaction
    const quint64 batch = got[u"good1"_s].batch;
    for (const WriteResult& r : got) QCOMPARE(r.batch, batch);     // one transaction, failures isolated by savepoints
    QVERIFY(!got[u"good1"_s].durable);
    const QSqlDatabase r = db.reader();
    QCOMPARE(scalar(r, QStringLiteral("SELECT count(*) FROM jobs")), 2);
    QCOMPARE(scalar(r, QStringLiteral("SELECT count(*) FROM audit")), 0);   // partial work of failed writes is gone
    QCOMPARE(text(r, QStringLiteral("PRAGMA journal_mode")), u"wal"_s);
    const DbWriter::Stats st = db.writer().stats();
    QCOMPARE(st.committed, 2u);
    QCOMPARE(st.failed, 6u);
    QCOMPARE(st.failedBatches, 0u);
    QCOMPARE(st.refused, 1u);
}

void TestStudioStore::txGuards() {
    QTemporaryDir dir;
    Db db(opts(dir.filePath(QStringLiteral("studio.db")), 1000));
    QString err;
    QVERIFY2(db.open(&err), qPrintable(err));
    QObject ctx;
    QMap<QString, WriteResult> got;
    auto as = [&](const QString& name) { return [&got, name](const WriteResult& r) { got[name] = r; }; };
    auto exec = [](const QString& sql, QVariantList binds) {
        return Write{[sql, binds](Tx& tx) { tx.exec(sql, binds); }, Durability::Batched, QStringLiteral("exec")};
    };
    DbWriter& w = db.writer();
    // One cached statement: fewer binds than placeholders is an error, never the previous execution's values.
    const QString ins = QStringLiteral("INSERT INTO audit (ts_ms, action, target) VALUES (?, ?, ?)");
    QVERIFY(w.submit(exec(ins, {1, u"full"_s, u"t1"_s}), &ctx, as(u"full"_s)));
    QVERIFY(w.submit(exec(ins, {2, u"short"_s}), &ctx, as(u"short"_s)));
    QVERIFY(w.submit(exec(ins, {3}), &ctx, as(u"shorter"_s)));
    QVERIFY(w.submit(exec(ins, {4, u"again"_s, u"t4"_s}), &ctx, as(u"again"_s)));          // still works afterwards
    QVERIFY(w.submit(exec(ins, {5, u"long"_s, u"t5"_s, u"extra"_s}), &ctx, as(u"long"_s)));
    // Transaction control and pragmas stay refused behind leading comments (SQLite skips them).
    QVERIFY(w.submit(exec(QStringLiteral("/**/COMMIT"), {}), &ctx, as(u"c1"_s)));
    QVERIFY(w.submit(exec(QStringLiteral("-- x\nCOMMIT"), {}), &ctx, as(u"c2"_s)));
    QVERIFY(w.submit(exec(QStringLiteral("  /* a */ -- b\n /* c */ end"), {}), &ctx, as(u"c3"_s)));
    QVERIFY(w.submit(exec(QStringLiteral("/* x */ PRAGMA synchronous = OFF"), {}), &ctx, as(u"c4"_s)));
    QVERIFY(w.submit(Write{[](Tx& tx) { tx.select(QStringLiteral("-- sneaky\nROLLBACK")); }}, &ctx, as(u"c5"_s)));
    // A commented INSERT is still an INSERT (it reports its rowid).
    QVERIFY(w.submit(exec(QStringLiteral("/* note */ INSERT INTO audit (ts_ms, action) VALUES (6, 'commented')"), {}), &ctx,
                     as(u"commented"_s)));
    QVERIFY(w.flush());
    QVERIFY(QTest::qWaitFor([&] { return got.size() == 11; }, 5000));
    QVERIFY2(got[u"full"_s].ok, qPrintable(got[u"full"_s].error));
    for (const char* name : {"short", "shorter"}) {
        const WriteResult& r = got[QString::fromLatin1(name)];
        QVERIFY2(!r.ok && r.error.contains(u"mismatch", Qt::CaseInsensitive),
                 qPrintable(QString::fromLatin1(name) + u": "_s + r.error));
    }
    QVERIFY2(got[u"again"_s].ok, qPrintable(got[u"again"_s].error));
    QVERIFY2(!got[u"long"_s].ok, "more binds than placeholders must fail too");
    for (const char* name : {"c1", "c2", "c3", "c4", "c5"}) {
        const WriteResult& r = got[QString::fromLatin1(name)];
        QVERIFY2(!r.ok && r.error.contains(u"not allowed"), qPrintable(QString::fromLatin1(name) + u": "_s + r.error));
    }
    QVERIFY2(got[u"commented"_s].ok, qPrintable(got[u"commented"_s].error));
    QVERIFY(got[u"commented"_s].lastInsertId > 0);
    for (const WriteResult& r : got) QCOMPARE(r.batch, got[u"full"_s].batch);   // one batch, every failure isolated
    const QSqlDatabase r = db.reader();
    QCOMPARE(scalar(r, QStringLiteral("SELECT count(*) FROM audit")), 3);
    QCOMPARE(scalar(r, QStringLiteral("SELECT count(*) FROM audit WHERE ts_ms IN (2, 3, 5)")), 0);
    QCOMPARE(text(r, QStringLiteral("SELECT target FROM audit WHERE ts_ms = 4")), u"t4"_s);
    QCOMPARE(text(r, QStringLiteral("PRAGMA journal_mode")), u"wal"_s);
    QCOMPARE(db.writer().stats().failedBatches, 0u);
}

void TestStudioStore::commitBeforeSend() {
    QTemporaryDir dir;
    // A long window: only a durable write can explain a fast commit.
    Db db(opts(dir.filePath(QStringLiteral("studio.db")), 3000));
    QString err;
    QVERIFY2(db.open(&err), qPrintable(err));
    DbWriter& w = db.writer();
    QObject ctx;
    struct Ev { QString what; bool visible; bool main; quint64 batch; bool durable; };
    QList<Ev> evs;
    auto has = [&](const QString& table, const QString& id) {
        return scalar(db.reader(), QStringLiteral("SELECT count(*) FROM %1 WHERE id = ?").arg(table), {id}) == 1;
    };
    QElapsedTimer t;
    t.start();
    // A (batched job) then B (money row, commit then send): B pulls A into its durable commit.
    QVERIFY(w.submit(jobs::insert({job(u"job_a"_s)}), &ctx, [&](const WriteResult& r) {
        evs << Ev{u"A"_s, has(u"jobs"_s, u"job_a"_s), onMainThread(), r.batch, r.durable};
    }));
    qint64 sentAfterMs = -1;
    QVERIFY(w.commitThenSend(spend::append(spendRow(u"spd_b"_s, u"reserve"_s, money::cents(30))), &ctx,
                             [&](const WriteResult& r) {
                                 sentAfterMs = t.elapsed();
                                 // the "send": the money row and everything before it are already committed
                                 evs << Ev{u"send-B"_s, has(u"spend"_s, u"spd_b"_s) && has(u"jobs"_s, u"job_a"_s),
                                           onMainThread(), r.batch, r.durable};
                             },
                             [&](const WriteResult&) { evs << Ev{u"fail-B"_s, false, onMainThread(), 0, false}; }));
    QVERIFY(QTest::qWaitFor([&] { return evs.size() == 2; }, 2000));
    QVERIFY2(sentAfterMs >= 0 && sentAfterMs < 2000, qPrintable(QString::number(sentAfterMs)));
    QCOMPARE(evs[0].what, u"A"_s);
    QCOMPARE(evs[1].what, u"send-B"_s);
    for (const Ev& e : evs) {
        QVERIFY2(e.visible, qPrintable(e.what));
        QVERIFY(e.main);
        QVERIFY(e.durable);                // A rode in B's synchronous=FULL batch
        QCOMPARE(e.batch, evs[0].batch);
    }
    QCOMPARE(w.stats().durableBatches, 1u);

    // A batched write alone waits for the window; flush commits it now.
    bool cDone = false;
    QVERIFY(w.submit(events::append(u"job"_s, {}, {}, 1), &ctx, [&](const WriteResult& r) { cDone = r.ok; }));
    QTest::qWait(300);
    QVERIFY(!cDone);
    QCOMPARE(events::lastSeq(db.reader()), 0);
    QVERIFY(w.flush());
    QVERIFY(QTest::qWaitFor([&] { return cDone; }, 2000));
    QCOMPARE(w.stats().durableBatches, 1u);

    // A durable write that fails never sends; a second one in the same batch still does.
    int sends = 0, fails = 0;
    QString failError;
    bool dVisible = false;
    QVERIFY(w.commitThenSend(spend::append(spendRow(u"spd_b"_s, u"charge"_s, 1)), &ctx,
                             [&](const WriteResult&) { ++sends; },
                             [&](const WriteResult& r) { ++fails; failError = r.error; }));
    QVERIFY(w.commitThenSend(spend::append(spendRow(u"spd_d"_s, u"charge"_s, 2)), &ctx,
                             [&](const WriteResult&) { ++sends; dVisible = has(u"spend"_s, u"spd_d"_s); }));
    QVERIFY(QTest::qWaitFor([&] { return sends + fails == 2; }, 2000));
    QCOMPARE(fails, 1);
    QCOMPARE(sends, 1);
    QVERIFY(dVisible);
    QVERIFY2(failError.contains(u"UNIQUE"), qPrintable(failError));
    QCOMPARE(scalar(db.reader(), QStringLiteral("SELECT count(*) FROM spend")), 2);
    QCOMPARE(scalar(db.reader(), QStringLiteral("SELECT micro FROM spend WHERE id = 'spd_b'")), money::cents(30));

    // commitThenSend needs a context and a send function.
    QVERIFY(!w.commitThenSend(noop(), &ctx, {}));
}

void TestStudioStore::callbackThreads() {
    QTemporaryDir dir;
    Db db(opts(dir.filePath(QStringLiteral("studio.db")), 5));
    QString err;
    QVERIFY2(db.open(&err), qPrintable(err));
    DbWriter& w = db.writer();

    // A context in a worker thread gets its callback there.
    QThread worker;
    worker.start();
    auto* obj = new QObject;
    obj->moveToThread(&worker);
    std::atomic<QThread*> seen{nullptr};
    QVERIFY(w.submit(noop(), obj, [&](const WriteResult&) { seen = QThread::currentThread(); }));
    QVERIFY(QTest::qWaitFor([&] { return seen.load() != nullptr; }, 3000));
    QCOMPARE(seen.load(), &worker);
    obj->deleteLater();
    worker.quit();
    QVERIFY(worker.wait(5000));

    // No context: the writer thread itself.
    std::atomic<QThread*> writerThread{nullptr};
    QVERIFY(w.submit(noop(), nullptr, [&](const WriteResult&) { writerThread = QThread::currentThread(); }));
    QVERIFY(QTest::qWaitFor([&] { return writerThread.load() != nullptr; }, 3000));
    QVERIFY(writerThread.load() != QThread::currentThread());

    // A context destroyed before delivery: the write commits, the callback is skipped.
    auto* gone = new QObject;
    bool called = false;
    QVERIFY(w.submit(jobs::insert({job(u"job_gone"_s)}), gone, [&](const WriteResult&) { called = true; }));
    delete gone;
    QVERIFY(w.flush());
    QTest::qWait(100);
    QVERIFY(!called);
    QVERIFY(jobs::get(db.reader(), u"job_gone"_s));
}

void TestStudioStore::batchWindowAndFlush() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("studio.db"));
    QString err;
    {
        DbOptions o = opts(path, 1200);
        o.maxQueue = 50;
        Db db(o);
        QVERIFY2(db.open(&err), qPrintable(err));
        DbWriter& w = db.writer();
        QObject ctx;
        // The window: a lone batched write commits about batchWindowMs after it was queued.
        QElapsedTimer t;
        t.start();
        qint64 doneAt = -1;
        QVERIFY(w.submit(jobs::insert({job(u"job_w"_s)}), &ctx, [&](const WriteResult&) { doneAt = t.elapsed(); }));
        QTest::qWait(400);
        QVERIFY(!jobs::get(db.reader(), u"job_w"_s));
        QCOMPARE(w.stats().batches, 0u);
        QVERIFY(QTest::qWaitFor([&] { return doneAt >= 0; }, 5000));
        QVERIFY2(doneAt >= 1000 && doneAt < 4000, qPrintable(QString::number(doneAt)));
        QCOMPARE(w.stats().batches, 1u);

        // A full queue refuses (and says so) instead of growing without bound.
        int refusedAt = -1;
        for (int i = 0; i < 60; ++i)
            if (!w.submit(noop())) { refusedAt = i; break; }
        QCOMPARE(refusedAt, 50);
        QCOMPARE(w.stats().refused, 1u);
        QCOMPARE(w.stats().queued, 50);
        QVERIFY(w.flush());
        QCOMPARE(w.stats().queued, 0);

        // Close drains what is still queued.
        for (int i = 0; i < 20; ++i) QVERIFY(w.submit(jobs::insert({job(QStringLiteral("job_close_%1").arg(i))})));
        db.close();
        QVERIFY(!db.isOpen());
    }
    {
        Db db(opts(path));
        QVERIFY2(db.open(&err), qPrintable(err));
        QCOMPARE(scalar(db.reader(), QStringLiteral("SELECT count(*) FROM jobs WHERE id LIKE 'job_close_%'")), 20);
    }
    // Close checkpointed the WAL: nothing is left in it.
    QVERIFY(QFileInfo(path + QStringLiteral("-wal")).size() == 0 || !QFileInfo::exists(path + QStringLiteral("-wal")));

    // A stopped writer refuses; flush on it fails fast.
    DbWriter w(wopts(path));
    QVERIFY2(w.start(&err), qPrintable(err));
    QVERIFY(!w.start(&err));                // already started
    QVERIFY(w.running());
    w.stop();
    QVERIFY(!w.running());
    QVERIFY(!w.submit(noop()));
    QVERIFY(!w.flush(100));
}

void TestStudioStore::lockedDatabase() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("studio.db"));
    DbOptions o = opts(path, 10);
    o.busyTimeoutMs = 200;
    Db db(o);
    QString err;
    QVERIFY2(db.open(&err), qPrintable(err));
    QObject ctx;
    // Another connection (a sqlite3 shell, a second process) holds the write lock past the busy timeout.
    ScopedConnection blocker(path, ScopedConnection::Mode::ReadWrite, 0);
    QVERIFY(blocker.isOpen());
    {
        QSqlQuery b(blocker.db());
        QVERIFY(b.exec(QStringLiteral("BEGIN IMMEDIATE")));
    }
    int sent = 0, failed = 0;
    QString why;
    QVERIFY(db.writer().commitThenSend(spend::append(spendRow(u"spd_l"_s, u"reserve"_s, 5)), &ctx,
                                       [&](const WriteResult&) { ++sent; },
                                       [&](const WriteResult& r) { ++failed; why = r.error; }));
    QVERIFY(QTest::qWaitFor([&] { return sent + failed > 0; }, 5000));
    QCOMPARE(sent, 0);                     // fail closed: the paid request is never sent
    QCOMPARE(failed, 1);
    QVERIFY2(why.contains(u"begin"), qPrintable(why));
    QCOMPARE(db.writer().stats().failedBatches, 1u);
    // Readers are not blocked by a writer in WAL mode.
    QCOMPARE(scalar(db.reader(), QStringLiteral("SELECT count(*) FROM spend")), 0);
    {
        QSqlQuery b(blocker.db());
        QVERIFY(b.exec(QStringLiteral("ROLLBACK")));
    }
    QVERIFY(db.writer().commitThenSend(spend::append(spendRow(u"spd_l"_s, u"reserve"_s, 5)), &ctx,
                                       [&](const WriteResult&) { ++sent; },
                                       [&](const WriteResult&) { ++failed; }));
    QVERIFY(QTest::qWaitFor([&] { return sent == 1; }, 5000));
    QCOMPARE(failed, 1);
    QCOMPARE(scalar(db.reader(), QStringLiteral("SELECT count(*) FROM spend")), 1);
}

void TestStudioStore::stuckWriterStop() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("studio.db"));
    QString err;
    std::atomic<bool> started{false}, release{false};
    struct Release {                       // a failed check must not leave a writer spinning
        std::atomic<bool>& r;
        ~Release() { r = true; }
    } guard{release};
    auto slow = [&](const QString& action) {
        return Write{[&started, &release, action](Tx& tx) {
                         started = true;
                         QElapsedTimer t;
                         t.start();
                         while (!release.load() && t.elapsed() < 15000) QThread::msleep(2);
                         tx.exec(QStringLiteral("INSERT INTO audit (ts_ms, action) VALUES (1, ?)"), {action});
                     },
                     Durability::Batched, QStringLiteral("slow")};
    };
    // Db::close gives up on a writer stuck in a write after stopTimeoutMs and leaks it instead of freeing it under its
    // thread: the writer finishes the write, commits and stops by itself. This test reclaims it to stay leak-free.
    {
        DbOptions o = opts(path, 5);
        o.stopTimeoutMs = 100;
        Db db(o);
        QVERIFY2(db.open(&err), qPrintable(err));
        std::atomic<bool> done{false};
        QVERIFY(db.writer().submit(slow(u"slow-db"_s), nullptr, [&done](const WriteResult& r) { done = r.ok; }));
        QVERIFY(QTest::qWaitFor([&] { return started.load(); }, 3000));
        DbWriter* stuck = &db.writer();
        QElapsedTimer t;
        t.start();
        db.close();
        QVERIFY2(t.elapsed() < 5000, qPrintable(QString::number(t.elapsed())));
        QVERIFY(!db.isOpen());
        QVERIFY(!done.load());
        QVERIFY(!stuck->running());        // still a live object: leaked, not freed
        QVERIFY(!stuck->submit(noop()));
        release = true;
        QVERIFY(stuck->stop(-1));
        delete stuck;
        QVERIFY(done.load());              // its no-context callback ran on the writer thread before it stopped
    }
    // A DbWriter owned directly: stop() past its deadline answers false and keeps the object intact; the destructor
    // waits for the writer.
    started = false;
    release = false;
    {
        DbWriter w(wopts(path));
        QVERIFY2(w.start(&err), qPrintable(err));
        QVERIFY(w.submit(slow(u"slow-direct"_s)));
        QVERIFY(QTest::qWaitFor([&] { return started.load(); }, 3000));
        QVERIFY(!w.stop(50));
        QVERIFY(!w.running());
        QVERIFY(!w.submit(noop()));
        QVERIFY(!w.stop(0));               // still busy
        release = true;
    }                                      // ~DbWriter joins it
    ScopedConnection c(path, ScopedConnection::Mode::ReadOnly);
    QVERIFY(c.isOpen());
    QCOMPARE(scalar(c.db(), QStringLiteral("SELECT count(*) FROM audit WHERE action LIKE 'slow-%'")), 2);
}

void TestStudioStore::crashMidBatch() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("crash.db"));
    QProcess p;
    p.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    p.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--writer-child"), path, QStringLiteral("die")});
    QVERIFY(p.waitForStarted(10000));
    ChildOut out;
    const bool finished = readChildUntil(p, out, [&] { return false; }, 40000) || p.state() == QProcess::NotRunning;
    if (p.state() != QProcess::NotRunning) {
        p.kill();
        p.waitForFinished(5000);
    }
    QVERIFY2(finished, "the child did not die by itself");
    QVERIFY2(out.problems.isEmpty(), qPrintable(out.problems.join(u" | ")));
    QCOMPARE(out.ready, 1);
    QVERIFY2(out.dying > 0, "no DYING line");
    QCOMPARE(p.exitCode(), 9);

    const CrashState s = inspectCrash(path);
    qInfo("die mid-batch: last executed %lld, last acked %lld; %s", out.dying, out.lastAck, qPrintable(describe(s)));
    verifyConsistent(s);
    if (QTest::currentTestFailed()) return;
    // The 51 writes the dying batch had executed are all gone (uncommitted), everything acknowledged is there.
    QVERIFY2(s.maxN <= out.dying - 51, qPrintable(describe(s)));
    QVERIFY2(s.maxN >= out.lastAck, qPrintable(describe(s)));

    // The database is fully usable afterwards.
    Db db(opts(path, 50, {crashModule()}));
    QString err;
    QVERIFY2(db.open(&err), qPrintable(err));
    QVERIFY(db.migrations().applied.isEmpty());
    bool ok = false;
    QObject ctx;
    QVERIFY(db.writer().submit(crashWrite(s.maxN + 1), &ctx, [&](const WriteResult& r) { ok = r.ok; }));
    QVERIFY(db.writer().flush());
    QVERIFY(QTest::qWaitFor([&] { return ok; }, 3000));
}

void TestStudioStore::crashKillRandom() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("crash.db"));
    qint64 expectNext = 1;
    for (int round = 0; round < 3; ++round) {
        QProcess p;
        p.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        p.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--writer-child"), path, QStringLiteral("flood")});
        QVERIFY(p.waitForStarted(10000));
        ChildOut out;
        const bool ready = readChildUntil(p, out, [&] { return out.ready > 0; }, 20000);
        QVERIFY2(ready, qPrintable(out.problems.join(u" | ")));
        QCOMPARE(out.ready, expectNext);   // resumed right after the last committed write
        const qint64 target = out.ready + 2000 + round * 1500;
        const bool acked = readChildUntil(p, out, [&] { return out.lastAck >= target; }, 30000);
        QTest::qWait(int(QRandomGenerator::global()->bounded(1, 40)));
        p.kill();                          // TerminateProcess on our own child, mid-flood
        QVERIFY(p.waitForFinished(10000));
        parseChild(out, p.readAllStandardOutput());
        QVERIFY2(acked, qPrintable(QStringLiteral("ack %1 < %2; %3").arg(out.lastAck).arg(target).arg(out.problems.join(u" | "))));
        QVERIFY2(out.problems.isEmpty(), qPrintable(out.problems.join(u" | ")));

        const CrashState s = inspectCrash(path);
        qInfo("kill round %d: last acked %lld; %s", round, out.lastAck, qPrintable(describe(s)));
        verifyConsistent(s);
        if (QTest::currentTestFailed()) return;
        QVERIFY2(s.maxN >= out.lastAck, qPrintable(describe(s)));   // nothing acknowledged was lost
        expectNext = s.maxN + 1;
    }
}

int main(int argc, char* argv[]) {
    for (int i = 1; i + 2 < argc; ++i)
        if (qstrcmp(argv[i], "--writer-child") == 0)
            return writerChild(argc, argv, QString::fromLocal8Bit(argv[i + 1]), QString::fromLatin1(argv[i + 2]));
    QCoreApplication app(argc, argv);
    TestStudioStore tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "test_studio_store.moc"
