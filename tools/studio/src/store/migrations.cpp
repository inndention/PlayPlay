#include "store/migrations.h"

#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "core/json.h"
#include "core/log.h"

// AUTORCC compiles tools/studio/resources.qrc into the static PlayPlayStudioLib, where nothing references the
// initializer and the linker would drop it (see studioservice.cpp). Q_INIT_RESOURCE must run from the global namespace;
// calling it more than once is harmless.
static void studioStoreInitResources() { Q_INIT_RESOURCE(resources); }

namespace studio::store {
namespace {

constexpr auto kCat = u"store";

bool isWordChar(QChar c) { return c.isLetterOrNumber() || c == u'_'; }

QString sqlError(const QSqlQuery& q) {
    const QSqlError e = q.lastError();
    QString t = e.databaseText().trimmed();
    if (t.isEmpty()) t = e.text().trimmed();
    return t.isEmpty() ? QStringLiteral("unknown SQL error") : t;
}

// Every step after core/1 goes here, in order, with the next version number. Never change a released step.
QList<MigrationStep> coreLaterSteps() {
    return {};
}

bool tableExists(QSqlDatabase& db, const QString& name) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?"));
    q.addBindValue(name);
    return q.exec() && q.next();
}

} // namespace

MigrationModule coreModule() {
    studioStoreInitResources();
    MigrationModule m;
    m.name = QStringLiteral("core");
    QFile f(QStringLiteral(":/studio/store/schema.sql"));
    if (!f.open(QIODevice::ReadOnly)) return m;
    m.steps.append({1, QStringLiteral("base schema"), QString::fromUtf8(f.readAll())});
    m.steps.append(coreLaterSteps());
    return m;
}

int coreSchemaVersion() { return 1 + int(coreLaterSteps().size()); }

bool splitStatements(QStringView sql, QStringList* out, QString* error) {
    out->clear();
    QString cur, word;
    bool pendingSpace = false;
    QStringList head;                      // the statement's first words, upper case (trigger detection)
    bool trigger = false, triggerEnded = false;
    int caseDepth = 0;

    auto fail = [&](const QString& why) {
        if (error) *error = why;
        out->clear();
        return false;
    };
    auto flushWord = [&] {
        if (word.isEmpty()) return;
        const QString w = word.toUpper();
        word.clear();
        if (head.size() < 3) {
            head << w;
            if (!trigger && head.size() >= 2 && head[0] == u"CREATE")
                trigger = head[1] == u"TRIGGER"
                    || (head.size() == 3 && (head[1] == u"TEMP" || head[1] == u"TEMPORARY") && head[2] == u"TRIGGER");
        }
        if (!trigger) return;
        if (w == u"CASE") ++caseDepth;
        else if (w == u"END") { if (caseDepth > 0) --caseDepth; else triggerEnded = true; }
    };
    auto put = [&](QStringView s) {
        if (pendingSpace && !cur.isEmpty()) cur += u' ';
        pendingSpace = false;
        cur += s;
    };
    auto endStatement = [&] {
        const QString s = cur.trimmed();
        if (!s.isEmpty()) out->append(s);
        cur.clear();
        word.clear();
        head.clear();
        pendingSpace = trigger = triggerEnded = false;
        caseDepth = 0;
    };

    const qsizetype n = sql.size();
    for (qsizetype i = 0; i < n; ++i) {
        const QChar c = sql[i];
        const QChar next = i + 1 < n ? sql[i + 1] : QChar();
        if (c == u'\'' || c == u'"' || c == u'`' || c == u'[') {
            flushWord();
            const QChar close = c == u'[' ? QChar(u']') : c;
            qsizetype j = i + 1;
            for (;; ++j) {
                if (j >= n) return fail(QStringLiteral("unterminated %1 quote at offset %2").arg(c).arg(i));
                if (sql[j] == close) {
                    if (c != u'[' && j + 1 < n && sql[j + 1] == close) { ++j; continue; }   // '' "" `` escapes
                    break;
                }
            }
            put(sql.mid(i, j - i + 1));
            i = j;
            continue;
        }
        if (c == u'-' && next == u'-') {
            flushWord();
            while (i < n && sql[i] != u'\n') ++i;
            pendingSpace = true;
            continue;
        }
        if (c == u'/' && next == u'*') {
            flushWord();
            const qsizetype end = sql.indexOf(u"*/", i + 2);
            if (end < 0) return fail(QStringLiteral("unterminated comment at offset %1").arg(i));
            i = end + 1;
            pendingSpace = true;
            continue;
        }
        if (c.isSpace()) {
            flushWord();
            pendingSpace = true;
            continue;
        }
        if (c == u';') {
            flushWord();
            if (trigger && !triggerEnded) {      // a statement inside the trigger body
                cur += u';';
                pendingSpace = false;
                continue;
            }
            endStatement();
            continue;
        }
        if (isWordChar(c)) word += c;
        else flushWord();
        put(sql.sliced(i, 1));
    }
    flushWord();
    if (trigger && !triggerEnded && !cur.trimmed().isEmpty())
        return fail(QStringLiteral("unterminated CREATE TRIGGER body (no END)"));
    endStatement();
    return true;
}

QString stepHash(const QStringList& statements) {
    return QString::fromLatin1(json::sha256Hex(statements.join(QStringLiteral(";\n")).toUtf8()));
}

bool migrate(QSqlDatabase& db, const QList<MigrationModule>& modules, qint64 nowMs, MigrationReport* report,
             QString* error) {
    MigrationReport local;
    MigrationReport& rep = report ? *report : local;
    rep = MigrationReport();
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        log::error(kCat, QStringLiteral("migration refused: ") + why);
        return false;
    };

    // ---- validate and prepare every step before touching the database ----
    struct Prepared { int version; QString name; QStringList statements; QString sha; };
    struct Module { QString name; QList<Prepared> steps; };
    QList<Module> mods;
    QList<MigrationModule> all;
    all << coreModule() << modules;
    static const QRegularExpression nameRe(QStringLiteral("^[a-z][a-z0-9_]{0,31}$"));
    QSet<QString> seen;
    for (int mi = 0; mi < all.size(); ++mi) {
        const MigrationModule& m = all[mi];
        if (!nameRe.match(m.name).hasMatch()) return fail(QStringLiteral("bad migration module name '%1'").arg(m.name));
        if (seen.contains(m.name)) return fail(QStringLiteral("migration module '%1' given twice").arg(m.name));
        seen.insert(m.name);
        if (m.steps.isEmpty())
            return fail(mi == 0 ? QStringLiteral("core schema resource :/studio/store/schema.sql is missing")
                                : QStringLiteral("migration module '%1' has no steps").arg(m.name));
        Module pm{m.name, {}};
        for (int si = 0; si < m.steps.size(); ++si) {
            const MigrationStep& s = m.steps[si];
            if (s.version != si + 1)
                return fail(QStringLiteral("%1: step versions must be 1..N in order (got %2 at position %3)")
                                .arg(m.name).arg(s.version).arg(si + 1));
            Prepared p{s.version, s.name, {}, {}};
            QString err;
            if (!splitStatements(s.sql, &p.statements, &err))
                return fail(QStringLiteral("%1/%2: %3").arg(m.name).arg(s.version).arg(err));
            if (p.statements.isEmpty()) return fail(QStringLiteral("%1/%2 has no statements").arg(m.name).arg(s.version));
            p.sha = stepHash(p.statements);
            pm.steps.append(p);
        }
        mods.append(pm);
    }

    QSqlQuery q(db);
    auto exec = [&](const QString& sql) { return q.exec(sql); };
    auto rollback = [&] { QSqlQuery r(db); r.exec(QStringLiteral("ROLLBACK")); };

    // ---- the database must be a Studio database (or empty) and not newer than this build ----
    if (!exec(QStringLiteral("BEGIN IMMEDIATE"))) return fail(QStringLiteral("cannot lock the database: ") + sqlError(q));
    if (!tableExists(db, QStringLiteral("schema_migrations"))) {
        QSqlQuery c(db);
        if (!c.exec(QStringLiteral("SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'")) || !c.next()) {
            const QString e = sqlError(c);
            c.finish();
            rollback();
            return fail(QStringLiteral("cannot read the schema: ") + e);
        }
        const qint64 objects = c.value(0).toLongLong();
        c.finish();
        if (objects > 0) {
            rollback();
            return fail(QStringLiteral("not a PlayPlayStudio database (it has %1 schema objects but no schema_migrations); "
                                       "refusing to write into it").arg(objects));
        }
        if (!exec(QStringLiteral("CREATE TABLE schema_migrations (module TEXT NOT NULL, version INTEGER NOT NULL, "
                                 "name TEXT NOT NULL, sha256 TEXT NOT NULL, applied_ms INTEGER NOT NULL, "
                                 "PRIMARY KEY (module, version)) STRICT, WITHOUT ROWID"))) {
            const QString e = sqlError(q);
            rollback();
            return fail(QStringLiteral("cannot create schema_migrations: ") + e);
        }
    }
    if (!exec(QStringLiteral("PRAGMA user_version")) || !q.next()) {
        const QString e = sqlError(q);
        rollback();
        return fail(QStringLiteral("cannot read user_version: ") + e);
    }
    const int userVersion = q.value(0).toInt();
    q.finish();
    if (userVersion > mods[0].steps.size()) {
        rollback();
        return fail(QStringLiteral("the database schema (core %1) is newer than this build (core %2); refusing to open it")
                        .arg(userVersion).arg(mods[0].steps.size()));
    }
    // Every recorded step of a known module must match this build.
    for (const Module& m : mods) {
        QSqlQuery a(db);
        a.prepare(QStringLiteral("SELECT version, sha256 FROM schema_migrations WHERE module = ? ORDER BY version"));
        a.addBindValue(m.name);
        if (!a.exec()) {
            const QString e = sqlError(a);
            rollback();
            return fail(QStringLiteral("cannot read schema_migrations: ") + e);
        }
        int expect = 1;
        QString bad;
        while (a.next()) {
            const int v = a.value(0).toInt();
            const QString sha = a.value(1).toString();
            if (v != expect) { bad = QStringLiteral("%1: applied versions have a gap at %2 (found %3)").arg(m.name).arg(expect).arg(v); break; }
            if (v > m.steps.size()) {
                bad = QStringLiteral("%1/%2 is applied but this build only knows %1 1..%3 (the database is newer); "
                                     "refusing to open it").arg(m.name).arg(v).arg(m.steps.size());
                break;
            }
            if (sha != m.steps[v - 1].sha) {
                bad = QStringLiteral("%1/%2 (%3) changed after it was applied to this database; add a new step instead "
                                     "of editing a released one").arg(m.name).arg(v).arg(m.steps[v - 1].name);
                break;
            }
            ++expect;
        }
        a.finish();
        if (!bad.isEmpty()) {
            rollback();
            return fail(bad);
        }
    }
    if (!exec(QStringLiteral("COMMIT"))) {
        const QString e = sqlError(q);
        rollback();
        return fail(QStringLiteral("cannot commit: ") + e);
    }

    // ---- apply pending steps, one transaction each; re-read the version inside it (another process may have won) ----
    for (const Module& m : mods) {
        for (const Prepared& s : m.steps) {
            if (!exec(QStringLiteral("BEGIN IMMEDIATE"))) return fail(QStringLiteral("cannot lock the database: ") + sqlError(q));
            QSqlQuery a(db);
            a.prepare(QStringLiteral("SELECT count(*) FROM schema_migrations WHERE module = ? AND version = ?"));
            a.addBindValue(m.name);
            a.addBindValue(s.version);
            if (!a.exec() || !a.next()) {
                const QString e = sqlError(a);
                a.finish();
                rollback();
                return fail(QStringLiteral("cannot read schema_migrations: ") + e);
            }
            const bool done = a.value(0).toLongLong() > 0;
            a.finish();
            if (done) {
                exec(QStringLiteral("COMMIT"));
                continue;
            }
            for (int i = 0; i < s.statements.size(); ++i) {
                if (!exec(s.statements[i])) {
                    const QString e = sqlError(q);
                    rollback();
                    return fail(QStringLiteral("%1/%2 (%3) failed at statement %4: %5; the step was rolled back")
                                    .arg(m.name).arg(s.version).arg(s.name).arg(i + 1).arg(e));
                }
            }
            QSqlQuery ins(db);
            ins.prepare(QStringLiteral("INSERT INTO schema_migrations (module, version, name, sha256, applied_ms) "
                                       "VALUES (?, ?, ?, ?, ?)"));
            ins.addBindValue(m.name);
            ins.addBindValue(s.version);
            ins.addBindValue(s.name.isNull() ? QString::fromLatin1("") : s.name);
            ins.addBindValue(s.sha);
            ins.addBindValue(nowMs);
            bool ok = ins.exec();
            QString e = ok ? QString() : sqlError(ins);
            if (ok && m.name == u"core") {
                ok = exec(QStringLiteral("PRAGMA user_version = %1").arg(s.version));
                if (!ok) e = sqlError(q);
            }
            if (ok) {
                ok = exec(QStringLiteral("COMMIT"));
                if (!ok) e = sqlError(q);
            }
            if (!ok) {
                rollback();
                return fail(QStringLiteral("%1/%2 (%3) could not be recorded: %4").arg(m.name).arg(s.version).arg(s.name, e));
            }
            const QString tag = QStringLiteral("%1/%2 %3").arg(m.name).arg(s.version).arg(s.name);
            rep.applied << tag;
            log::info(kCat, QStringLiteral("migration applied: ") + tag);
        }
    }
    rep.coreVersion = int(mods[0].steps.size());
    return true;
}

} // namespace studio::store
