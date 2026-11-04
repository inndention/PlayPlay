#pragma once
// Typed access to the core tables (ST-03). Writes are Write builders for the DbWriter (they carry the durability their
// rows need: spend rows and provider-accepted markers are always Durable, so a caller cannot batch a money row by
// mistake); reads take a connection of the calling thread (Db::reader() on the thread that opened the Db).
//
//   writer.submit(jobs::insert(rows), this, done);
//   writer.commitThenSend(spend::append(reserve), this, [=](const WriteResult&) { sendPaidRequest(); });
//   writer.submit(jobs::markAccepted(id, providerJobId), this, [=](const WriteResult& r) { if (r.ok) startPolling(); });
//
// Only what ST-03 and its tests need is here: projects, runs, jobs + edges, the spend ledger, events (WS replay) and
// the audit log. The spend gate (ST-11), artifacts (ST-13) and the film model (ST-21, its own module code over the same
// tables) build their logic on these and on DbWriter/Tx directly. Text columns are NOT NULL: empty values are written
// as '' (a null QString would bind NULL).
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <optional>

#include "core/money.h"
#include "store/dbwriter.h"

namespace studio::store {

struct ProjectRow {
    QString id, kind = QStringLiteral("film"), title;
    QJsonObject brief;
    money::Micro budget = 0, spent = 0;
    QString tier, status = QStringLiteral("draft");
    qint64 createdMs = 0, updatedMs = 0;
};

struct RunRow {
    QString id, projectId, phase, tier;
    QJsonObject plan;
    QString planHash, approvalId;
    money::Micro budget = 0, ceiling = 0, spent = 0;
    QString state = QStringLiteral("pending");
    qint64 startedMs = 0, finishedMs = 0;
};

struct JobRow {
    QString id, runId, filmNodeId, kind, role, lane;
    int priority = 0;
    QString orderKey;
    QStringList deps;                      // ids that must finish first; jobs::insert writes a job_edges row for each
    int indegree = -1;                     // -1 = deps.size()
    QString state = QStringLiteral("pending");
    QString model;
    QStringList fallbacks;
    money::Micro estimate = 0, actual = 0;
    QString costSource;                    // actual | estimated
    int attempts = 0;
    QString providerJobId, idemKey, inputHash;
    qint64 leaseUntilMs = 0;
    QJsonObject outputs;
    QString error;
    qint64 createdMs = 0, startedMs = 0, finishedMs = 0;
};

struct SpendRow {
    QString id;                            // empty: spend::append makes "spd_<hex>"
    qint64 tsMs = 0;
    QString projectId, runId, jobId, agentId, model;
    QString kind;                          // reserve | charge | release
    money::Micro micro = 0;                // >= 0; kind gives the direction
    QString source = QStringLiteral("estimated");   // actual | estimated
    QString generationId;
};

struct EventRow {
    qint64 seq = 0, tsMs = 0;
    QString type, projectId;
    QJsonObject payload;
};

namespace projects {
/// Insert, or replace every field but created_ms and spent_micro. Both keep their first-insert value (spent normally
/// 0): spent_micro is the booked-money counter and only spend writes (ST-11) change it, so an upsert from a stale
/// snapshot cannot undo spend booked after the snapshot was read.
Write upsert(const ProjectRow& p);
std::optional<ProjectRow> get(const QSqlDatabase& db, const QString& id);
QList<ProjectRow> list(const QSqlDatabase& db, int limit = 200);
} // namespace projects

namespace runs {
Write insert(const RunRow& r);
/// Sets the state; a non-zero finishedMs is written too. Fails when the run does not exist.
Write setState(const QString& id, const QString& state, qint64 finishedMs = 0);
std::optional<RunRow> get(const QSqlDatabase& db, const QString& id);
QList<RunRow> listByProject(const QSqlDatabase& db, const QString& projectId, int limit = 200);
} // namespace runs

namespace jobs {
/// done | failed | skipped | cancelled | unknown.
bool isTerminal(QStringView state);
/// Inserts the rows and one job_edges row (dep -> job) per dependency, all or nothing. Empty ids fail the write.
Write insert(QList<JobRow> rows);
/// Sets the state (and `error`, cleared when empty). Entering "running" stamps started_ms once; a terminal state
/// stamps finished_ms. Fails when the job does not exist.
Write setState(const QString& id, const QString& state, qint64 nowMs, const QString& error = {});
/// The provider-accepted marker (Durable): stores the provider's job id and moves the job to `state` (polling by
/// default). Commit it before the first poll: a restart then resumes polling instead of paying twice. Fails when the
/// job does not exist or `providerJobId` is empty.
Write markAccepted(const QString& id, const QString& providerJobId, const QString& state = QStringLiteral("polling"));
std::optional<JobRow> get(const QSqlDatabase& db, const QString& id);
/// Jobs of a run (all runs when runId is empty), optionally of one state, in (priority, order_key, id) order.
QList<JobRow> list(const QSqlDatabase& db, const QString& runId, const QString& state = {}, int limit = 1000);
qint64 count(const QSqlDatabase& db, const QString& runId, const QString& state = {});
/// Ids that depend on `id` (job_edges to_id), sorted.
QStringList dependents(const QSqlDatabase& db, const QString& id);
} // namespace jobs

namespace spend {
/// Appends one ledger row (Durable). Refused (the write fails, nothing is written) for an unknown kind or source, a
/// negative amount or a missing timestamp.
Write append(SpendRow row);
struct Filter {
    QString projectId, runId, jobId, kind;
    qint64 fromMs = 0;                     // ts_ms >= fromMs
    qint64 toMs = 0;                       // ts_ms < toMs (0 = open)
};
/// Sum of micro over the matching rows (exact integer arithmetic in SQLite). ok=false on a read error.
money::Micro sum(const QSqlDatabase& db, const Filter& f, bool* ok = nullptr);
QList<SpendRow> list(const QSqlDatabase& db, const Filter& f, int limit = 1000);
} // namespace spend

namespace events {
/// Appends one event; WriteResult::value is its seq.
Write append(const QString& type, const QString& projectId, const QJsonObject& payload, qint64 tsMs);
/// Events with seq > `seq`, oldest first (one project only when projectId is set).
QList<EventRow> since(const QSqlDatabase& db, qint64 seq, int limit = 500, const QString& projectId = {});
qint64 lastSeq(const QSqlDatabase& db);
} // namespace events

namespace audit {
Write append(const QString& actor, const QString& action, const QString& target, const QJsonObject& detail, qint64 tsMs);
} // namespace audit

} // namespace studio::store
