#pragma once
// Schema migrations of studio.db (ST-03).
//
// A migration module is a named, ordered list of steps (versions 1..N, contiguous). The "core" module is the Studio
// schema: step 1 is store/schema.sql (embedded through tools/studio/resources.qrc), later steps live in coreModule().
// Other modules (a studio-film table set, a test fixture) pass their own MigrationModule to Db::open(), so a lane that
// does not own store/ can still add tables without editing it.
//
// Every applied step is a row of schema_migrations (module, version, name, sha256, applied_ms); PRAGMA user_version
// mirrors the core version for tools such as the sqlite3 shell. The sha256 is taken over the step's statements with
// comments removed and spacing collapsed, so editing a comment is harmless, while editing a released statement is
// caught: such a database refuses to open instead of running code against a schema it does not match.
//
// Opening refuses (fail closed, nothing written):
//   - a database whose core or module version is newer than this build knows (no downgrades),
//   - a step whose statements changed since it was applied,
//   - gaps in the applied versions (a damaged schema_migrations),
//   - a non-empty database without schema_migrations (not a Studio database: --data pointed somewhere else).
// Each pending step runs in its own BEGIN IMMEDIATE transaction together with its schema_migrations row, so a failing
// step leaves the database exactly at the previous version.
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

class QSqlDatabase;

namespace studio::store {

struct MigrationStep {
    int version = 0;                       // 1..N within its module, contiguous
    QString name;                          // short description ("base schema", "jobs.accepted_ms")
    QString sql;                           // one or more statements separated by ';' (triggers allowed)
};

struct MigrationModule {
    QString name;                          // [a-z][a-z0-9_]{0,31}; "core" is the Studio schema
    QList<MigrationStep> steps;
};

struct MigrationReport {
    int coreVersion = 0;                   // after migrate()
    QStringList applied;                   // "core/1 base schema", in the order applied by this call
};

/// The Studio schema module: step 1 = schema.sql from qrc (":/studio/store/schema.sql"). An empty step list means the
/// resource is missing (a build problem); migrate() then refuses.
MigrationModule coreModule();
/// The highest core version this build knows.
int coreSchemaVersion();

/// Brings `db` (an open read-write connection on the calling thread) up to date for `core` + `modules`. False with
/// `error` set on any refusal or failure; steps applied before a failing one stay applied.
bool migrate(QSqlDatabase& db, const QList<MigrationModule>& modules, qint64 nowMs, MigrationReport* report,
             QString* error);

/// Splits SQL text into single statements (the Qt SQLite driver runs one statement per exec). Comments are removed and
/// whitespace outside quotes collapses to one space; ';' inside '...', "...", `...`, [...] and inside a
/// CREATE TRIGGER ... BEGIN ... END body (CASE ... END nesting included) does not split. Empty statements are dropped.
/// False (and `error`) on an unterminated quote, comment or trigger body.
bool splitStatements(QStringView sql, QStringList* out, QString* error = nullptr);
/// sha256 hex of a step's statements as splitStatements() normalises them (joined by ";\n").
QString stepHash(const QStringList& statements);

} // namespace studio::store
