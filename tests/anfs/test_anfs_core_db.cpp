// test_anfs_core_db - ANFS-F3: the vendored SQLite 3.54.0 and core/db (serialized writer, read-only WAL reader pool,
// statement cache, ModSchema, openCopyReadOnly).
//
// - Library: version and source id, compile options (THREADSAFE=1, ENABLE_FTS5, DQS=0, ...), DQS off, JSON built in.
// - Connections: WAL / synchronous NORMAL / foreign_keys OFF / busy_timeout / mmap_size on the writer and the readers,
//   read-only readers, :memory: and readers=0 using the writer, folder creation, extended result codes.
// - Values: every type both ways, '' vs NULL, empty blob vs NULL, int64 edges, rows in SELECT order, repeated column
//   names as a JS object, the better-sqlite3 change count, scripts, parameter checks, error texts and codes.
// - Statement cache: reuse, a twin for nested iteration of the same SQL, the 64 cap, nothing left stepping after a
//   callback (no stale reader snapshot, checkpoints not pinned), a prepare()d Stmt& never evicted during its callback
//   (pins end with the outermost callback).
// - Transactions: commit, rollback on throw, nested tx joins, tx inside write, shortcuts and read() inside tx, other
//   threads serialized, busy_timeout against a second connection, a hand-made BEGIN left open rolled back at the end
//   of the callback (writer and reader) and never joined by tx().
// - Reader pool: a 2,000-row writer tx stays under 250 ms while 4 readers and a 400 ms open read run; readers see
//   whole transactions only; at most 4 readers at once; a nested read() reuses the thread's reader.
// - ModSchema: the C# FsCore_ModSchema cases with exact texts, _mod_schema DDL byte-identical, blank steps, concurrent
//   appliers run each step once.
// - FTS5 (unicode61, remove_diacritics, prefix, highlight) and JSON1 on the reader pool.
// - openCopyReadOnly: a fresh -wal refused (60 s on an injected clock), an idle copy opens read-only and stays
//   byte-identical and reopens at once; a cold copy (no -wal) opens twice in a row with no -wal/-shm left behind,
//   also under a path with '#', '%' and spaces.
// - The C# oracle (D:\anfs-oracle\anfs.exe, loaded into Windows PowerShell 5.1; skipped when absent):
//   the anfs.db its Glue_Db selftest group creates opens here with the same versions and DDL; versions written here
//   are read and honoured by the C# ModSchema; both binaries use the file at the same time through the WAL.
#include "core/clock.h"
#include "core/db.h"
#include "core/fsutil.h"
#include "core/json.h"

#include <sqlite3.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtTest/QtTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace anfs;

namespace {

QByteArray qb(const std::string& s) { return QByteArray(s.data(), static_cast<qsizetype>(s.size())); }
#define SEQ(actual, expected) QCOMPARE(qb(actual), qb(std::string(expected)))
QString qs(const std::string& s) { return QString::fromStdString(s); }
std::string ss(const QString& s) { return QDir::toNativeSeparators(s).toStdString(); }

struct Caught {
    bool thrown = false;
    int code = 0;
    int ext = 0;
    std::string what;
};

template <typename F>
Caught catchDb(F&& f)
{
    Caught c;
    try {
        f();
    } catch (const DbError& e) {
        c.thrown = true;
        c.code = e.code;
        c.ext = e.extendedCode;
        c.what = e.what();
    }
    return c;
}

template <typename F>
std::string catchStd(F&& f)
{
    try {
        f();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "<no throw>";
}

int64_t count(Db& db, std::string_view sql, const DbParams& p = {}) { return db.scalar(sql, p).asInt(-1); }

// DbConn::queryOne on the writer (Db has no queryOne shortcut).
JVal first(Db& db, std::string_view sql, const DbParams& p = {})
{
    JVal r;
    db.write([&](DbConn& c) { r = c.queryOne(sql, p); });
    return r;
}

int busyStatements(void* h)
{
    int n = 0;
    for (sqlite3_stmt* s = sqlite3_next_stmt(static_cast<sqlite3*>(h), nullptr); s;
         s = sqlite3_next_stmt(static_cast<sqlite3*>(h), s))
        if (sqlite3_stmt_busy(s)) ++n;
    return n;
}

int allStatements(void* h)
{
    int n = 0;
    for (sqlite3_stmt* s = sqlite3_next_stmt(static_cast<sqlite3*>(h), nullptr); s;
         s = sqlite3_next_stmt(static_cast<sqlite3*>(h), s))
        ++n;
    return n;
}

QByteArray sha(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex();
}

bool setMtime(const QString& path, const QDateTime& t)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite)) return false;
    return f.setFileTime(t, QFileDevice::FileModificationTime);
}

void updateMax(std::atomic<int>& m, int v)
{
    int cur = m.load();
    while (v > cur && !m.compare_exchange_weak(cur, v)) {}
}

QString findPowerShell51()
{
    const QString p = QStringLiteral("C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe");
    return QFileInfo::exists(p) ? p : QString();
}

QString oraclePath()
{
    const QByteArray env = qgetenv("ANFS_CSHARP_ORACLE");
    const QString p = env.isEmpty() ? QStringLiteral("D:/anfs-oracle/anfs.exe") : QString::fromLocal8Bit(env);
    return QFileInfo::exists(p) ? p : QString();
}

QString psQuote(const QString& s) { return QLatin1Char('\'') + QString(s).replace(QLatin1Char('\''), QStringLiteral("''")) + QLatin1Char('\''); }

QStringList psArgs(const QString& script)
{
    const QByteArray utf16(reinterpret_cast<const char*>(script.utf16()), script.size() * 2);
    return {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-EncodedCommand"),
            QString::fromLatin1(utf16.toBase64())};
}

// "key<TAB>value" lines from the oracle scripts.
std::multimap<QString, QString> parseSay(const QByteArray& out)
{
    std::multimap<QString, QString> m;
    for (const QByteArray& raw : out.split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        const qsizetype tab = line.indexOf(QLatin1Char('\t'));
        if (tab > 0) m.emplace(line.left(tab), line.mid(tab + 1));
    }
    return m;
}

QString one(const std::multimap<QString, QString>& m, const QString& key)
{
    const auto it = m.find(key);
    return it == m.end() ? QStringLiteral("<missing>") : it->second;
}

// Shared prologue of the oracle scripts: load the C# anfs.exe, Say k v, Msg (unwraps PowerShell's method wrapper only).
QString psPrologue(const QString& oracle)
{
    return QStringLiteral(
               "$ErrorActionPreference = 'Stop'\n"
               "$asm = [Reflection.Assembly]::LoadFrom(%1)\n"
               "function Say($k, $v) { [Console]::Out.WriteLine([string]$k + \"`t\" + [string]$v) }\n"
               "function Msg($e) { $x = $e.Exception; if ($x -is [System.Management.Automation.MethodInvocationException] "
               "-and $x.InnerException -ne $null) { $x = $x.InnerException }; return $x.Message }\n"
               "$none = [object[]]@()\n")
        .arg(psQuote(oracle));
}

} // namespace

class TestAnfsCoreDb : public QObject {
    Q_OBJECT
private slots:
    void libraryAndOptions();
    void connectionSetup();
    void valuesAndRows();
    void execQueryScalarScripts();
    void errorsCarryCodes();
    void statementCache();
    void transactions();
    void readersRunBesideTheWriter();
    void readerPoolWaits();
    void modSchemaCsharpCases();
    void fts5AndJson();
    void openCopyReadOnlyGuard();
    void csharpOracleSharesTheFile();
};

void TestAnfsCoreDb::libraryAndOptions()
{
    SEQ(Db::sqliteVersion(), "3.54.0");
    SEQ(std::string(sqlite3_sourceid()), SQLITE_SOURCE_ID);   // the library is the vendored header's build
    QVERIFY(Db::hasFts5());
    auto db = Db::open(":memory:");
    std::set<std::string> opts;
    const JVal optRows = db->query("PRAGMA compile_options");   // (a range-for over query(..).arr() would dangle)
    for (const JVal& r : optRows.arr()) opts.insert(r.getStr("compile_options"));
    std::string all;
    for (const std::string& o : opts) all += o + " ";
    for (const char* want : {"THREADSAFE=1", "ENABLE_FTS5", "DQS=0", "DEFAULT_WAL_SYNCHRONOUS=1", "OMIT_DEPRECATED"})
        QVERIFY2(opts.count(want), qPrintable(QStringLiteral("%1 missing from: %2").arg(want, qs(all))));
    // DQS=0: a double-quoted word is an identifier, never a string.
    const Caught dq = catchDb([&] { db->scalar("SELECT \"abc\""); });
    QVERIFY(dq.thrown);
    const std::string dqText = "no such column: \"abc\" - should this be a string literal in single-quotes?";
    SEQ(dq.what, dqText);
    QCOMPARE(dq.code, SQLITE_ERROR);
    QCOMPARE(db->scalar("SELECT json_extract('{\"a\":[1,2]}', '$.a[1]')").asInt(), 2);
}

void TestAnfsCoreDb::connectionSetup()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::string p = ss(tmp.path() + "/sub/deeper/a.db");   // both folders are created
    auto db = Db::open(p);
    QVERIFY(fs::isFile(p));
    SEQ(db->path(), fs::fullPath(p));
    SEQ(db->scalar("PRAGMA journal_mode").asString(), "wal");
    QCOMPARE(db->scalar("PRAGMA synchronous").asInt(), 1);
    QCOMPARE(db->scalar("PRAGMA foreign_keys").asInt(), 0);
    QCOMPARE(db->scalar("PRAGMA busy_timeout").asInt(), 5000);
    QCOMPARE(db->scalar("PRAGMA mmap_size").asInt(), 256LL << 20);
    db->exec("CREATE TABLE t(a)");
    QCOMPARE(db->exec("INSERT INTO t VALUES(1)"), 1);

    void* wh = nullptr;
    db->write([&](DbConn& c) {
        wh = c.handle();
        QVERIFY(!c.readOnly());
    });
    db->read([&](DbConn& c) {
        QVERIFY(c.readOnly());
        QVERIFY(c.handle() != wh);
        SEQ(c.scalar("PRAGMA journal_mode").asString(), "wal");
        QCOMPARE(c.scalar("PRAGMA busy_timeout").asInt(), 5000);
        QCOMPARE(c.scalar("PRAGMA mmap_size").asInt(), 256LL << 20);
        QCOMPARE(c.scalar("PRAGMA foreign_keys").asInt(), 0);
        QCOMPARE(c.scalar("SELECT count(*) FROM t").asInt(), 1);
        const Caught w = catchDb([&] { c.exec("INSERT INTO t VALUES(2)"); });
        QVERIFY(w.thrown);
        QCOMPARE(w.code, SQLITE_READONLY);
    });
    const QString dbq = qs(db->path());
    QCOMPARE(db->sizeBytes(), QFileInfo(dbq).size() + QFileInfo(dbq + "-wal").size());
    QVERIFY(db->sizeBytes() > 0);

    // :memory: has no readers: read() lends the writer.
    auto m = Db::open(":memory:");
    SEQ(m->path(), ":memory:");
    QCOMPARE(m->sizeBytes(), 0);
    SEQ(m->scalar("PRAGMA journal_mode").asString(), "memory");
    void* mh = nullptr;
    m->write([&](DbConn& c) { mh = c.handle(); });
    m->read([&](DbConn& c) {
        QCOMPARE(c.handle(), mh);
        QVERIFY(!c.readOnly());
    });

    // readers = 0 and other options.
    Db::Options o;
    o.readers = 0;
    o.busyTimeoutMs = 1234;
    o.mmapBytes = 0;
    auto z = Db::open(ss(tmp.path() + "/z.db"), o);
    void* zh = nullptr;
    z->write([&](DbConn& c) { zh = c.handle(); });
    z->read([&](DbConn& c) { QCOMPARE(c.handle(), zh); });
    QCOMPARE(z->scalar("PRAGMA busy_timeout").asInt(), 1234);
    QCOMPARE(z->scalar("PRAGMA mmap_size").asInt(), 0);

    // Extended result codes.
    db->exec("CREATE TABLE u(a UNIQUE, b NOT NULL DEFAULT 0)");
    db->exec("CREATE TABLE pk(id INTEGER PRIMARY KEY)");
    db->exec("INSERT INTO u(a) VALUES(1)");
    db->exec("INSERT INTO pk VALUES(1)");
    const Caught un = catchDb([&] { db->exec("INSERT INTO u(a) VALUES(1)"); });
    QCOMPARE(un.code, SQLITE_CONSTRAINT);
    QCOMPARE(un.ext, SQLITE_CONSTRAINT_UNIQUE);
    SEQ(un.what, "UNIQUE constraint failed: u.a");
    const Caught pk = catchDb([&] { db->exec("INSERT INTO pk VALUES(?)", {1}); });
    QCOMPARE(pk.ext, SQLITE_CONSTRAINT_PRIMARYKEY);
    SEQ(pk.what, "UNIQUE constraint failed: pk.id");
    const Caught nn = catchDb([&] { db->exec("INSERT INTO u(a, b) VALUES(5, NULL)"); });
    QCOMPARE(nn.ext, SQLITE_CONSTRAINT_NOTNULL);
    SEQ(nn.what, "NOT NULL constraint failed: u.b");

    // Open failures: a folder, a missing file read-only (and nothing is created).
    const Caught dir = catchDb([&] { Db::open(ss(tmp.path())); });
    QVERIFY(dir.thrown);
    QCOMPARE(dir.code, SQLITE_CANTOPEN);
    QVERIFY2(dir.what.find("unable to open database file") != std::string::npos, dir.what.c_str());
    Db::Options ro;
    ro.readOnly = true;
    const std::string missing = ss(tmp.path() + "/nofolder/missing.db");
    const Caught rm = catchDb([&] { Db::open(missing, ro); });
    QCOMPARE(rm.code, SQLITE_CANTOPEN);
    QVERIFY(!fs::exists(missing) && !fs::exists(ss(tmp.path() + "/nofolder")));
    QCOMPARE(catchDb([&] { Db::open(""); }).code, SQLITE_CANTOPEN);
}

void TestAnfsCoreDb::valuesAndRows()
{
    auto db = Db::open(":memory:");
    db->exec("CREATE TABLE t(a, b, c, d, e, f, g, h)");
    const std::string uni = "h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C \xF0\x9F\x98\x80";
    std::string blob(1 << 20, '\0');
    std::mt19937 rng(7);
    for (char& ch : blob) ch = static_cast<char>(rng() & 0xff);
    db->write([&](DbConn& c) {
        Stmt& s = c.prepare("INSERT INTO t VALUES(?,?,?,?,?,?,?,?)");
        s.bindNull(1).bind(2, 42).bind(3, int64_t(5000000123)).bind(4, 3.14159265358979).bind(5, uni);
        s.bindBlob(6, blob.data(), blob.size()).bind(7, true).bind(8, "");
        QCOMPARE(s.run(), 1);
        QCOMPARE(c.lastInsertId(), 1);
        QCOMPARE(c.changes(), 1);
    });
    const JVal types = first(*db, "SELECT typeof(a) a, typeof(b) b, typeof(c) c, typeof(d) d, typeof(e) e, "
                                    "typeof(f) f, typeof(g) g, typeof(h) h FROM t");
    SEQ(json::stringify(types),
        "{\"a\":\"null\",\"b\":\"integer\",\"c\":\"integer\",\"d\":\"real\",\"e\":\"text\",\"f\":\"blob\","
        "\"g\":\"integer\",\"h\":\"text\"}");
    const JVal row = first(*db, "SELECT a, b, c, d, e, g, h FROM t");
    const std::string want = "{\"a\":null,\"b\":42,\"c\":5000000123,\"d\":3.14159265358979,\"e\":\"" + uni +
                             "\",\"g\":1,\"h\":\"\"}";
    SEQ(json::stringify(row), want);
    const JVal f = db->scalar("SELECT f FROM t");
    QVERIFY(f.isString());
    QVERIFY(f.asString() == blob);                       // BLOB columns read as string bytes
    QCOMPARE(count(*db, "SELECT length(f) FROM t"), int64_t(1 << 20));

    // '' vs NULL, empty blob vs NULL, embedded NUL kept.
    db->exec("CREATE TABLE v(x)");
    db->write([&](DbConn& c) {
        Stmt& s = c.prepare("INSERT INTO v VALUES(?)");
        s.bind(1, std::string_view());                   // a null data pointer still binds ''
        s.run();
        s.reset();
        s.bindBlob(1, nullptr, 0);
        s.run();
        s.reset();
        s.bind(1, std::string_view("a\0b", 3));
        s.run();
        s.reset();
        s.bind(1, static_cast<const char*>(nullptr));     // binds NULL
        s.run();
    });
    SEQ(json::stringify(db->query("SELECT typeof(x) t, length(CAST(x AS BLOB)) n FROM v ORDER BY rowid")),
        "[{\"t\":\"text\",\"n\":0},{\"t\":\"blob\",\"n\":0},{\"t\":\"text\",\"n\":3},{\"t\":\"null\",\"n\":null}]");
    QCOMPARE(db->scalar("SELECT x FROM v WHERE rowid = 3").asString().size(), size_t(3));

    // JVal parameters: null, bool, int, double, string; arrays/objects refused.
    db->exec("CREATE TABLE j(x)");
    db->exec("INSERT INTO j VALUES(?),(?),(?),(?),(?),(?)",
             {JVal(), JVal(false), JVal(true), JVal(-7), JVal(0.5), JVal("s")});
    SEQ(json::stringify(db->query("SELECT x FROM j ORDER BY rowid")),
        "[{\"x\":null},{\"x\":0},{\"x\":1},{\"x\":-7},{\"x\":0.5},{\"x\":\"s\"}]");
    const Caught arr = catchDb([&] { db->exec("INSERT INTO j VALUES(?)", {JVal::array({1})}); });
    QCOMPARE(arr.code, SQLITE_MISMATCH);
    const Caught obj = catchDb([&] { db->exec("INSERT INTO j VALUES(?)", {JVal::object({{"a", 1}})}); });
    QCOMPARE(obj.code, SQLITE_MISMATCH);

    // int64 edges and 2^53 + 1 stay exact; doubles round-trip.
    const JVal edges = first(*db, "SELECT ? a, ? b, ? c, ? d", {JVal(static_cast<long long>(INT64_MIN)),
                                                                 JVal(static_cast<long long>(INT64_MAX)),
                                                                 JVal(9007199254740993LL), JVal(-0.1)});
    SEQ(json::stringify(edges), "{\"a\":-9223372036854775808,\"b\":9223372036854775807,\"c\":9007199254740993,\"d\":-0.1}");

    // Rows follow the SELECT order; a repeated name keeps its first position and the last value (JS object).
    SEQ(json::stringify(first(*db, "SELECT h, a, e FROM t")), "{\"h\":\"\",\"a\":null,\"e\":\"" + uni + "\"}");
    SEQ(json::stringify(db->query("SELECT 1 AS x, 2 AS y, 3 AS x")), "[{\"x\":3,\"y\":2}]");
    db->read([&](DbConn& c) {
        Stmt& s = c.prepare("SELECT 1 AS x, 2 AS y, 3 AS x");
        QVERIFY(s.step());
        SEQ(json::stringify(s.row()), "{\"x\":3,\"y\":2}");
        QCOMPARE(s.columnCount(), 3);
        SEQ(s.columnName(2), "x");
    });

    // Column accessors (types first: after a text conversion sqlite3_column_type is undefined).
    db->read([&](DbConn& c) {
        Stmt& s = c.prepare("SELECT a, b, d, e, f FROM t");
        QVERIFY(s.step());
        QVERIFY(s.value(0).isNull());
        QVERIFY(s.value(1).isInt() && s.value(2).isDouble() && s.value(3).isString() && s.value(4).isString());
        QVERIFY(s.isNull(0) && !s.isNull(1));
        QCOMPARE(s.i64(1), 42);
        QCOMPARE(s.dbl(2), 3.14159265358979);
        SEQ(s.str(0), "");
        SEQ(s.str(1), "42");
        SEQ(s.str(3), uni);
        QVERIFY(s.str(4) == blob);
        QVERIFY(!s.step());
    });
    // A 2 MB text value round-trips.
    const std::string big(2 << 20, 'q');
    QCOMPARE(db->scalar("SELECT length(?)", {big}).asInt(), int64_t(2 << 20));
}

void TestAnfsCoreDb::execQueryScalarScripts()
{
    auto db = Db::open(":memory:");
    QCOMPARE(db->exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT)"), 0);
    QCOMPARE(db->exec("INSERT INTO t(v) VALUES(?)", {"x"}), 1);
    QCOMPARE(db->exec("UPDATE t SET v = 'y'"), 1);
    // better-sqlite3 / C#: a statement that changed nothing reports 0, not the previous statement's count.
    QCOMPARE(db->exec("CREATE INDEX t_v ON t(v)"), 0);
    QCOMPARE(db->exec("SELECT * FROM t"), 0);
    db->exec("INSERT INTO t(v) VALUES('a'),('b'),('c')");
    QCOMPARE(db->exec("DELETE FROM t WHERE id > ?", {1}), 3);
    QCOMPARE(db->exec("DELETE FROM t WHERE id > ?", {1}), 0);

    QVERIFY(db->query("SELECT * FROM t WHERE id < 0").isArray());
    QCOMPARE(db->query("SELECT * FROM t WHERE id < 0").size(), size_t(0));
    QVERIFY(db->scalar("SELECT v FROM t WHERE id < 0").isNull());
    QVERIFY(db->scalar("SELECT NULL").isNull());
    db->write([&](DbConn& c) {
        QVERIFY(c.queryOne("SELECT * FROM t WHERE id < 0").isNull());
        SEQ(json::stringify(c.queryOne("SELECT id, v FROM t")), "{\"id\":1,\"v\":\"y\"}");
    });
    QCOMPARE(db->queryRead("SELECT * FROM t").size(), size_t(1));

    // exec without parameters runs scripts (C# Db.Exec); with parameters it refuses them.
    QCOMPARE(db->exec("INSERT INTO t(v) VALUES('s1'); INSERT INTO t(v) VALUES('s2');"), 1);
    QCOMPARE(count(*db, "SELECT count(*) FROM t"), 3);
    const Caught mp = catchDb([&] { db->exec("INSERT INTO t(v) VALUES(?); INSERT INTO t(v) VALUES('z')", {"p"}); });
    QCOMPARE(mp.code, SQLITE_MISUSE);
    QCOMPARE(count(*db, "SELECT count(*) FROM t"), 3);
    QCOMPARE(catchDb([&] { db->query("SELECT 1; SELECT 2"); }).code, SQLITE_MISUSE);
    QCOMPARE(catchDb([&] { db->write([](DbConn& c) { c.prepare("SELECT 1; SELECT 2"); }); }).code, SQLITE_MISUSE);
    // Trailing separators and comments are not a second statement.
    QCOMPARE(db->scalar("SELECT 5; -- five").asInt(), 5);
    QCOMPARE(db->scalar("SELECT 6 /* six */ ;;  ").asInt(), 6);
    QCOMPARE(db->exec(""), 0);
    QCOMPARE(db->exec("  -- only a comment\n"), 0);
    SEQ(catchDb([&] { db->exec("", {1}); }).what, "empty SQL");
    QCOMPARE(catchDb([&] { db->write([](DbConn& c) { c.prepare("  "); }); }).code, SQLITE_MISUSE);

    db->write([&](DbConn& c) {
        c.execScript("CREATE TABLE s(a); INSERT INTO s VALUES(1); /* c */ INSERT INTO s VALUES(2);\n-- end");
        c.execScript("");
    });
    QCOMPARE(count(*db, "SELECT count(*) FROM s"), 2);
    const Caught sp = catchDb([&] { db->write([](DbConn& c) { c.execScript("INSERT INTO s VALUES(?)"); }); });
    QCOMPARE(sp.code, SQLITE_RANGE);
    SEQ(sp.what, "SQL has parameters but none were supplied");
    // Parameter count checks (C# texts).
    SEQ(catchDb([&] { db->exec("INSERT INTO s VALUES(?)", {1, 2}); }).what, "SQL has 1 parameters but 2 were supplied");
    SEQ(catchDb([&] { db->exec("INSERT INTO s VALUES(?)"); }).what, "SQL has 1 parameters but 0 were supplied");
    QCOMPARE(count(*db, "SELECT count(*) FROM s"), 2);
    // A script stops at its first failing statement; the statements before it stay (no implicit transaction).
    QVERIFY(catchDb([&] { db->exec("INSERT INTO s VALUES(3); INSERT INTO nope VALUES(4); INSERT INTO s VALUES(5)"); })
                .thrown);
    QCOMPARE(count(*db, "SELECT count(*) FROM s"), 3);
}

void TestAnfsCoreDb::errorsCarryCodes()
{
    auto db = Db::open(":memory:");
    const Caught syn = catchDb([&] { db->exec("SELEC 1"); });
    QCOMPARE(syn.code, SQLITE_ERROR);
    QCOMPARE(syn.ext, SQLITE_ERROR);
    SEQ(syn.what, "near \"SELEC\": syntax error");
    const Caught nt = catchDb([&] { db->query("SELECT * FROM nope"); });
    SEQ(nt.what, "no such table: nope");
    // A failing step leaves the connection usable and the statement idle.
    db->exec("CREATE TABLE c(x CHECK (x > 0))");
    const Caught ck = catchDb([&] { db->exec("INSERT INTO c VALUES(?)", {-1}); });
    QCOMPARE(ck.ext, SQLITE_CONSTRAINT_CHECK);
    db->write([&](DbConn& c) {
        QCOMPARE(busyStatements(c.handle()), 0);
        QCOMPARE(c.exec("INSERT INTO c VALUES(?)", {1}), 1);
    });
    // DbError is a std::runtime_error: generic handlers see the SQLite text.
    SEQ(catchStd([&] { db->exec("INSERT INTO c VALUES(0)"); }), "CHECK constraint failed: x > 0");
}

void TestAnfsCoreDb::statementCache()
{
    auto db = Db::open(":memory:");
    db->exec("CREATE TABLE t(i)");
    db->tx([&](DbConn& c) {
        for (int i = 0; i < 10; ++i) c.exec("INSERT INTO t VALUES(?)", {i});
    });
    void* h = nullptr;
    db->write([&](DbConn& c) {
        h = c.handle();
        Stmt& a = c.prepare("SELECT i FROM t ORDER BY i");
        Stmt& b = c.prepare("SELECT i FROM t ORDER BY i");
        QCOMPARE(&a, &b);                                 // cached and idle: the same statement
        QVERIFY(a.step());
        QCOMPARE(a.i64(0), 0);
        // The same SQL while `a` is stepping gets a twin; `a` is not reset under its caller.
        Stmt& twin = c.prepare("SELECT i FROM t ORDER BY i");
        QVERIFY(&twin != &a);
        QCOMPARE(c.query("SELECT i FROM t ORDER BY i").size(), size_t(10));
        QVERIFY(a.step());
        QCOMPARE(a.i64(0), 1);
        // 70 more distinct statements: the cache stays at 64, and the stepping `a` (least recently used) survives.
        for (int k = 0; k < 70; ++k) QCOMPARE(c.scalar("SELECT " + std::to_string(k)).asInt(), k);
        QCOMPARE(allStatements(h), 64);
        QVERIFY(a.step());
        QCOMPARE(a.i64(0), 2);
        QCOMPARE(busyStatements(h), 1);
    });
    QCOMPARE(busyStatements(h), 0);                       // reset when the callback returned

    // A Stmt& from prepare() is never evicted before the outermost callback returns, also while it is idle between
    // run() and reset() (the ingest pattern) and 100 other statements pass through the cache.
    db->exec("CREATE TABLE p(i, s)");
    db->write([&](DbConn& c) {
        Stmt& ins = c.prepare("INSERT INTO p VALUES(?, ?)");
        ins.bind(1, 1).bind(2, "first");
        QCOMPARE(ins.run(), 1);
        ins.reset();
        db->tx([&](DbConn& t) {                           // nested callbacks on the writer keep the pin too
            for (int k = 0; k < 100; ++k) QCOMPARE(t.scalar("SELECT " + std::to_string(1000 + k)).asInt(), 1000 + k);
        });
        QCOMPARE(allStatements(h), 64);                   // at the cap: the others made room, `ins` stayed
        QCOMPARE(&c.prepare("INSERT INTO p VALUES(?, ?)"), &ins);
        ins.bind(1, 2).bind(2, "second");
        QCOMPARE(ins.run(), 1);
        ins.reset();
        // Held statements are never pulled away: 70 more grow the cache past 64 (63 unpinned ones made room first).
        std::vector<Stmt*> held;
        for (int k = 0; k < 70; ++k) held.push_back(&c.prepare("SELECT " + std::to_string(2000 + k) + " AS v"));
        QCOMPARE(allStatements(h), 71);
        for (int k = 0; k < 70; ++k) {
            QVERIFY(held[k]->step());
            QCOMPARE(held[k]->i64(0), 2000 + k);
        }
    });
    QCOMPARE(busyStatements(h), 0);
    // The pins ended with the callback: the next new statement brings the cache back to 64.
    SEQ(db->scalar("SELECT 'next'").asString(), "next");
    QCOMPARE(allStatements(h), 64);
    SEQ(json::stringify(db->query("SELECT i, s FROM p ORDER BY i")),
        "[{\"i\":1,\"s\":\"first\"},{\"i\":2,\"s\":\"second\"}]");

    // A reader left mid-iteration by its callback must not keep a stale snapshot or pin the WAL.
    QTemporaryDir tmp;
    Db::Options o;
    o.readers = 1;                                        // one reader: the same connection every time
    auto f = Db::open(ss(tmp.path() + "/c.db"), o);
    f->exec("CREATE TABLE t(i)");
    f->exec("INSERT INTO t VALUES(1)");
    void* rh = nullptr;
    f->read([&](DbConn& c) {
        rh = c.handle();
        Stmt& s = c.prepare("SELECT i FROM t");
        QVERIFY(s.step());                                // left stepping on purpose
    });
    QCOMPARE(busyStatements(rh), 0);
    f->exec("INSERT INTO t VALUES(2)");
    QCOMPARE(f->queryRead("SELECT i FROM t").size(), size_t(2));
    const JVal ck = first(*f, "PRAGMA wal_checkpoint(TRUNCATE)");
    QCOMPARE(ck.getInt("busy", -1), 0);
}

void TestAnfsCoreDb::transactions()
{
    QTemporaryDir tmp;
    Db::Options o;
    o.readers = 2;
    auto db = Db::open(ss(tmp.path() + "/tx.db"), o);
    db->exec("CREATE TABLE t(v TEXT)");
    auto has = [&](const char* v) { return count(*db, "SELECT count(*) FROM t WHERE v = ?", {v}); };

    db->tx([&](DbConn& c) { c.exec("INSERT INTO t VALUES('commit')"); });
    QCOMPARE(db->queryRead("SELECT * FROM t WHERE v = 'commit'").size(), size_t(1));

    SEQ(catchStd([&] {
            db->tx([&](DbConn& c) {
                c.exec("INSERT INTO t VALUES('rolled')");
                throw std::runtime_error("boom");
            });
        }),
        "boom");
    QCOMPARE(has("rolled"), 0);
    db->write([&](DbConn& c) { QVERIFY(sqlite3_get_autocommit(static_cast<sqlite3*>(c.handle())) != 0); });

    // Nested tx joins the outer transaction (C# Db.Tx): the outer throw rolls back both...
    catchStd([&] {
        db->tx([&](DbConn& c) {
            c.exec("INSERT INTO t VALUES('outer1')");
            db->tx([&](DbConn& i) { i.exec("INSERT INTO t VALUES('inner1')"); });
            throw std::runtime_error("x");
        });
    });
    QCOMPARE(has("outer1") + has("inner1"), 0);
    // ...and an inner throw caught by the outer callback still commits with it.
    db->tx([&](DbConn& c) {
        c.exec("INSERT INTO t VALUES('outer2')");
        catchStd([&] {
            db->tx([&](DbConn& i) {
                i.exec("INSERT INTO t VALUES('inner2')");
                throw std::runtime_error("y");
            });
        });
    });
    QCOMPARE(has("outer2") + has("inner2"), 2);

    // tx inside write() begins its own transaction and commits it.
    db->write([&](DbConn& c) {
        sqlite3* h = static_cast<sqlite3*>(c.handle());
        QVERIFY(sqlite3_get_autocommit(h) != 0);
        db->tx([&](DbConn& t) {
            QVERIFY(sqlite3_get_autocommit(static_cast<sqlite3*>(t.handle())) == 0);
            t.exec("INSERT INTO t VALUES('in-write')");
        });
        QVERIFY(sqlite3_get_autocommit(h) != 0);
        int64_t seen = -1;
        std::thread([&] { seen = db->queryRead("SELECT * FROM t WHERE v = 'in-write'").size(); }).join();
        QCOMPARE(seen, 1);
    });

    // A transaction opened by hand and left open (a script that stopped at its failing statement after BEGIN) is
    // rolled back when the outermost writer callback returns, so later tx() calls never join it and never commit.
    QVERIFY(catchDb([&] { db->exec("BEGIN; INSERT INTO t VALUES('stray'); INSERT INTO nope VALUES(1); COMMIT"); }).thrown);
    db->write([&](DbConn& c) { QVERIFY(sqlite3_get_autocommit(static_cast<sqlite3*>(c.handle())) != 0); });
    QCOMPARE(has("stray"), 0);
    db->tx([&](DbConn& c) { c.exec("INSERT INTO t VALUES('after-stray')"); });
    int64_t afterStray = -1;
    std::thread([&] { afterStray = db->queryRead("SELECT * FROM t WHERE v = 'after-stray'").size(); }).join();
    QCOMPARE(afterStray, 1);                              // committed: another thread's reader sees it
    // Inside one write() callback, tx() does not join a hand-made transaction: it fails as C# Db.Tx did.
    db->write([&](DbConn& c) {
        c.exec("BEGIN");
        c.exec("INSERT INTO t VALUES('by-hand')");
        const Caught in = catchDb([&] { db->tx([](DbConn& t) { t.exec("INSERT INTO t VALUES('joined')"); }); });
        QCOMPARE(in.code, SQLITE_ERROR);
        SEQ(in.what, "cannot start a transaction within a transaction");
    });
    QCOMPARE(has("by-hand") + has("joined"), 0);
    // A reader callback that left a BEGIN open does not keep its snapshot.
    void* rh = nullptr;
    db->read([&](DbConn& r) {
        rh = r.handle();
        r.exec("BEGIN");
        QCOMPARE(r.scalar("SELECT count(*) FROM t WHERE v = 'commit'").asInt(), 1);
        QVERIFY(sqlite3_get_autocommit(static_cast<sqlite3*>(rh)) == 0);
    });
    QVERIFY(sqlite3_get_autocommit(static_cast<sqlite3*>(rh)) != 0);

    // Shortcuts and read() inside tx on the same thread: no deadlock; read() sees the open transaction's rows while
    // another thread's reader does not.
    db->tx([&](DbConn& c) {
        db->exec("INSERT INTO t VALUES('shortcut')");
        QCOMPARE(db->scalar("SELECT count(*) FROM t WHERE v = 'shortcut'").asInt(), 1);
        db->read([&](DbConn& r) {
            QCOMPARE(r.handle(), c.handle());
            QCOMPARE(r.scalar("SELECT count(*) FROM t WHERE v = 'shortcut'").asInt(), 1);
        });
        QCOMPARE(db->queryRead("SELECT * FROM t WHERE v = 'shortcut'").size(), size_t(1));
        int64_t other = -1;
        std::thread([&] { other = db->queryRead("SELECT * FROM t WHERE v = 'shortcut'").size(); }).join();
        QCOMPARE(other, 0);
    });
    QCOMPARE(has("shortcut"), 1);

    // Other threads wait for the writer: an exec started during a 200 ms tx finishes after it.
    std::atomic<bool> inTx{false};
    QElapsedTimer t;
    int64_t waitedMs = -1;
    std::thread other;
    db->tx([&](DbConn& c) {
        c.exec("INSERT INTO t VALUES('long')");
        other = std::thread([&] {
            QElapsedTimer w;
            w.start();
            db->exec("INSERT INTO t VALUES('after-long')");
            waitedMs = w.elapsed();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    });
    other.join();
    QVERIFY2(waitedMs >= 150, qPrintable(QString::number(waitedMs)));
    QCOMPARE(has("after-long"), 1);

    // A second connection (as another process would be) waits busy_timeout for the write lock, then SQLITE_BUSY.
    Db::Options quick;
    quick.readers = 0;
    quick.busyTimeoutMs = 150;
    auto second = Db::open(db->path(), quick);
    std::promise<Caught> busy;
    db->tx([&](DbConn&) {
        std::thread([&] {
            QElapsedTimer w;
            w.start();
            Caught c = catchDb([&] { second->exec("INSERT INTO t VALUES('second')"); });
            c.what += " after " + std::to_string(w.elapsed()) + " ms";
            busy.set_value(c);
        }).join();
    });
    const Caught b = busy.get_future().get();
    QVERIFY2(b.thrown && b.code == SQLITE_BUSY, b.what.c_str());
    QCOMPARE(second->exec("INSERT INTO t VALUES('second')"), 1);   // free again after the tx

    // A read-only Db cannot begin a write transaction.
    Db::Options ro;
    ro.readOnly = true;
    auto r = Db::open(db->path(), ro);
    QCOMPARE(catchDb([&] { r->tx([](DbConn& c) { c.exec("INSERT INTO t VALUES('ro')"); }); }).code, SQLITE_READONLY);
    QCOMPARE(r->queryRead("SELECT * FROM t WHERE v = 'commit'").size(), size_t(1));
}

void TestAnfsCoreDb::readersRunBesideTheWriter()
{
    QTemporaryDir tmp;
    auto db = Db::open(ss(tmp.path() + "/w.db"));
    db->exec("CREATE TABLE w(id INTEGER PRIMARY KEY, grp INTEGER NOT NULL, v TEXT NOT NULL)");
    db->exec("CREATE INDEX w_grp ON w(grp)");
    const std::string pad(48, 'x');
    int64_t grp = 0;
    std::vector<int64_t> txMs;
    auto batch = [&] {   // one 2,000-row ingest transaction (CONVENTIONS.md section 1)
        QElapsedTimer t;
        t.start();
        const int64_t g = ++grp;
        db->tx([&](DbConn& c) {
            Stmt& ins = c.prepare("INSERT INTO w(grp, v) VALUES(?, ?)");
            for (int i = 0; i < 2000; ++i) {
                ins.bind(1, g).bind(2, pad);
                ins.run();
                ins.reset();
            }
        });
        return t.nsecsElapsed() / 1000000;
    };
    for (int i = 0; i < 50; ++i) batch();                 // 100,000 rows

    std::atomic<bool> stop{false}, writing{false}, longOpen{false};
    std::atomic<int> active{0}, maxActive{0}, errors{0};
    std::atomic<int64_t> reads{0}, readsWhileWriting{0}, torn{0};
    std::mutex errMu;
    std::string firstErr;
    auto reader = [&] {
        while (!stop) {
            try {
                db->read([&](DbConn& c) {
                    updateMax(maxActive, ++active);
                    const JVal r = c.queryOne("SELECT count(*) AS n, count(DISTINCT grp) AS g FROM w");
                    if (r.getInt("n") != r.getInt("g") * 2000) ++torn;   // only whole transactions are visible
                    --active;
                });
                ++reads;
                if (writing) ++readsWhileWriting;
            } catch (const std::exception& e) {
                ++errors;
                std::lock_guard<std::mutex> lk(errMu);
                if (firstErr.empty()) firstErr = e.what();
            }
        }
    };
    // A read transaction held open for 400 ms on one reader: the writer must not wait for it.
    int64_t snap0 = -1, snap1 = -2;
    auto longReader = [&] {
        db->read([&](DbConn& c) {
            Stmt& s = c.prepare("SELECT id FROM w ORDER BY id");
            if (!s.step()) return;
            snap0 = c.scalar("SELECT count(*) FROM w").asInt();
            longOpen = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            snap1 = c.scalar("SELECT count(*) FROM w").asInt();
            longOpen = false;
        });
    };

    std::vector<std::thread> threads;
    threads.emplace_back(longReader);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!longOpen && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!longOpen) {
        threads[0].join();
        QFAIL("the long read never started");
    }
    for (int i = 0; i < 3; ++i) threads.emplace_back(reader);
    writing = true;
    int txWhileLongRead = 0;
    for (int i = 0; i < 40; ++i) {
        const bool during = longOpen;
        txMs.push_back(batch());
        if (during && longOpen) ++txWhileLongRead;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    writing = false;
    stop = true;
    for (auto& t : threads) t.join();

    const int64_t maxTx = *std::max_element(txMs.begin(), txMs.end());
    int64_t sum = 0;
    for (int64_t v : txMs) sum += v;
    qInfo("writer: 40 tx of 2,000 rows, max %lld ms, mean %lld ms, %d committed during the open read; readers: %lld "
          "reads (%lld during writes), max %d at once",
          static_cast<long long>(maxTx), static_cast<long long>(sum / 40), txWhileLongRead,
          static_cast<long long>(reads.load()), static_cast<long long>(readsWhileWriting.load()), maxActive.load());
    QVERIFY2(errors == 0, firstErr.c_str());
    QVERIFY2(maxTx < 250, qPrintable(QString::number(maxTx)));
    QVERIFY(txWhileLongRead >= 2);
    QCOMPARE(snap0, snap1);                               // the open read kept its snapshot
    QCOMPARE(torn.load(), int64_t(0));
    QVERIFY(readsWhileWriting >= 10);
    QVERIFY(maxActive >= 2);
    QCOMPARE(count(*db, "SELECT count(*) FROM w"), int64_t(90 * 2000));
    QCOMPARE(db->queryRead("SELECT count(*) AS n FROM w").at(0).getInt("n"), int64_t(90 * 2000));
    QCOMPARE(first(*db, "PRAGMA wal_checkpoint(TRUNCATE)").getInt("busy", -1), 0);   // nothing pinned
}

void TestAnfsCoreDb::readerPoolWaits()
{
    QTemporaryDir tmp;
    auto db = Db::open(ss(tmp.path() + "/p.db"));       // 4 readers (default)
    db->exec("CREATE TABLE t(i)");
    db->exec("INSERT INTO t VALUES(1)");
    std::atomic<int> active{0}, maxActive{0}, wrong{0};
    std::mutex mu;
    std::set<void*> handles;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&, t] {
            for (int k = 0; k < 15; ++k)
                db->read([&](DbConn& c) {
                    updateMax(maxActive, ++active);
                    {
                        std::lock_guard<std::mutex> lk(mu);
                        handles.insert(c.handle());
                    }
                    if (c.scalar("SELECT count(*) FROM t").asInt() != 1) ++wrong;
                    if (t == 0 && k == 0)
                        db->read([&](DbConn& inner) {
                            if (inner.handle() != c.handle()) ++wrong;
                        });
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                    --active;
                });
        });
    for (auto& th : threads) th.join();
    QCOMPARE(wrong.load(), 0);
    QVERIFY2(maxActive <= 4 && maxActive >= 2, qPrintable(QString::number(maxActive.load())));
    QCOMPARE(handles.size(), size_t(4));

    // All four readers held, then each holder reads again: the nested read reuses its own reader (no deadlock).
    std::atomic<int> holding{0}, nestedOk{0};
    auto holder = [&] {
        db->read([&](DbConn& c) {
            ++holding;
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (holding < 4 && std::chrono::steady_clock::now() < until) std::this_thread::yield();
            db->read([&](DbConn& inner) {
                if (inner.handle() == c.handle()) ++nestedOk;
            });
            db->queryRead("SELECT 1");
        });
    };
    auto all = std::async(std::launch::async, [&] {
        std::vector<std::thread> hs;
        for (int i = 0; i < 4; ++i) hs.emplace_back(holder);
        for (auto& h : hs) h.join();
    });
    QVERIFY(all.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    QCOMPARE(nestedOk.load(), 4);
}

void TestAnfsCoreDb::modSchemaCsharpCases()
{
    // C# SelfTest FsCore_ModSchema, case by case.
    auto db = Db::open(":memory:");
    const std::vector<std::string> s1 = {"CREATE TABLE m_a (id INTEGER PRIMARY KEY, v TEXT)"};
    ModSchema::apply(*db, "m", s1);
    QCOMPARE(count(*db, "SELECT version FROM _mod_schema WHERE module = ?", {"m"}), 1);
    QCOMPARE(count(*db, "SELECT count(*) FROM m_a"), 0);
    db->exec("INSERT INTO m_a(v) VALUES(?)", {"keep"});
    ModSchema::apply(*db, "m", s1);
    QCOMPARE(count(*db, "SELECT version FROM _mod_schema WHERE module = ?", {"m"}), 1);
    QCOMPARE(count(*db, "SELECT count(*) FROM m_a"), 1);

    const std::vector<std::string> s2 = {s1[0], "CREATE TABLE m_b (id INTEGER PRIMARY KEY); CREATE INDEX m_b_i ON m_b(id)"};
    ModSchema::apply(*db, "m", s2);
    QCOMPARE(ModSchema::version(*db, "m"), 2);
    QCOMPARE(count(*db, "SELECT count(*) FROM sqlite_master WHERE name = 'm_b_i'"), 1);
    QCOMPARE(count(*db, "SELECT count(*) FROM m_a"), 1);

    SEQ(catchStd([&] { ModSchema::apply(*db, "m", s1); }), "m schema version 2 is newer than this build (1 steps)");
    QCOMPARE(ModSchema::version(*db, "m"), 2);

    std::vector<std::string> s3 = {s2[0], s2[1], "CREATE TABLE m_c (id INTEGER); CREATE TABLE m_d (this is not valid sql"};
    std::string sqliteText;                               // what SQLite says about the bad statement on its own
    {
        auto scratch = Db::open(":memory:");
        sqliteText = catchDb([&] { scratch->write([](DbConn& c) { c.execScript("CREATE TABLE m_d (this is not valid sql"); }); }).what;
    }
    QVERIFY(!sqliteText.empty());
    SEQ(catchStd([&] { ModSchema::apply(*db, "m", s3); }), "m schema step 3 failed: " + sqliteText);
    QCOMPARE(ModSchema::version(*db, "m"), 2);
    QCOMPARE(count(*db, "SELECT count(*) FROM sqlite_master WHERE name = 'm_c'"), 0);   // its first statement rolled back

    s3[2] = "CREATE TABLE m_c (id INTEGER)";
    ModSchema::apply(*db, "m", s3);
    QCOMPARE(ModSchema::version(*db, "m"), 3);

    ModSchema::apply(*db, "other", {"CREATE TABLE o_a (id INTEGER)", "CREATE TABLE o_b (id INTEGER)"});
    QCOMPARE(ModSchema::version(*db, "other"), 2);
    QCOMPARE(ModSchema::version(*db, "m"), 3);
    ModSchema::apply(*db, "empty", {});
    QVERIFY(db->scalar("SELECT version FROM _mod_schema WHERE module = ?", {"empty"}).isNull());
    QCOMPARE(ModSchema::version(*db, "empty"), 0);
    QCOMPARE(ModSchema::version(*db, "unknown"), 0);

    // The table text is byte-identical to C# FsCoreSchema.cs (checked against the C# file in the oracle slot too).
    SEQ(db->scalar("SELECT sql FROM sqlite_master WHERE name = '_mod_schema'").asString(),
        "CREATE TABLE _mod_schema (module TEXT PRIMARY KEY, version INTEGER NOT NULL)");
    // Blank steps only bump the version; an empty module name is refused; version() never creates the table.
    ModSchema::apply(*db, "blank", {"CREATE TABLE b1(x)", "  \r\n\t", "", "CREATE TABLE b2(x)"});
    QCOMPARE(ModSchema::version(*db, "blank"), 4);
    SEQ(catchStd([&] { ModSchema::apply(*db, "", s1); }), "module");
    auto fresh = Db::open(":memory:");
    QCOMPARE(ModSchema::version(*fresh, "m"), 0);
    QCOMPARE(count(*fresh, "SELECT count(*) FROM sqlite_master"), 0);
    // A REAL version (INTEGER affinity keeps 2.5) reads like C# Convert.ToInt64: half to even.
    fresh->exec("CREATE TABLE _mod_schema (module TEXT PRIMARY KEY, version INTEGER NOT NULL)");
    fresh->exec("INSERT INTO _mod_schema VALUES('r', 2.5), ('s', 3.5)");
    QCOMPARE(ModSchema::version(*fresh, "r"), 2);
    QCOMPARE(ModSchema::version(*fresh, "s"), 4);

    // File database with readers: version() right after apply; 4 threads applying the same steps run each step once.
    QTemporaryDir tmp;
    auto f = Db::open(ss(tmp.path() + "/ms.db"));
    const std::vector<std::string> once = {"CREATE TABLE once_t(x)", "INSERT INTO once_t VALUES(1)",
                                           "CREATE TABLE once_u(y); INSERT INTO once_u VALUES(2)"};
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i)
        ts.emplace_back([&] {
            try {
                ModSchema::apply(*f, "once", once);
            } catch (const std::exception&) {
                ++errors;
            }
        });
    for (auto& t : ts) t.join();
    QCOMPARE(errors.load(), 0);
    QCOMPARE(ModSchema::version(*f, "once"), 3);
    QCOMPARE(f->queryRead("SELECT * FROM once_t").size(), size_t(1));
    QCOMPARE(f->queryRead("SELECT * FROM once_u").size(), size_t(1));
}

void TestAnfsCoreDb::fts5AndJson()
{
    QTemporaryDir tmp;
    auto db = Db::open(ss(tmp.path() + "/fts.db"));
    db->exec("CREATE VIRTUAL TABLE docs USING fts5(title, body, tokenize = 'unicode61 remove_diacritics 2')");
    db->tx([&](DbConn& c) {
        c.exec("INSERT INTO docs(title, body) VALUES(?, ?)", {"Caf\xC3\xA9 cr\xC3\xA8me", "Morning broadcast from Martinsville"});
        c.exec("INSERT INTO docs(title, body) VALUES(?, ?)", {"News at noon", "Broadcasting the caf\xC3\xA9 opening"});
        c.exec("INSERT INTO docs(title, body) VALUES(?, ?)", {"Weather", "Sunny and warm"});
    });
    auto titles = [&](const char* match) {
        std::set<std::string> out;
        const JVal rows = db->queryRead("SELECT title FROM docs WHERE docs MATCH ? ORDER BY rank", {match});
        for (const JVal& r : rows.arr()) out.insert(r.getStr("title"));
        return out;
    };
    QCOMPARE(titles("cafe").size(), size_t(2));           // diacritics folded
    QCOMPARE(titles("broad*").size(), size_t(2));         // prefix query
    QCOMPARE(titles("title:weather").size(), size_t(1));  // column filter
    QCOMPARE(titles("nothing").size(), size_t(0));
    SEQ(db->queryRead("SELECT highlight(docs, 0, '[', ']') AS h FROM docs WHERE docs MATCH 'title:cafe'").at(0).getStr("h"),
        "[Caf\xC3\xA9] cr\xC3\xA8me");
    QVERIFY(db->queryRead("SELECT bm25(docs) AS s FROM docs WHERE docs MATCH 'cafe'").at(0).get("s").isDouble());

    QCOMPARE(db->queryRead("SELECT sum(value) AS s FROM json_each('[1,2,3]')").at(0).getInt("s"), 6);
    SEQ(db->scalar("SELECT json_object('a', 1, 'b', 'x')").asString(), "{\"a\":1,\"b\":\"x\"}");
    QCOMPARE(db->scalar("SELECT '{\"a\":{\"b\":7}}' ->> '$.a.b'").asInt(), 7);
}

void TestAnfsCoreDb::openCopyReadOnlyGuard()
{
    QTemporaryDir tmp;
    const QString liveDir = tmp.path() + "/live", copyDir = tmp.path() + "/copy";
    QVERIFY(QDir().mkpath(copyDir));
    const std::string livePath = ss(liveDir + "/anfs.db");
    auto live = Db::open(livePath);
    live->exec("CREATE TABLE t(v)");
    live->exec("INSERT INTO t VALUES('a'),('b'),('c')");
    QVERIFY(QFileInfo::exists(liveDir + "/anfs.db-wal"));

    // The live file: its -wal was just written.
    const Caught hot = catchDb([&] { Db::openCopyReadOnly(livePath); });
    QVERIFY(hot.thrown);
    QCOMPARE(hot.code, SQLITE_BUSY);
    QVERIFY2(hot.what.find("-wal") != std::string::npos && hot.what.find("looks live") != std::string::npos,
             hot.what.c_str());

    // A copy taken while it runs keeps the -wal's time (CopyFileW): still refused until 60 s passed.
    QVERIFY(QFile::copy(liveDir + "/anfs.db", copyDir + "/anfs.db"));
    QVERIFY(QFile::copy(liveDir + "/anfs.db-wal", copyDir + "/anfs.db-wal"));
    const std::string copyPath = ss(copyDir + "/anfs.db");
    QCOMPARE(catchDb([&] { Db::openCopyReadOnly(copyPath); }).code, SQLITE_BUSY);
    const int64_t walMs = fs::stat(copyPath + "-wal").mtimeMs;
    {
        ManualClock mc(walMs + 59999);
        ScopedClock sc(mc);
        QCOMPARE(catchDb([&] { Db::openCopyReadOnly(copyPath); }).code, SQLITE_BUSY);
        mc.set(walMs - 5000);                             // a -wal stamped in the future counts as fresh
        QCOMPARE(catchDb([&] { Db::openCopyReadOnly(copyPath); }).code, SQLITE_BUSY);
        mc.set(walMs + 60000);
        auto c = Db::openCopyReadOnly(copyPath);
        QCOMPARE(c->queryRead("SELECT v FROM t").size(), size_t(3));
    }

    // With the system clock once the copy's -wal is two minutes old; rows come from the -wal; nothing can be written;
    // the copy's bytes are unchanged afterwards (no checkpoint at close).
    QVERIFY(setMtime(copyDir + "/anfs.db-wal", QDateTime::currentDateTimeUtc().addSecs(-120)));
    const QByteArray dbHash = sha(copyDir + "/anfs.db"), walHash = sha(copyDir + "/anfs.db-wal");
    {
        auto c = Db::openCopyReadOnly(copyPath);
        SEQ(c->path(), fs::fullPath(copyPath));
        SEQ(json::stringify(c->query("SELECT v FROM t ORDER BY v")), "[{\"v\":\"a\"},{\"v\":\"b\"},{\"v\":\"c\"}]");
        QCOMPARE(catchDb([&] { c->exec("INSERT INTO t VALUES('x')"); }).code, SQLITE_READONLY);
        QCOMPARE(catchDb([&] { c->tx([](DbConn& t) { t.exec("DELETE FROM t"); }); }).code, SQLITE_READONLY);
        c->read([&](DbConn& r) { QVERIFY(r.readOnly()); });
    }
    QCOMPARE(sha(copyDir + "/anfs.db"), dbHash);
    QCOMPARE(sha(copyDir + "/anfs.db-wal"), walHash);
    // The -wal keeps its old time (a read-only connection never writes it), so the same copy reopens at once.
    const int64_t staleWalMs = fs::stat(copyPath + "-wal").mtimeMs;
    QVERIFY(anfs::nowMs() - staleWalMs >= 60000);
    {
        auto again = Db::openCopyReadOnly(copyPath);
        QCOMPARE(again->queryRead("SELECT v FROM t").size(), size_t(3));
    }
    QCOMPARE(fs::stat(copyPath + "-wal").mtimeMs, staleWalMs);
    QCOMPARE(sha(copyDir + "/anfs.db-wal"), walHash);

    // Missing file or a folder: CANTOPEN, nothing created.
    const std::string missing = ss(tmp.path() + "/none/anfs.db");
    QCOMPARE(catchDb([&] { Db::openCopyReadOnly(missing); }).code, SQLITE_CANTOPEN);
    QVERIFY(!QFileInfo::exists(tmp.path() + "/none"));
    QCOMPARE(catchDb([&] { Db::openCopyReadOnly(ss(copyDir)); }).code, SQLITE_CANTOPEN);
    QCOMPARE(catchDb([&] { Db::openCopyReadOnly(""); }).code, SQLITE_CANTOPEN);

    // After the live Db closed cleanly its -wal is gone and the file opens: a cold WAL-mode copy. It opens immutable,
    // so no -wal or -shm appears next to it and opening it twice in a row works (a plain read-only open would leave a
    // fresh empty -wal behind and the guard would then refuse the copy for 60 s); the folder and bytes stay as found.
    live.reset();
    QVERIFY(!QFileInfo::exists(liveDir + "/anfs.db-wal"));
    const QStringList before = QDir(liveDir).entryList(QDir::Files | QDir::Hidden | QDir::System, QDir::Name);
    QCOMPARE(before, QStringList{QStringLiteral("anfs.db")});
    QByteArray header;
    {
        QFile f(liveDir + "/anfs.db");
        QVERIFY(f.open(QIODevice::ReadOnly));
        header = f.read(20);
    }
    QVERIFY(header.size() == 20 && header[18] == 2 && header[19] == 2);   // still WAL mode in the file header
    const QByteArray coldHash = sha(liveDir + "/anfs.db");
    for (int round = 0; round < 2; ++round) {
        auto cold = Db::openCopyReadOnly(livePath);
        SEQ(json::stringify(cold->queryRead("SELECT v FROM t ORDER BY v")),
            "[{\"v\":\"a\"},{\"v\":\"b\"},{\"v\":\"c\"}]");
        QCOMPARE(catchDb([&] { cold->exec("INSERT INTO t VALUES('x')"); }).code, SQLITE_READONLY);
        QCOMPARE(QDir(liveDir).entryList(QDir::Files | QDir::Hidden | QDir::System, QDir::Name), before);
    }
    QCOMPARE(QDir(liveDir).entryList(QDir::Files | QDir::Hidden | QDir::System, QDir::Name), before);
    QCOMPARE(sha(liveDir + "/anfs.db"), coldHash);
    // A path with URI characters ('#', '?' is not allowed on Windows, '%', a space) opens the right file.
    const QString oddDir = tmp.path() + "/odd #1 100% copy";
    QVERIFY(QDir().mkpath(oddDir));
    QVERIFY(QFile::copy(liveDir + "/anfs.db", oddDir + "/anfs.db"));
    {
        auto odd = Db::openCopyReadOnly(ss(oddDir + "/anfs.db"));
        QCOMPARE(odd->queryRead("SELECT v FROM t").size(), size_t(3));
        SEQ(odd->path(), fs::fullPath(ss(oddDir + "/anfs.db")));
    }
    QCOMPARE(QDir(oddDir).entryList(QDir::Files | QDir::Hidden | QDir::System, QDir::Name), before);
}

void TestAnfsCoreDb::csharpOracleSharesTheFile()
{
    const QString ps = findPowerShell51();
    if (ps.isEmpty()) QSKIP("powershell.exe 5.1 not found");
    const QString oracle = oraclePath();
    if (oracle.isEmpty()) QSKIP("C# oracle anfs.exe not found (set ANFS_CSHARP_ORACLE)");
    QTemporaryDir tmp;
    const QString root = QDir::toNativeSeparators(tmp.path());
    const std::string peerPath = ss(tmp.path() + "/gluedb1/peer/anfs.db");

    // Phase 1 (C#): the selftest group Glue_Db_Schema_Once_Idempotent_Reopen run with its Tester rooted here (so the
    // runner's cleanup never deletes the result), then the C# Db reads back what it made.
    const QString phase1 = psPrologue(oracle) + QStringLiteral(
        "$root = %1\n"
        "$t = [StarFs.Tester]::new(); $t.Root = $root\n"
        "$m = $asm.GetType('StarFs.GlueTests').GetMethod('Glue_Db_Schema_Once_Idempotent_Reopen')\n"
        "$inv = [object[]]::new(1); $inv[0] = $t.psobject.BaseObject; [void]$m.Invoke($null, $inv)\n"
        "Say 'glue' ([string]$t.Passed + '/' + [string]$t.Failed)\n"
        "$db = [StarFs.Db]::Open((Join-Path $root 'gluedb1\\peer\\anfs.db'))\n"
        "try {\n"
        "  Say 'sqlite' ([StarFs.Db]::SqliteVersion)\n"
        "  foreach ($r in $db.Query('SELECT module, version FROM _mod_schema ORDER BY module', $none)) { Say 'ver' ([string]$r['module'] + '=' + [string]$r['version']) }\n"
        "  Say 'ddl' $db.Scalar(\"SELECT sql FROM sqlite_master WHERE name = '_mod_schema'\", $none)\n"
        "  Say 'rows' $db.Scalar('SELECT count(*) FROM glue_t', $none)\n"
        "  try { [StarFs.ModSchema]::Apply($db, 'glue-test', [string[]]@('CREATE TABLE glue_t(a INTEGER)')); Say 'newer' 'no throw' } catch { Say 'newer' (Msg $_) }\n"
        "} finally { $db.Dispose() }\n").arg(psQuote(root));
    QProcess p1;
    p1.start(ps, psArgs(phase1));
    QVERIFY(p1.waitForStarted(10000));
    p1.closeWriteChannel();
    QVERIFY(p1.waitForFinished(90000));
    const QByteArray out1 = p1.readAllStandardOutput();
    QVERIFY2(p1.exitCode() == 0, (out1 + p1.readAllStandardError()).left(3000).constData());
    const auto cs1 = parseSay(out1);
    const QStringList glue = one(cs1, "glue").split('/');
    QVERIFY2(glue.size() == 2 && glue[0].toInt() > 0 && glue[1] == "0", qPrintable(one(cs1, "glue")));
    QCOMPARE(one(cs1, "sqlite"), QStringLiteral("3.54.0"));
    QStringList csVersions;
    for (auto [it, end] = cs1.equal_range("ver"); it != end; ++it) csVersions << it->second;
    QVERIFY(csVersions.contains("glue-test=2") && csVersions.contains("glue-late=2"));

    // C++ opens the C#-made file: same versions, same DDL, same rows; the C# texts for the same refusals.
    {
        auto db = Db::open(peerPath);
        QStringList cpp;
        const JVal rows = db->queryRead("SELECT module, version FROM _mod_schema ORDER BY module");
        for (const JVal& r : rows.arr()) cpp << qs(r.getStr("module")) + "=" + QString::number(r.getInt("version"));
        QCOMPARE(cpp, csVersions);
        QCOMPARE(ModSchema::version(*db, "glue-test"), 2);
        QCOMPARE(QString::number(ModSchema::version(*db, "srv-registry")),
                 csVersions.filter(QRegularExpression("^srv-registry=")).value(0).section('=', 1));
        const std::string ddl = db->scalar("SELECT sql FROM sqlite_master WHERE name = '_mod_schema'").asString();
        QCOMPARE(qs(ddl), one(cs1, "ddl"));
        auto mine = Db::open(":memory:");
        ModSchema::apply(*mine, "x", {});
        SEQ(mine->scalar("SELECT sql FROM sqlite_master WHERE name = '_mod_schema'").asString(), ddl);
        QCOMPARE(QString::number(count(*db, "SELECT count(*) FROM glue_t")), one(cs1, "rows"));
        SEQ(db->scalar("PRAGMA integrity_check").asString(), "ok");
        SEQ(db->scalar("PRAGMA journal_mode").asString(), "wal");
        QCOMPARE(qs(catchStd([&] { ModSchema::apply(*db, "glue-test", {"CREATE TABLE glue_t(a INTEGER)"}); })),
                 one(cs1, "newer"));
        ModSchema::apply(*db, "glue-test", {"CREATE TABLE glue_t(a INTEGER)", "INSERT INTO glue_t VALUES(1)",
                                            "CREATE TABLE glue_cpp(x TEXT); INSERT INTO glue_cpp VALUES('from c++')"});
        QCOMPARE(ModSchema::version(*db, "glue-test"), 3);
    }

    // Phase 2 (C#) reads what C++ wrote and applies the same steps; then, with its connection still open, writes a
    // row and waits; meanwhile C++ uses the file through its own WAL connections.
    const QString flags = QDir::toNativeSeparators(tmp.path() + "/flags");
    QVERIFY(QDir().mkpath(flags));
    const QString phase2 = psPrologue(oracle) + QStringLiteral(
        "$root = %1; $flags = %2\n"
        "[StarFs.Db]::Init($root)\n"
        "$s3 = [string[]]@('CREATE TABLE glue_t(a INTEGER)', 'INSERT INTO glue_t VALUES(1)', \"CREATE TABLE glue_cpp(x TEXT); INSERT INTO glue_cpp VALUES('from c++')\")\n"
        "$db = [StarFs.Db]::Open((Join-Path $root 'gluedb1\\peer\\anfs.db'))\n"
        "try {\n"
        "  Say 'ver3' $db.Scalar('SELECT version FROM _mod_schema WHERE module = ?', [object[]]@('glue-test'))\n"
        "  Say 'cpprows' $db.Scalar('SELECT count(*) FROM glue_cpp', $none)\n"
        "  [StarFs.ModSchema]::Apply($db, 'glue-test', $s3)\n"
        "  Say 'after' $db.Scalar('SELECT version FROM _mod_schema WHERE module = ?', [object[]]@('glue-test'))\n"
        "  Say 'cpprows2' $db.Scalar('SELECT count(*) FROM glue_cpp', $none)\n"
        "  try { [StarFs.ModSchema]::Apply($db, 'glue-test', [string[]]@($s3[0], $s3[1])); Say 'newer' 'no throw' } catch { Say 'newer' (Msg $_) }\n"
        "  try { [StarFs.ModSchema]::Apply($db, 'cs-fail', [string[]]@('CREATE TABLE cs_ok (id INTEGER); CREATE TABLE cs_bad (this is not valid sql')); Say 'fail' 'no throw' } catch { Say 'fail' (Msg $_) }\n"
        "  Say 'csok' $db.Scalar(\"SELECT count(*) FROM sqlite_master WHERE name = 'cs_ok'\", $none)\n"
        "  [void]$db.Exec('INSERT INTO glue_t VALUES(?)', [object[]]@(555))\n"
        "  [IO.File]::WriteAllText((Join-Path $flags 'ready.flag'), 'x')\n"
        "  $go = Join-Path $flags 'go.flag'; $until = [DateTime]::UtcNow.AddSeconds(60)\n"
        "  while (-not (Test-Path $go)) { if ([DateTime]::UtcNow -gt $until) { throw 'no go.flag' }; Start-Sleep -Milliseconds 20 }\n"
        "  Say 'seen' $db.Scalar('SELECT count(*) FROM glue_t WHERE a = ?', [object[]]@(777))\n"
        "} finally { $db.Dispose() }\n").arg(psQuote(root), psQuote(flags));
    QProcess p2;
    p2.start(ps, psArgs(phase2));
    QVERIFY(p2.waitForStarted(10000));
    p2.closeWriteChannel();
    QElapsedTimer wait;
    wait.start();
    while (!QFileInfo::exists(flags + "/ready.flag") && p2.state() == QProcess::Running && wait.elapsed() < 90000)
        p2.waitForFinished(20);
    const bool ready = QFileInfo::exists(flags + "/ready.flag");
    std::string cppNewer, cppFail, hotWhat;
    int64_t seenCs = -1;
    if (ready) {
        // The live file (C# holds it open and just wrote its -wal): the importer guard refuses it.
        hotWhat = catchDb([&] { Db::openCopyReadOnly(peerPath); }).what;
        auto db = Db::open(peerPath);
        seenCs = db->queryRead("SELECT count(*) AS n FROM glue_t WHERE a = 555").at(0).getInt("n");
        db->exec("INSERT INTO glue_t VALUES(?)", {777});
        cppNewer = catchStd([&] {
            ModSchema::apply(*db, "glue-test", {"CREATE TABLE glue_t(a INTEGER)", "INSERT INTO glue_t VALUES(1)"});
        });
        cppFail = catchStd([&] {
            ModSchema::apply(*db, "cs-fail", {"CREATE TABLE cs_ok (id INTEGER); CREATE TABLE cs_bad (this is not valid sql"});
        });
        QFile go(flags + "/go.flag");
        QVERIFY(go.open(QIODevice::WriteOnly));
        go.close();
        QVERIFY(p2.waitForFinished(60000));
        QCOMPARE(count(*db, "SELECT count(*) FROM sqlite_master WHERE name = 'cs_ok'"), 0);
    } else {
        p2.kill();
        p2.waitForFinished(5000);
    }
    const QByteArray out2 = p2.readAllStandardOutput();
    QVERIFY2(ready && p2.exitCode() == 0, (out2 + p2.readAllStandardError()).left(3000).constData());
    const auto cs2 = parseSay(out2);
    QCOMPARE(one(cs2, "ver3"), QStringLiteral("3"));      // the C# ModSchema reads the version C++ wrote
    QCOMPARE(one(cs2, "cpprows"), QStringLiteral("1"));
    QCOMPARE(one(cs2, "after"), QStringLiteral("3"));     // and applying the same steps there is a no-op
    QCOMPARE(one(cs2, "cpprows2"), QStringLiteral("1"));
    QCOMPARE(qs(cppNewer), one(cs2, "newer"));            // "glue-test schema version 3 is newer than this build (2 steps)"
    // The failing-step text differs only by C#'s "sqlite error N: " prefix (C++ carries better-sqlite3's text).
    QCOMPARE(qs(cppFail), QString(one(cs2, "fail")).replace(QStringLiteral("sqlite error 1: "), QString()));
    QVERIFY2(qs(cppFail).startsWith("cs-fail schema step 1 failed: "), cppFail.c_str());
    QCOMPARE(one(cs2, "csok"), QStringLiteral("0"));
    QVERIFY2(hotWhat.find("looks live") != std::string::npos, hotWhat.c_str());
    QCOMPARE(seenCs, int64_t(1));                         // the C# write, seen by a C++ reader during its session
    QCOMPARE(one(cs2, "seen"), QStringLiteral("1"));      // the C++ write, seen by the still-open C# connection

    auto fin = Db::open(peerPath);
    SEQ(fin->scalar("PRAGMA integrity_check").asString(), "ok");
    SEQ(json::stringify(fin->query("SELECT a FROM glue_t ORDER BY a")), "[{\"a\":1},{\"a\":99},{\"a\":555},{\"a\":777}]");
}

QTEST_GUILESS_MAIN(TestAnfsCoreDb)
#include "test_anfs_core_db.moc"
