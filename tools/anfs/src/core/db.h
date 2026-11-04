// core/db.h - SQLite (vendored 3.54.0 amalgamation, raw C API) with one serialized writer connection and a small
// pool of read-only WAL readers. FROZEN (ANFS-F0); implemented by ANFS-F3 (core/db.cpp, third_party/sqlite).
//
// Connection setup: journal_mode=WAL, synchronous=NORMAL, busy_timeout=5000, foreign_keys=OFF (Node parity),
// mmap_size=256 MB, extended result codes, a per-connection prepared-statement cache. SQL uses single quotes only
// (the library is built with SQLITE_DQS=0), '?' parameters, FTS5 and JSON1 compiled in.
//
// Concurrency (anfs-cpp.md 2.7): the writer is one connection behind a mutex; tx() holds it for the whole
// transaction (BEGIN IMMEDIATE ... COMMIT, ROLLBACK when the callback throws). Keep each tx under about 250 ms: big
// ingests batch 2,000 rows per tx (CONVENTIONS.md section 1). read() runs on one of `readers` SQLITE_OPEN_READONLY
// connections, so UI polling never waits behind an ingest; a ":memory:" database has no readers and read() uses the
// writer.
//
// Values cross the API as JVal (null, Int, Double, String); BLOB columns read as String bytes. Rows are JVal objects
// whose keys follow the SELECT column order, so they serialize in Node's order (a repeated column name keeps its
// first position and the last value, like a JS object).
//
// ANFS-F3 notes (behaviour of core/db.cpp):
// - DbError::what() is sqlite3_errmsg, the text better-sqlite3 gives Node (C# prefixed "sqlite error N: "); code is
//   the primary result code, extendedCode the extended one (SQLITE_CONSTRAINT_UNIQUE ...). Misuse (parameter count,
//   several statements where one is expected, empty SQL, binding an array/object) is a DbError too.
// - Statement cache (64 per connection, LRU): prepare() of the same SQL hands back the same Stmt unless that one is
//   still stepping, then a twin, so nested iteration works. A Stmt& from prepare() lives until the outermost callback
//   on its connection returns: it is never evicted before that (the cache grows past 64 if a callback holds more).
//   run() leaves the statement done: reset() before binding it again in a loop. Statements left stepping are reset
//   when the outermost callback returns.
// - Row counts (exec, run) are 0 unless the statement changed rows (better-sqlite3 and C# rule).
// - Nested tx() joins the open transaction (C# Db.Tx, by tx depth); an inner throw that the outer callback catches
//   does not undo the inner writes. tx() inside write() begins its own transaction; if that write() callback opened
//   one by hand (BEGIN), tx() throws "cannot start a transaction within a transaction" (C#). The writer shortcuts may
//   be called inside tx(). A transaction a callback opened by hand and left open (BEGIN without COMMIT, or a script
//   that stopped at its failing statement) is rolled back when the outermost callback on that connection returns.
// - read() on a thread inside tx()/write() uses the writer (it sees the open transaction); a nested read() reuses the
//   thread's reader; otherwise it waits for a free reader. A read transaction never blocks the writer (WAL).
#pragma once

#include "core/json.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace anfs {

class DbError : public std::runtime_error {
public:
    DbError(const std::string& what, int code, int extendedCode)
        : std::runtime_error(what), code(code), extendedCode(extendedCode) {}
    int code = 0;
    int extendedCode = 0;
};

using DbParams = std::vector<JVal>;

// A prepared statement on one connection (owned by the connection's cache: never outlives it). Not thread-safe.
class Stmt {
public:
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bindNull(int index);                    // 1-based, like sqlite3_bind_*
    Stmt& bind(int index, int64_t v);
    Stmt& bind(int index, double v);              // float promotes here
    Stmt& bind(int index, std::string_view text);
    Stmt& bindBlob(int index, const void* p, size_t n);
    Stmt& bind(int index, const JVal& v);         // null/bool(0|1)/int/double/string
    // Forwarders for the natural argument types. Without them bind(i, n), bind(i, s.size()), bind(i, flag),
    // bind(i, str) and bind(i, "lit") are ambiguous (MSVC C2668: an int converts equally well to int64_t and double,
    // a std::string or const char* equally well to string_view and JVal). Any integer and bool bind as INTEGER
    // (bool 0|1; unsigned values above INT64_MAX wrap, callers never pass them); a null const char* binds NULL.
    template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
    Stmt& bind(int index, T v) { return bind(index, static_cast<int64_t>(v)); }
    Stmt& bind(int index, const std::string& text) { return bind(index, std::string_view(text)); }
    Stmt& bind(int index, const char* text) { return text ? bind(index, std::string_view(text)) : bindNull(index); }
    Stmt& bind(int index, std::nullptr_t) { return bindNull(index); }
    Stmt& bindAll(const DbParams& params);        // index 1..n
    bool step();                                  // true while a row is available; throws DbError
    int run();                                    // steps to the end; returns sqlite3_changes
    void reset();                                 // reset + clear bindings (done automatically before reuse)

    int columnCount() const;
    std::string columnName(int col) const;        // 0-based
    bool isNull(int col) const;
    int64_t i64(int col) const;
    double dbl(int col) const;
    std::string str(int col) const;
    JVal value(int col) const;
    JVal row() const;                             // object of the current row

    // Internal: statements are created by DbConn::prepare only.
    Stmt(void* stmt, void* db) : st_(stmt), db_(db) {}

private:
    void* st_ = nullptr;                          // sqlite3_stmt*
    void* db_ = nullptr;                          // sqlite3*
};

// One connection. Handed out by Db under its lock; never stored by callers beyond the callback.
class DbConn {
public:
    DbConn(const DbConn&) = delete;
    DbConn& operator=(const DbConn&) = delete;

    // One statement with parameters; returns the changed row count.
    int exec(std::string_view sql, const DbParams& params = {});
    // Several ';'-separated statements without parameters (schema steps).
    void execScript(std::string_view sql);
    JVal query(std::string_view sql, const DbParams& params = {});       // array of row objects
    JVal queryOne(std::string_view sql, const DbParams& params = {});    // first row object, or null
    JVal scalar(std::string_view sql, const DbParams& params = {});      // first column of the first row, or null
    Stmt& prepare(std::string_view sql);          // cached; reset before it is returned
    int64_t lastInsertId() const;
    int changes() const;
    bool readOnly() const { return readOnly_; }
    void* handle() const { return db_; }          // sqlite3*

    // Internal: connections are created and closed by Db only.
    DbConn(void* db, bool readOnly);
    ~DbConn();

private:
    struct Cache;
    void* db_ = nullptr;
    bool readOnly_ = false;
    std::unique_ptr<Cache> cache_;
};

class Db {
public:
    struct Options {
        int readers = 4;                          // read-only pool size (0 = reads use the writer)
        int busyTimeoutMs = 5000;
        int64_t mmapBytes = 256LL << 20;
        bool readOnly = false;                    // open everything read-only (importer, inspection)
    };

    // Opens or creates the database (creates the folder). ":memory:" is supported. Throws DbError.
    static std::shared_ptr<Db> open(const std::string& path);
    static std::shared_ptr<Db> open(const std::string& path, const Options& options);
    // Importer guard: opens a COPY read-only; refuses (DbError) when "<path>-wal" was modified in the last 60 s,
    // which would mean a live database. (ANFS-F3: the age is measured on anfs::clock(), a -wal stamped in the future
    // counts as fresh; the result has no readers, every write fails with SQLITE_READONLY, and no connection
    // checkpoints at close, so the copy's bytes stay as they were. A copy without -wal opens immutable: no sidecar
    // file is created and it can be reopened at once; a copy with a stale -wal may get a -shm, SQLite's WAL index.)
    static std::shared_ptr<Db> openCopyReadOnly(const std::string& path);
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    void tx(const std::function<void(DbConn&)>& fn);      // writer lock + transaction
    void write(const std::function<void(DbConn&)>& fn);   // writer lock, no transaction (pragmas, VACUUM)
    void read(const std::function<void(DbConn&)>& fn);    // a pooled reader (waits for a free one)

    // Writer shortcuts (each takes the writer lock for one statement).
    int exec(std::string_view sql, const DbParams& params = {});
    JVal query(std::string_view sql, const DbParams& params = {});
    JVal scalar(std::string_view sql, const DbParams& params = {});
    // Reader shortcut.
    JVal queryRead(std::string_view sql, const DbParams& params = {});

    const std::string& path() const;
    int64_t sizeBytes() const;                    // main file + -wal
    static std::string sqliteVersion();           // sqlite3_libversion()
    static bool hasFts5();

private:
    Db();
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// Per-module schema versions, byte-identical to C# FsCoreSchema.cs, so the C# and C++ binaries share one anfs.db:
//   CREATE TABLE IF NOT EXISTS _mod_schema (module TEXT PRIMARY KEY, version INTEGER NOT NULL)
// steps[i] brings the module from version i to i+1, each step in its own tx together with
//   INSERT INTO _mod_schema(module, version) VALUES(?, ?) ON CONFLICT(module) DO UPDATE SET version = excluded.version
// Errors: std::runtime_error "<module> schema version N is newer than this build (M steps)" and
// "<module> schema step N failed: <sqlite message>" (1-based; the failing step rolls back, the version stays).
// Never edit an applied step: append. (ANFS-F3: an empty module name throws std::invalid_argument("module"); a step
// may hold several ';'-separated statements; a blank step only bumps the version; apply() holds the writer lock from
// start to end, so concurrent appliers never run a step twice.)
struct ModSchema {
    static void apply(Db& db, std::string_view module, const std::vector<std::string>& steps);
    static int64_t version(Db& db, std::string_view module);   // 0 when unknown
};

} // namespace anfs
