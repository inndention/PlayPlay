// core/db.cpp - ANFS-F3: SQLite connections (vendored 3.54.0 amalgamation, raw C API), the serialized writer, the
// read-only WAL reader pool, openCopyReadOnly's hot-WAL guard and ModSchema (byte-identical to C# FsCoreSchema.cs).
//
// - Every connection is opened SQLITE_OPEN_NOMUTEX: Db serializes the writer behind its lock and lends each reader to
//   one thread at a time, so SQLite's per-connection mutex would only cost time. Memory statistics are switched off
//   once per process (SQLITE_CONFIG_MEMSTATUS), so readers on several threads do not meet on SQLite's allocator mutex.
// - Statement cache: 64 statements per connection, least recently used evicted (C# Db.MaxCache). A statement that is
//   still stepping (sqlite3_stmt_busy) is never handed out again or evicted, so nested iteration over the same SQL gets
//   a second statement instead of resetting the outer one. A statement handed out by prepare() is pinned (never
//   evicted, even idle between run()/reset()) until the outermost callback on its connection returns: the caller
//   holds that Stmt& for the whole callback.
// - When the outermost Db callback on a connection returns (endCallback): busy statements are reset (a statement left
//   mid-iteration would otherwise keep a read transaction open, pinning a stale snapshot on that reader and stopping
//   checkpoints from moving past it, so the WAL would grow), a transaction the callback opened by hand and left open
//   is rolled back (a script that stopped at its failing statement after BEGIN), and the prepare() pins end.
// - Row counts follow better-sqlite3 and C# Db.Exec: 0 unless the statement changed rows (total_changes moved).
// - Nested tx() joins the open transaction (C# Db.Tx, by tx depth, not by sqlite3_get_autocommit, so a hand-made
//   BEGIN is never joined and left uncommitted). read() on a thread that holds the writer (inside tx()/write()) uses
//   the writer, so the callback sees its own uncommitted rows, as with the C#/Node single connection.
// - openCopyReadOnly: a copy with no -wal (and no -journal) opens with immutable=1, because a plain read-only open of a
//   WAL-mode file creates an empty -wal and a -shm next to it (kept by NO_CKPT_ON_CLOSE), and that fresh -wal would
//   make the guard refuse the same copy for the next 60 s.
// - Close order: readers first, writer last; the writer's close checkpoints and removes the -wal, as C# Dispose did.
#include "core/db.h"

#include "core/clock.h"
#include "core/fsutil.h"
#include "core/nodepath.h"

#if !ANFS_HAVE_SQLITE
#error "core/db needs SQLite: tools/anfs/third_party/sqlite/sqlite3.c (ANFS_SQLITE=vendored) or the conda library (ANFS_SQLITE=conda)"
#endif
#include <sqlite3.h>

#include <atomic>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <iterator>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace anfs {

namespace {

constexpr size_t kStmtCacheMax = 64;          // C# Db.MaxCache
constexpr int64_t kHotWalMs = 60000;          // openCopyReadOnly: a -wal written in the last 60 s means a live database
constexpr const char kConnStateKey[] = "anfs.conn";   // sqlite3_set_clientdata name of ConnState

// Byte-identical to C# FsCoreSchema.cs (the C# and C++ binaries share one anfs.db).
constexpr const char kModSchemaTable[] =
    "CREATE TABLE IF NOT EXISTS _mod_schema (module TEXT PRIMARY KEY, version INTEGER NOT NULL)";
constexpr const char kModSchemaUpsert[] =
    "INSERT INTO _mod_schema(module, version) VALUES(?, ?) ON CONFLICT(module) DO UPDATE SET version = excluded.version";

sqlite3* H(void* p) { return static_cast<sqlite3*>(p); }
sqlite3_stmt* S(void* p) { return static_cast<sqlite3_stmt*>(p); }

// With extended result codes on, rc is the extended code; the primary code is its low byte. The message is
// sqlite3_errmsg, the text better-sqlite3 puts in its SqliteError (C# prefixed "sqlite error N: ").
[[noreturn]] void fail(sqlite3* db, int rc)
{
    const char* m = db ? sqlite3_errmsg(db) : nullptr;
    throw DbError(m && *m ? m : sqlite3_errstr(rc), rc & 0xff, rc);
}

[[noreturn]] void misuse(const std::string& what, int code = SQLITE_MISUSE) { throw DbError(what, code, code); }

void check(void* db, int rc)
{
    if (rc != SQLITE_OK) fail(H(db), rc);
}

void ensureInit()
{
    static std::once_flag once;
    std::call_once(once, [] {
        sqlite3_config(SQLITE_CONFIG_MEMSTATUS, 0);   // SQLITE_MISUSE when someone initialized first: harmless
        sqlite3_initialize();
    });
}

bool isBlank(std::string_view s)
{
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\f' && c != '\v') return false;
    return true;
}

int sqlLen(std::string_view sql)
{
    if (sql.size() >= static_cast<size_t>(INT_MAX)) misuse("SQL text too long", SQLITE_TOOBIG);
    return static_cast<int>(sql.size());
}

// True when nothing but separators and comments follows a prepared statement (C# OnlyTrivia accepted only
// whitespace and ';'; a trailing comment is fine here because SQLite itself decides).
bool restIsEmpty(sqlite3* db, const char* p, const char* end)
{
    while (p && p < end) {
        const char c = *p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v' || c == ';') { ++p; continue; }
        sqlite3_stmt* st = nullptr;
        const char* tail = nullptr;
        const int rc = sqlite3_prepare_v2(db, p, static_cast<int>(end - p), &st, &tail);
        if (st) sqlite3_finalize(st);
        if (rc != SQLITE_OK || st) return false;   // another statement (or garbage) follows
        if (!tail || tail <= p) return true;
        p = tail;
    }
    return true;
}

// Resets every statement of the connection that is still stepping (see the file comment).
void resetBusy(void* db)
{
    for (sqlite3_stmt* s = sqlite3_next_stmt(H(db), nullptr); s; s = sqlite3_next_stmt(H(db), s))
        if (sqlite3_stmt_busy(s)) sqlite3_reset(s);
}

// Per-connection state shared by the statement cache and Db's callback bookkeeping, which only sees the sqlite3*: it
// hangs off the connection as client data (SQLite deletes it at close). `callbacks` counts finished outermost
// callbacks; a prepare() pin taken while it had value N lasts while it still has value N.
struct ConnState {
    uint64_t callbacks = 1;
};

void deleteConnState(void* p) { delete static_cast<ConnState*>(p); }

void rollbackQuiet(sqlite3* db)
{
    if (!sqlite3_get_autocommit(db)) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
}

// The outermost Db callback on this connection returned (see the file comment).
void endCallback(void* db)
{
    resetBusy(db);
    rollbackQuiet(H(db));
    if (auto* st = static_cast<ConnState*>(sqlite3_get_clientdata(H(db), kConnStateKey))) ++st->callbacks;
}

// "file:" URI for sqlite3_open_v2(SQLITE_OPEN_URI). Only '%', '?' and '#' mean something to SQLite's URI parser in a
// path, so only they are escaped; drive letters, backslashes and UTF-8 bytes pass through (no "//" follows "file:",
// so there is no authority part).
std::string fileUri(const std::string& path, const char* query)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out = "file:";
    out.reserve(path.size() + 32);
    for (const char ch : path) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '%' || c == '?' || c == '#') {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += ch;
        }
    }
    return out + "?" + query;
}

// One-off statement for setup pragmas (not cached); returns the first column of the first row as text.
std::string runText(sqlite3* db, const char* sql)
{
    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
    if (rc != SQLITE_OK) fail(db, rc);
    std::string out;
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        if (const unsigned char* t = sqlite3_column_text(st, 0))
            out.assign(reinterpret_cast<const char*>(t), static_cast<size_t>(sqlite3_column_bytes(st, 0)));
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {}
    }
    if (rc != SQLITE_DONE) {
        const DbError e(sqlite3_errmsg(db), rc & 0xff, rc);
        sqlite3_finalize(st);
        throw e;
    }
    sqlite3_finalize(st);
    return out;
}

void runSimple(sqlite3* db, const char* sql)
{
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) fail(db, rc);
}

// C# Convert.ToInt64 of what the version column holds (a REAL rounds half to even, like Math.Round).
int64_t toInt64(const JVal& v)
{
    if (v.isInt()) return v.asInt();
    if (v.isDouble()) return static_cast<int64_t>(std::nearbyint(v.asDouble()));
    if (v.isString()) return std::strtoll(v.asString().c_str(), nullptr, 10);
    return 0;
}

// The statement a query helper used goes back to the idle state (and drops its parameter copies) on every exit.
struct ResetGuard {
    sqlite3_stmt* st;
    ~ResetGuard()
    {
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
    }
};

struct FinalizeGuard {
    sqlite3_stmt* st;
    ~FinalizeGuard() { sqlite3_finalize(st); }
};

} // namespace

// ---- Stmt --------------------------------------------------------------------------------------------------------

Stmt& Stmt::bindNull(int index)
{
    check(db_, sqlite3_bind_null(S(st_), index));
    return *this;
}

Stmt& Stmt::bind(int index, int64_t v)
{
    check(db_, sqlite3_bind_int64(S(st_), index, v));
    return *this;
}

Stmt& Stmt::bind(int index, double v)
{
    check(db_, sqlite3_bind_double(S(st_), index, v));
    return *this;
}

Stmt& Stmt::bind(int index, std::string_view text)
{
    // A null data pointer would bind NULL: an empty string stays '' (C# kept '' distinct from NULL too).
    check(db_, sqlite3_bind_text64(S(st_), index, text.data() ? text.data() : "", text.size(), SQLITE_TRANSIENT,
                                   SQLITE_UTF8));
    return *this;
}

Stmt& Stmt::bindBlob(int index, const void* p, size_t n)
{
    // An empty blob is a zero-length BLOB, not NULL (C# bound zeroblob(0) for byte[0]).
    if (n == 0 || !p) check(db_, sqlite3_bind_zeroblob(S(st_), index, 0));
    else check(db_, sqlite3_bind_blob64(S(st_), index, p, n, SQLITE_TRANSIENT));
    return *this;
}

Stmt& Stmt::bind(int index, const JVal& v)
{
    switch (v.type()) {
    case JVal::Type::Null: return bindNull(index);
    case JVal::Type::Bool: return bind(index, static_cast<int64_t>(v.asBool() ? 1 : 0));
    case JVal::Type::Int: return bind(index, static_cast<int64_t>(v.asInt()));
    case JVal::Type::Double: return bind(index, v.asDouble());
    case JVal::Type::String: return bind(index, std::string_view(v.asString()));
    default: break;
    }
    misuse("cannot bind an array or object to SQL parameter " + std::to_string(index), SQLITE_MISMATCH);
}

Stmt& Stmt::bindAll(const DbParams& params)
{
    const int need = sqlite3_bind_parameter_count(S(st_));
    if (need != static_cast<int>(params.size()))
        misuse("SQL has " + std::to_string(need) + " parameters but " + std::to_string(params.size()) +
               " were supplied", SQLITE_RANGE);
    for (size_t i = 0; i < params.size(); ++i) bind(static_cast<int>(i) + 1, params[i]);
    return *this;
}

bool Stmt::step()
{
    const int rc = sqlite3_step(S(st_));
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    fail(H(db_), rc);
}

int Stmt::run()
{
    const int64_t before = sqlite3_total_changes64(H(db_));
    while (step()) {}
    return sqlite3_total_changes64(H(db_)) == before ? 0 : static_cast<int>(sqlite3_changes64(H(db_)));
}

void Stmt::reset()
{
    sqlite3_reset(S(st_));
    sqlite3_clear_bindings(S(st_));
}

int Stmt::columnCount() const { return sqlite3_column_count(S(st_)); }

std::string Stmt::columnName(int col) const
{
    const char* n = sqlite3_column_name(S(st_), col);
    return n ? std::string(n) : std::string();
}

bool Stmt::isNull(int col) const { return sqlite3_column_type(S(st_), col) == SQLITE_NULL; }
int64_t Stmt::i64(int col) const { return sqlite3_column_int64(S(st_), col); }
double Stmt::dbl(int col) const { return sqlite3_column_double(S(st_), col); }

std::string Stmt::str(int col) const
{
    sqlite3_stmt* st = S(st_);
    switch (sqlite3_column_type(st, col)) {
    case SQLITE_NULL: return {};
    case SQLITE_BLOB: {
        const void* p = sqlite3_column_blob(st, col);
        const int n = sqlite3_column_bytes(st, col);
        return p && n > 0 ? std::string(static_cast<const char*>(p), static_cast<size_t>(n)) : std::string();
    }
    default: {
        const unsigned char* p = sqlite3_column_text(st, col);
        const int n = sqlite3_column_bytes(st, col);
        return p && n > 0 ? std::string(reinterpret_cast<const char*>(p), static_cast<size_t>(n)) : std::string();
    }
    }
}

JVal Stmt::value(int col) const
{
    sqlite3_stmt* st = S(st_);
    switch (sqlite3_column_type(st, col)) {
    case SQLITE_INTEGER: return JVal(static_cast<long long>(sqlite3_column_int64(st, col)));
    case SQLITE_FLOAT: return JVal(sqlite3_column_double(st, col));
    case SQLITE_TEXT:
    case SQLITE_BLOB: return JVal(str(col));
    default: return JVal();
    }
}

JVal Stmt::row() const
{
    // JS object semantics for repeated column names (SELECT 1 AS a, 2 AS a): first position, last value.
    const int n = columnCount();
    JObject o;
    o.reserve(static_cast<size_t>(n));
    for (int c = 0; c < n; ++c) o.set(columnName(c), value(c));
    return JVal(std::move(o));
}

// ---- DbConn ------------------------------------------------------------------------------------------------------

struct DbConn::Cache {
    struct Entry {
        std::string sql;
        sqlite3_stmt* raw = nullptr;
        std::unique_ptr<Stmt> stmt;
        uint64_t pinnedIn = 0;           // ConnState::callbacks when prepare() handed it out (0 = never)
    };
    enum class Got { Ok, Empty, Multi };

    std::list<Entry> lru;                                                   // front = most recently used
    std::unordered_multimap<std::string, std::list<Entry>::iterator> index; // SQL text -> entries (busy twins)
    ConnState* state = nullptr;          // owned by the connection; null (no pins) if registering it failed

    ~Cache()
    {
        for (Entry& e : lru) sqlite3_finalize(e.raw);
    }

    void pin(Entry& e)
    {
        if (state) e.pinnedIn = state->callbacks;
    }
    bool pinned(const Entry& e) const { return state && e.pinnedIn == state->callbacks; }

    // A cached, idle statement for sql (reset and unbound), or a newly prepared one. Empty SQL and SQL with more than
    // one statement are reported through got (nothing is cached for them).
    Entry* get(sqlite3* db, std::string_view sql, Got& got)
    {
        got = Got::Ok;
        if (sql.empty()) {                         // also a null data pointer, which SQLite answers with MISUSE
            got = Got::Empty;
            return nullptr;
        }
        std::string key(sql);
        const auto range = index.equal_range(key);
        for (auto it = range.first; it != range.second; ++it) {
            const auto e = it->second;
            if (sqlite3_stmt_busy(e->raw)) continue;   // an outer caller is still iterating it
            lru.splice(lru.begin(), lru, e);
            sqlite3_reset(e->raw);
            sqlite3_clear_bindings(e->raw);
            return &*e;
        }
        sqlite3_stmt* st = nullptr;
        const char* tail = nullptr;
        const int rc = sqlite3_prepare_v3(db, sql.data(), sqlLen(sql), SQLITE_PREPARE_PERSISTENT, &st, &tail);
        if (rc != SQLITE_OK) {
            if (st) sqlite3_finalize(st);
            fail(db, rc);
        }
        if (!st) {
            got = Got::Empty;
            return nullptr;
        }
        if (!restIsEmpty(db, tail, sql.data() + sql.size())) {
            sqlite3_finalize(st);
            got = Got::Multi;
            return nullptr;
        }
        lru.push_front(Entry{key, st, std::make_unique<Stmt>(st, db)});
        index.emplace(std::move(key), lru.begin());
        evict();
        return &lru.front();
    }

    void evict()
    {
        while (lru.size() > kStmtCacheMax) {
            auto victim = lru.end();
            for (auto it = std::prev(lru.end()); it != lru.begin(); --it)   // never the entry just added (front)
                if (!sqlite3_stmt_busy(it->raw) && !pinned(*it)) { victim = it; break; }
            if (victim == lru.end()) return;   // all stepping or held: grow rather than pull a statement away
            const auto range = index.equal_range(victim->sql);
            for (auto it = range.first; it != range.second; ++it)
                if (it->second == victim) { index.erase(it); break; }
            sqlite3_finalize(victim->raw);
            lru.erase(victim);
        }
    }

    Entry& single(sqlite3* db, std::string_view sql)
    {
        Got got;
        Entry* e = get(db, sql, got);
        if (got == Got::Empty) misuse("empty SQL");
        if (got == Got::Multi)
            misuse("multiple statements are not allowed here (parameters, query, prepare); use execScript for scripts");
        return *e;
    }
};

DbConn::DbConn(void* db, bool readOnly) : db_(db), readOnly_(readOnly), cache_(std::make_unique<Cache>())
{
    // SQLite owns the ConnState from here (it deletes it at close, or at once when it cannot store it).
    if (db_ && sqlite3_set_clientdata(H(db_), kConnStateKey, new ConnState(), deleteConnState) == SQLITE_OK)
        cache_->state = static_cast<ConnState*>(sqlite3_get_clientdata(H(db_), kConnStateKey));
}

DbConn::~DbConn()
{
    cache_.reset();                      // finalizes every cached statement first
    if (db_) sqlite3_close_v2(H(db_));
}

int DbConn::exec(std::string_view sql, const DbParams& params)
{
    sqlite3* db = H(db_);
    Cache::Got got;
    Cache::Entry* e = cache_->get(db, sql, got);
    if (!e) {
        if (!params.empty()) {
            if (got == Cache::Got::Empty) misuse("empty SQL");
            misuse("multiple statements are not allowed with parameters; use execScript for scripts");
        }
        if (got == Cache::Got::Empty) return 0;
        // C# Db.Exec without parameters ran scripts: keep that for code ported from it.
        const int64_t before = sqlite3_total_changes64(db);
        execScript(sql);
        return sqlite3_total_changes64(db) == before ? 0 : static_cast<int>(sqlite3_changes64(db));
    }
    const ResetGuard g{e->raw};
    e->stmt->bindAll(params);
    return e->stmt->run();
}

void DbConn::execScript(std::string_view sql)
{
    sqlite3* db = H(db_);
    const char* p = sql.data();
    const char* const end = p + sql.size();
    sqlLen(sql);
    while (p && p < end) {
        sqlite3_stmt* st = nullptr;
        const char* tail = nullptr;
        const int rc = sqlite3_prepare_v2(db, p, static_cast<int>(end - p), &st, &tail);
        if (rc != SQLITE_OK) fail(db, rc);
        if (!st) break;                  // only separators and comments were left
        const FinalizeGuard g{st};
        if (sqlite3_bind_parameter_count(st) != 0) misuse("SQL has parameters but none were supplied", SQLITE_RANGE);
        for (;;) {
            const int r = sqlite3_step(st);
            if (r == SQLITE_ROW) continue;
            if (r == SQLITE_DONE) break;
            fail(db, r);
        }
        if (!tail || tail <= p) break;
        p = tail;
    }
}

JVal DbConn::query(std::string_view sql, const DbParams& params)
{
    Cache::Entry& e = cache_->single(H(db_), sql);
    const ResetGuard g{e.raw};
    Stmt& s = *e.stmt;
    s.bindAll(params);
    // Column names once per call; a repeated name takes the JS object rule (first position, last value).
    const int n = s.columnCount();
    std::vector<std::string> names(static_cast<size_t>(n));
    bool unique = true;
    for (int c = 0; c < n; ++c) {
        names[c] = s.columnName(c);
        for (int k = 0; k < c && unique; ++k) unique = names[k] != names[c];
    }
    JVal::Array rows;
    while (s.step()) {
        JObject o;
        o.reserve(static_cast<size_t>(n));
        for (int c = 0; c < n; ++c) {
            if (unique) o.append(names[c], s.value(c));
            else o.set(names[c], s.value(c));
        }
        rows.emplace_back(std::move(o));
    }
    return JVal(std::move(rows));
}

JVal DbConn::queryOne(std::string_view sql, const DbParams& params)
{
    Cache::Entry& e = cache_->single(H(db_), sql);
    const ResetGuard g{e.raw};
    e.stmt->bindAll(params);
    return e.stmt->step() ? e.stmt->row() : JVal();
}

JVal DbConn::scalar(std::string_view sql, const DbParams& params)
{
    Cache::Entry& e = cache_->single(H(db_), sql);
    const ResetGuard g{e.raw};
    e.stmt->bindAll(params);
    if (!e.stmt->step() || e.stmt->columnCount() == 0) return JVal();
    return e.stmt->value(0);
}

Stmt& DbConn::prepare(std::string_view sql)
{
    Cache::Entry& e = cache_->single(H(db_), sql);
    cache_->pin(e);                      // the caller holds the Stmt& until its callback returns
    return *e.stmt;
}

int64_t DbConn::lastInsertId() const { return sqlite3_last_insert_rowid(H(db_)); }
int DbConn::changes() const { return static_cast<int>(sqlite3_changes64(H(db_))); }

// ---- Db ----------------------------------------------------------------------------------------------------------

struct Db::Impl {
    std::string path;                    // full path, or ":memory:"
    bool memory = false;
    Options opt;

    std::recursive_mutex wmu;            // the writer; recursive so tx()/exec()/read() nest on one thread
    std::atomic<std::thread::id> wOwner{};
    int wDepth = 0;                      // guarded by wmu
    int txDepth = 0;                     // guarded by wmu: tx() callbacks running (C# Db.txDepth)
    std::unique_ptr<DbConn> writer;

    std::mutex rmu;                      // the reader pool
    std::condition_variable rcv;
    std::vector<std::unique_ptr<DbConn>> readers;
    std::vector<DbConn*> idle;

    // The writer lock; the outermost section cleans up after the callback on the writer (endCallback).
    struct WLock {
        Impl& d;
        std::unique_lock<std::recursive_mutex> lk;
        explicit WLock(Impl& impl) : d(impl), lk(impl.wmu)
        {
            if (d.wDepth++ == 0) d.wOwner.store(std::this_thread::get_id());
        }
        ~WLock()
        {
            if (d.wDepth == 1) {
                endCallback(d.writer->handle());
                d.wOwner.store(std::thread::id());
            }
            --d.wDepth;
        }
        bool outermost() const { return d.wDepth == 1; }
    };

    // The reader this thread already holds for this Db (a nested read() reuses it rather than taking a second one).
    struct Held {
        const Impl* db;
        DbConn* conn;
    };
    static std::vector<Held>& held()
    {
        thread_local std::vector<Held> h;
        return h;
    }

    struct ReaderLease {
        Impl& d;
        DbConn* conn = nullptr;
        bool nested = false;
        explicit ReaderLease(Impl& impl) : d(impl)
        {
            for (const Held& h : held())
                if (h.db == &d) { conn = h.conn; nested = true; return; }
            std::unique_lock<std::mutex> lk(d.rmu);
            d.rcv.wait(lk, [&] { return !d.idle.empty(); });
            conn = d.idle.back();
            d.idle.pop_back();
            lk.unlock();
            held().push_back(Held{&d, conn});
        }
        ~ReaderLease()
        {
            if (nested) return;
            endCallback(conn->handle());
            auto& h = held();
            for (auto it = h.begin(); it != h.end(); ++it)
                if (it->db == &d) { h.erase(it); break; }
            {
                std::lock_guard<std::mutex> lk(d.rmu);
                d.idle.push_back(conn);
            }
            d.rcv.notify_one();
        }
    };

    // immutable (read-only only): SQLite reads the main file alone, with no locks and no -wal/-shm/-journal files.
    static std::unique_ptr<DbConn> connect(const std::string& path, bool readOnly, bool memory, const Options& opt,
                                           bool immutable = false)
    {
        int flags = (readOnly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)) |
                    SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE;
        std::string name = path;
        if (immutable && readOnly) {
            name = fileUri(path, "mode=ro&immutable=1");
            flags |= SQLITE_OPEN_URI;
        }
        sqlite3* h = nullptr;
        const int rc = sqlite3_open_v2(name.c_str(), &h, flags, nullptr);
        if (rc != SQLITE_OK) {
            std::string msg = h ? sqlite3_errmsg(h) : sqlite3_errstr(rc);
            sqlite3_close_v2(h);
            throw DbError(msg + ": " + path, rc & 0xff, rc);
        }
        auto conn = std::make_unique<DbConn>(h, readOnly);   // owns h from here (closes it if setup throws)
        sqlite3_extended_result_codes(h, 1);
        sqlite3_busy_timeout(h, opt.busyTimeoutMs);
        // Double-quoted strings are identifiers only (also with the conda fallback, built with DQS on).
        sqlite3_db_config(h, SQLITE_DBCONFIG_DQS_DML, 0, nullptr);
        sqlite3_db_config(h, SQLITE_DBCONFIG_DQS_DDL, 0, nullptr);
        // A read-only connection never checkpoints at close (it cannot write; and an importer must leave the copy as
        // it found it).
        if (readOnly) sqlite3_db_config(h, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, nullptr);
        if (!readOnly && !memory) runText(h, "PRAGMA journal_mode=WAL");
        runSimple(h, "PRAGMA synchronous=NORMAL");
        runSimple(h, "PRAGMA foreign_keys=OFF");
        runSimple(h, ("PRAGMA mmap_size=" + std::to_string(opt.mmapBytes < 0 ? 0 : opt.mmapBytes)).c_str());
        return conn;
    }

    static std::shared_ptr<Db> openDb(const std::string& path, const Options& options, bool immutable);
};

Db::Db() : d_(std::make_unique<Impl>()) {}

Db::~Db()
{
    if (!d_) return;
    d_->idle.clear();
    d_->readers.clear();                 // readers first: the writer closes last and checkpoints the -wal away
    d_->writer.reset();
}

std::shared_ptr<Db> Db::open(const std::string& path) { return open(path, Options()); }

std::shared_ptr<Db> Db::open(const std::string& path, const Options& options)
{
    return Impl::openDb(path, options, false);
}

std::shared_ptr<Db> Db::Impl::openDb(const std::string& path, const Options& options, bool immutable)
{
    ensureInit();
    if (path.empty()) misuse("empty database path", SQLITE_CANTOPEN);
    std::shared_ptr<Db> db(new Db());
    Impl& d = *db->d_;
    d.opt = options;
    d.memory = path == ":memory:";
    if (d.memory) {
        d.path = path;
        d.opt.readers = 0;               // every :memory: connection is its own database
    } else {
        d.path = fs::fullPath(path);
        if (d.path.empty()) misuse("cannot resolve the database path: " + path, SQLITE_CANTOPEN);
        if (!options.readOnly) {
            const std::string dir = nodepath::dirname(d.path);
            std::string err;
            if (!dir.empty() && !fs::isDir(dir) && !fs::makeDirs(dir, &err))
                misuse("cannot create the database folder: " + err, SQLITE_CANTOPEN);
        }
    }
    d.writer = Impl::connect(d.path, options.readOnly, d.memory, d.opt, immutable && !d.memory);
    if (!d.memory && d.opt.readers > 0) {
        // Readers only help in WAL mode; with a rollback journal (a file system without shared memory) a reader's
        // SHARED lock would stall the writer's commit, so reads go through the writer instead.
        const std::string mode = runText(H(d.writer->handle()), "PRAGMA journal_mode");
        if (mode != "wal" && !options.readOnly) d.opt.readers = 0;
    }
    for (int i = 0; i < d.opt.readers; ++i) {
        d.readers.push_back(Impl::connect(d.path, true, false, d.opt));
        d.idle.push_back(d.readers.back().get());
    }
    return db;
}

std::shared_ptr<Db> Db::openCopyReadOnly(const std::string& path)
{
    ensureInit();
    const std::string full = path.empty() ? std::string() : fs::fullPath(path);
    if (full.empty() || !fs::isFile(full)) misuse("unable to open database file: " + path, SQLITE_CANTOPEN);
    const fs::FileStat wal = fs::stat(full + "-wal");
    if (wal.exists) {
        const int64_t age = anfs::nowMs() - wal.mtimeMs;   // a -wal stamped in the future counts as fresh
        if (age < kHotWalMs)
            misuse("refusing to open " + full + ": its -wal was modified " + std::to_string(age < 0 ? 0 : age / 1000) +
                       " s ago, so the database looks live (open a copy that has been idle for 60 s)",
                   SQLITE_BUSY);
    }
    Options o;
    o.readers = 0;
    o.readOnly = true;
    // No -wal (a copy taken after a clean shutdown) and no hot -journal: everything is in the main file, so open it
    // immutable. A plain read-only open of a WAL-mode file would create an empty -wal and a -shm beside the copy, and
    // that fresh -wal would make this guard refuse the same copy for the next 60 s. With a (stale) -wal the normal
    // read-only open is needed to see its pages: SQLite then reads the -wal and keeps its WAL index in a -shm (created
    // if missing), and never writes the main file or the -wal.
    const bool immutable = !wal.exists && !fs::stat(full + "-journal").exists;
    return Impl::openDb(full, o, immutable);
}

void Db::tx(const std::function<void(DbConn&)>& fn)
{
    Impl::WLock lk(*d_);
    DbConn& c = *d_->writer;
    sqlite3* h = H(c.handle());
    if (d_->txDepth > 0) {               // nested: join the open transaction (C# Db.Tx)
        fn(c);
        return;
    }
    // A transaction the callback of an enclosing write() opened by hand is not joined (it would never be committed):
    // BEGIN fails with "cannot start a transaction within a transaction", as C# Db.Tx did.
    runSimple(h, "BEGIN IMMEDIATE");
    ++d_->txDepth;
    try {
        fn(c);
    } catch (...) {
        --d_->txDepth;
        if (lk.outermost()) resetBusy(h);
        rollbackQuiet(h);
        throw;
    }
    --d_->txDepth;
    // An abandoned write statement would make COMMIT fail ("SQL statements in progress"); with no outer callback
    // nothing can still be iterating, so clear them first.
    if (lk.outermost()) resetBusy(h);
    const int rc = sqlite3_exec(h, "COMMIT", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        const DbError e(sqlite3_errmsg(h), rc & 0xff, rc);
        rollbackQuiet(h);
        throw e;
    }
}

void Db::write(const std::function<void(DbConn&)>& fn)
{
    Impl::WLock lk(*d_);
    fn(*d_->writer);
}

void Db::read(const std::function<void(DbConn&)>& fn)
{
    // Inside tx()/write() on this thread, or without readers: the writer (sees this thread's uncommitted rows).
    if (d_->readers.empty() || d_->wOwner.load() == std::this_thread::get_id()) {
        Impl::WLock lk(*d_);
        fn(*d_->writer);
        return;
    }
    Impl::ReaderLease lease(*d_);
    fn(*lease.conn);
}

int Db::exec(std::string_view sql, const DbParams& params)
{
    int n = 0;
    write([&](DbConn& c) { n = c.exec(sql, params); });
    return n;
}

JVal Db::query(std::string_view sql, const DbParams& params)
{
    JVal r;
    write([&](DbConn& c) { r = c.query(sql, params); });
    return r;
}

JVal Db::scalar(std::string_view sql, const DbParams& params)
{
    JVal r;
    write([&](DbConn& c) { r = c.scalar(sql, params); });
    return r;
}

JVal Db::queryRead(std::string_view sql, const DbParams& params)
{
    JVal r;
    read([&](DbConn& c) { r = c.query(sql, params); });
    return r;
}

const std::string& Db::path() const { return d_->path; }

int64_t Db::sizeBytes() const
{
    if (d_->memory) return 0;
    int64_t n = 0;
    for (const std::string& f : {d_->path, d_->path + "-wal"}) {
        const fs::FileStat st = fs::stat(f);
        if (st.exists && !st.isDir) n += st.size;
    }
    return n;
}

std::string Db::sqliteVersion() { return sqlite3_libversion(); }

bool Db::hasFts5()
{
    static const bool has = [] {
        ensureInit();
        sqlite3* h = nullptr;
        bool ok = false;
        if (sqlite3_open_v2(":memory:", &h, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                            nullptr) == SQLITE_OK)
            ok = sqlite3_exec(h, "CREATE VIRTUAL TABLE temp.anfs_probe USING fts5(x)", nullptr, nullptr, nullptr) ==
                 SQLITE_OK;
        sqlite3_close_v2(h);
        return ok;
    }();
    return has;
}

// ---- ModSchema (C# FsCoreSchema.cs) ------------------------------------------------------------------------------

void ModSchema::apply(Db& db, std::string_view module, const std::vector<std::string>& steps)
{
    if (module.empty()) throw std::invalid_argument("module");
    const std::string mod(module);
    const int64_t n = static_cast<int64_t>(steps.size());
    // The whole run holds the writer lock, so two threads applying the same module never run a step twice (C# took
    // the lock per call). Each step is still its own transaction together with its version bump.
    db.write([&](DbConn& c) {
        c.exec(kModSchemaTable);
        const int64_t ver = toInt64(c.scalar("SELECT version FROM _mod_schema WHERE module = ?", {JVal(mod)}));
        if (ver > n)
            throw std::runtime_error(mod + " schema version " + std::to_string(ver) + " is newer than this build (" +
                                     std::to_string(n) + " steps)");
        for (int64_t i = ver; i < n; ++i) {
            try {
                db.tx([&](DbConn& t) {
                    const std::string& step = steps[static_cast<size_t>(i)];
                    if (!isBlank(step)) t.execScript(step);
                    t.exec(kModSchemaUpsert, {JVal(mod), JVal(static_cast<long long>(i + 1))});
                });
            } catch (const std::exception& e) {
                throw std::runtime_error(mod + " schema step " + std::to_string(i + 1) + " failed: " + e.what());
            }
        }
    });
}

int64_t ModSchema::version(Db& db, std::string_view module)
{
    int64_t v = 0;
    db.read([&](DbConn& c) {
        if (c.scalar("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = '_mod_schema'").isNull()) return;
        v = toInt64(c.scalar("SELECT version FROM _mod_schema WHERE module = ?", {JVal(std::string(module))}));
    });
    return v;
}

} // namespace anfs
