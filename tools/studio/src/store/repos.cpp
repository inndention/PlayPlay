#include "store/repos.h"

#include <QJsonDocument>
#include <QSet>
#include <QSqlQuery>
#include <QVariant>

#include "core/ids.h"
#include "core/json.h"

namespace studio::store {
namespace {

// NOT NULL text columns: a null QString would bind NULL, so empty values are written as ''.
QVariant txt(const QString& s) {
    static const QString empty = QStringLiteral("");
    return s.isNull() ? QVariant(empty) : QVariant(s);
}
QString jsonText(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }
QString jsonText(const QStringList& l) {
    return QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(l)).toJson(QJsonDocument::Compact));
}
QJsonObject objectOf(const QVariant& v) { return json::parseObject(v.toString().toUtf8()); }
QStringList listOf(const QVariant& v) {
    QStringList out;
    const QJsonValue j = json::parse(v.toString().toUtf8());
    for (const QJsonValue& e : j.toArray()) out << e.toString();
    return out;
}

// A prepared, executed read on the caller's connection (inactive on error).
QSqlQuery read(const QSqlDatabase& db, const QString& sql, const QVariantList& binds) {
    QSqlQuery q(db);
    q.setForwardOnly(true);
    if (!q.prepare(sql)) return q;
    for (int i = 0; i < binds.size(); ++i) q.bindValue(i, binds[i]);
    q.exec();
    return q;
}

const QString kProjectCols = QStringLiteral(
    "id, kind, title, brief_json, budget_micro, spent_micro, tier, status, created_ms, updated_ms");
ProjectRow projectFrom(const QSqlQuery& q) {
    ProjectRow p;
    p.id = q.value(0).toString();
    p.kind = q.value(1).toString();
    p.title = q.value(2).toString();
    p.brief = objectOf(q.value(3));
    p.budget = q.value(4).toLongLong();
    p.spent = q.value(5).toLongLong();
    p.tier = q.value(6).toString();
    p.status = q.value(7).toString();
    p.createdMs = q.value(8).toLongLong();
    p.updatedMs = q.value(9).toLongLong();
    return p;
}

const QString kRunCols = QStringLiteral(
    "id, project_id, phase, tier, plan_json, plan_hash, approval_id, budget_micro, ceiling_micro, spent_micro, state, "
    "started_ms, finished_ms");
RunRow runFrom(const QSqlQuery& q) {
    RunRow r;
    r.id = q.value(0).toString();
    r.projectId = q.value(1).toString();
    r.phase = q.value(2).toString();
    r.tier = q.value(3).toString();
    r.plan = objectOf(q.value(4));
    r.planHash = q.value(5).toString();
    r.approvalId = q.value(6).toString();
    r.budget = q.value(7).toLongLong();
    r.ceiling = q.value(8).toLongLong();
    r.spent = q.value(9).toLongLong();
    r.state = q.value(10).toString();
    r.startedMs = q.value(11).toLongLong();
    r.finishedMs = q.value(12).toLongLong();
    return r;
}

const QString kJobCols = QStringLiteral(
    "id, run_id, film_node_id, kind, role, lane, priority, order_key, deps_json, indegree, state, model, fallbacks_json, "
    "estimate_micro, actual_micro, cost_source, attempts, provider_job_id, idem_key, input_hash, lease_until_ms, "
    "outputs_json, error, created_ms, started_ms, finished_ms");
JobRow jobFrom(const QSqlQuery& q) {
    JobRow j;
    j.id = q.value(0).toString();
    j.runId = q.value(1).toString();
    j.filmNodeId = q.value(2).toString();
    j.kind = q.value(3).toString();
    j.role = q.value(4).toString();
    j.lane = q.value(5).toString();
    j.priority = q.value(6).toInt();
    j.orderKey = q.value(7).toString();
    j.deps = listOf(q.value(8));
    j.indegree = q.value(9).toInt();
    j.state = q.value(10).toString();
    j.model = q.value(11).toString();
    j.fallbacks = listOf(q.value(12));
    j.estimate = q.value(13).toLongLong();
    j.actual = q.value(14).toLongLong();
    j.costSource = q.value(15).toString();
    j.attempts = q.value(16).toInt();
    j.providerJobId = q.value(17).toString();
    j.idemKey = q.value(18).toString();
    j.inputHash = q.value(19).toString();
    j.leaseUntilMs = q.value(20).toLongLong();
    j.outputs = objectOf(q.value(21));
    j.error = q.value(22).toString();
    j.createdMs = q.value(23).toLongLong();
    j.startedMs = q.value(24).toLongLong();
    j.finishedMs = q.value(25).toLongLong();
    return j;
}

const QString kSpendCols = QStringLiteral(
    "id, ts_ms, project_id, run_id, job_id, agent_id, model, kind, micro, source, generation_id");
SpendRow spendFrom(const QSqlQuery& q) {
    SpendRow s;
    s.id = q.value(0).toString();
    s.tsMs = q.value(1).toLongLong();
    s.projectId = q.value(2).toString();
    s.runId = q.value(3).toString();
    s.jobId = q.value(4).toString();
    s.agentId = q.value(5).toString();
    s.model = q.value(6).toString();
    s.kind = q.value(7).toString();
    s.micro = q.value(8).toLongLong();
    s.source = q.value(9).toString();
    s.generationId = q.value(10).toString();
    return s;
}

// WHERE clause of a spend filter (always starts with "WHERE 1 = 1").
QString spendWhere(const spend::Filter& f, QVariantList* binds) {
    QString w = QStringLiteral(" WHERE 1 = 1");
    auto eq = [&](const char* col, const QString& v) {
        if (v.isEmpty()) return;
        w += QStringLiteral(" AND %1 = ?").arg(QLatin1StringView(col));
        *binds << v;
    };
    eq("project_id", f.projectId);
    eq("run_id", f.runId);
    eq("job_id", f.jobId);
    eq("kind", f.kind);
    if (f.fromMs > 0) { w += QStringLiteral(" AND ts_ms >= ?"); *binds << f.fromMs; }
    if (f.toMs > 0) { w += QStringLiteral(" AND ts_ms < ?"); *binds << f.toMs; }
    return w;
}

} // namespace

// ---------------- projects ----------------

Write projects::upsert(const ProjectRow& p) {
    Write w;
    w.tag = QStringLiteral("projects.upsert");
    w.fn = [p](Tx& tx) {
        if (p.id.isEmpty()) return tx.fail(QStringLiteral("project id is empty"));
        // spent_micro is not in the update list: it is the booked-money counter, moved only by the durable spend writes
        // (ST-11). A metadata upsert built from an older snapshot commits up to a batch window later and would otherwise
        // undo every spend booked in between, under-counting the project against its budget cap.
        tx.exec(QStringLiteral("INSERT INTO projects (%1) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT (id) DO UPDATE SET "
                               "kind = excluded.kind, title = excluded.title, brief_json = excluded.brief_json, "
                               "budget_micro = excluded.budget_micro, "
                               "tier = excluded.tier, status = excluded.status, updated_ms = excluded.updated_ms")
                    .arg(kProjectCols),
                {p.id, txt(p.kind), txt(p.title), jsonText(p.brief), p.budget, p.spent, txt(p.tier), txt(p.status),
                 p.createdMs, p.updatedMs});
    };
    return w;
}

std::optional<ProjectRow> projects::get(const QSqlDatabase& db, const QString& id) {
    QSqlQuery q = read(db, QStringLiteral("SELECT %1 FROM projects WHERE id = ?").arg(kProjectCols), {id});
    if (!q.next()) return std::nullopt;
    return projectFrom(q);
}

QList<ProjectRow> projects::list(const QSqlDatabase& db, int limit) {
    QList<ProjectRow> out;
    QSqlQuery q = read(db, QStringLiteral("SELECT %1 FROM projects ORDER BY created_ms DESC, id LIMIT ?").arg(kProjectCols),
                       {qMax(0, limit)});
    while (q.next()) out << projectFrom(q);
    return out;
}

// ---------------- runs ----------------

Write runs::insert(const RunRow& r) {
    Write w;
    w.tag = QStringLiteral("runs.insert");
    w.fn = [r](Tx& tx) {
        if (r.id.isEmpty()) return tx.fail(QStringLiteral("run id is empty"));
        tx.exec(QStringLiteral("INSERT INTO runs (%1) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)").arg(kRunCols),
                {r.id, txt(r.projectId), txt(r.phase), txt(r.tier), jsonText(r.plan), txt(r.planHash), txt(r.approvalId),
                 r.budget, r.ceiling, r.spent, txt(r.state), r.startedMs, r.finishedMs});
    };
    return w;
}

Write runs::setState(const QString& id, const QString& state, qint64 finishedMs) {
    Write w;
    w.tag = QStringLiteral("runs.setState");
    w.fn = [id, state, finishedMs](Tx& tx) {
        if (!tx.exec(QStringLiteral("UPDATE runs SET state = ?, finished_ms = CASE WHEN ? > 0 THEN ? ELSE finished_ms END "
                                    "WHERE id = ?"),
                     {txt(state), finishedMs, finishedMs, id}))
            return;
        if (tx.rowsAffected() == 0) tx.fail(QStringLiteral("no run %1").arg(id));
    };
    return w;
}

std::optional<RunRow> runs::get(const QSqlDatabase& db, const QString& id) {
    QSqlQuery q = read(db, QStringLiteral("SELECT %1 FROM runs WHERE id = ?").arg(kRunCols), {id});
    if (!q.next()) return std::nullopt;
    return runFrom(q);
}

QList<RunRow> runs::listByProject(const QSqlDatabase& db, const QString& projectId, int limit) {
    QList<RunRow> out;
    QSqlQuery q = read(db, QStringLiteral("SELECT %1 FROM runs WHERE project_id = ? ORDER BY started_ms DESC, id LIMIT ?")
                               .arg(kRunCols),
                       {projectId, qMax(0, limit)});
    while (q.next()) out << runFrom(q);
    return out;
}

// ---------------- jobs ----------------

bool jobs::isTerminal(QStringView s) {
    return s == u"done" || s == u"failed" || s == u"skipped" || s == u"cancelled" || s == u"unknown";
}

Write jobs::insert(QList<JobRow> rows) {
    Write w;
    w.tag = QStringLiteral("jobs.insert");
    w.fn = [rows = std::move(rows)](Tx& tx) {
        const QString ins = QStringLiteral("INSERT INTO jobs (%1) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
                                           "?, ?, ?, ?, ?, ?, ?, ?, ?)").arg(kJobCols);
        const QString edge = QStringLiteral("INSERT INTO job_edges (from_id, to_id) VALUES (?, ?)");
        for (const JobRow& j : rows) {
            if (j.id.isEmpty()) return tx.fail(QStringLiteral("job id is empty"));
            QStringList deps;
            QSet<QString> seen;
            for (const QString& d : j.deps) {
                if (d.isEmpty() || d == j.id) return tx.fail(QStringLiteral("job %1 has an invalid dependency").arg(j.id));
                if (!seen.contains(d)) { seen.insert(d); deps << d; }
            }
            const int indegree = j.indegree < 0 ? int(deps.size()) : j.indegree;
            if (!tx.exec(ins, {j.id, txt(j.runId), txt(j.filmNodeId), txt(j.kind), txt(j.role), txt(j.lane), j.priority,
                               txt(j.orderKey), jsonText(deps), indegree, txt(j.state), txt(j.model), jsonText(j.fallbacks),
                               j.estimate, j.actual, txt(j.costSource), j.attempts, txt(j.providerJobId), txt(j.idemKey),
                               txt(j.inputHash), j.leaseUntilMs, jsonText(j.outputs), txt(j.error), j.createdMs,
                               j.startedMs, j.finishedMs}))
                return;
            for (const QString& d : deps)
                if (!tx.exec(edge, {d, j.id})) return;
        }
    };
    return w;
}

Write jobs::setState(const QString& id, const QString& state, qint64 nowMs, const QString& error) {
    Write w;
    w.tag = QStringLiteral("jobs.setState");
    w.fn = [id, state, nowMs, error](Tx& tx) {
        const bool running = state == u"running";
        const bool terminal = isTerminal(state);
        if (!tx.exec(QStringLiteral("UPDATE jobs SET state = ?, error = ?, "
                                    "started_ms = CASE WHEN ? AND started_ms = 0 THEN ? ELSE started_ms END, "
                                    "finished_ms = CASE WHEN ? THEN ? ELSE finished_ms END WHERE id = ?"),
                     {txt(state), txt(error), running ? 1 : 0, nowMs, terminal ? 1 : 0, nowMs, id}))
            return;
        if (tx.rowsAffected() == 0) tx.fail(QStringLiteral("no job %1").arg(id));
    };
    return w;
}

Write jobs::markAccepted(const QString& id, const QString& providerJobId, const QString& state) {
    Write w;
    w.tag = QStringLiteral("jobs.markAccepted");
    w.durability = Durability::Durable;    // the provider holds our money from now on: on disk before the first poll
    w.fn = [id, providerJobId, state](Tx& tx) {
        if (providerJobId.isEmpty()) return tx.fail(QStringLiteral("provider job id is empty"));
        if (!tx.exec(QStringLiteral("UPDATE jobs SET provider_job_id = ?, state = ? WHERE id = ?"),
                     {providerJobId, txt(state), id}))
            return;
        if (tx.rowsAffected() == 0) tx.fail(QStringLiteral("no job %1").arg(id));
    };
    return w;
}

std::optional<JobRow> jobs::get(const QSqlDatabase& db, const QString& id) {
    QSqlQuery q = read(db, QStringLiteral("SELECT %1 FROM jobs WHERE id = ?").arg(kJobCols), {id});
    if (!q.next()) return std::nullopt;
    return jobFrom(q);
}

QList<JobRow> jobs::list(const QSqlDatabase& db, const QString& runId, const QString& state, int limit) {
    QString sql = QStringLiteral("SELECT %1 FROM jobs WHERE 1 = 1").arg(kJobCols);
    QVariantList binds;
    if (!runId.isEmpty()) { sql += QStringLiteral(" AND run_id = ?"); binds << runId; }
    if (!state.isEmpty()) { sql += QStringLiteral(" AND state = ?"); binds << state; }
    sql += QStringLiteral(" ORDER BY priority, order_key, id LIMIT ?");
    binds << qMax(0, limit);
    QList<JobRow> out;
    QSqlQuery q = read(db, sql, binds);
    while (q.next()) out << jobFrom(q);
    return out;
}

qint64 jobs::count(const QSqlDatabase& db, const QString& runId, const QString& state) {
    QString sql = QStringLiteral("SELECT count(*) FROM jobs WHERE 1 = 1");
    QVariantList binds;
    if (!runId.isEmpty()) { sql += QStringLiteral(" AND run_id = ?"); binds << runId; }
    if (!state.isEmpty()) { sql += QStringLiteral(" AND state = ?"); binds << state; }
    QSqlQuery q = read(db, sql, binds);
    return q.next() ? q.value(0).toLongLong() : -1;
}

QStringList jobs::dependents(const QSqlDatabase& db, const QString& id) {
    QStringList out;
    QSqlQuery q = read(db, QStringLiteral("SELECT to_id FROM job_edges WHERE from_id = ? ORDER BY to_id"), {id});
    while (q.next()) out << q.value(0).toString();
    return out;
}

// ---------------- spend ----------------

Write spend::append(SpendRow row) {
    Write w;
    w.tag = QStringLiteral("spend.append");
    w.durability = Durability::Durable;    // money row: committed (fsynced) before the request it pays for is sent
    if (row.id.isEmpty()) row.id = ids::random(u"spd", 8);
    w.fn = [row](Tx& tx) {
        if (row.kind != u"reserve" && row.kind != u"charge" && row.kind != u"release")
            return tx.fail(QStringLiteral("spend kind must be reserve, charge or release"));
        if (row.source != u"actual" && row.source != u"estimated")
            return tx.fail(QStringLiteral("spend source must be actual or estimated"));
        if (row.micro < 0) return tx.fail(QStringLiteral("spend amount is negative"));
        if (row.tsMs <= 0) return tx.fail(QStringLiteral("spend row has no timestamp"));
        tx.exec(QStringLiteral("INSERT INTO spend (%1) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)").arg(kSpendCols),
                {row.id, row.tsMs, txt(row.projectId), txt(row.runId), txt(row.jobId), txt(row.agentId), txt(row.model),
                 row.kind, row.micro, row.source, txt(row.generationId)});
        tx.setValue(row.id);
    };
    return w;
}

money::Micro spend::sum(const QSqlDatabase& db, const Filter& f, bool* ok) {
    QVariantList binds;
    const QString where = spendWhere(f, &binds);
    QSqlQuery q = read(db, QStringLiteral("SELECT COALESCE(SUM(micro), 0) FROM spend") + where, binds);
    const bool good = q.next();
    if (ok) *ok = good;
    return good ? q.value(0).toLongLong() : 0;
}

QList<SpendRow> spend::list(const QSqlDatabase& db, const Filter& f, int limit) {
    QVariantList binds;
    const QString where = spendWhere(f, &binds);
    binds << qMax(0, limit);
    QList<SpendRow> out;
    QSqlQuery q = read(db, QStringLiteral("SELECT %1 FROM spend").arg(kSpendCols) + where
                               + QStringLiteral(" ORDER BY ts_ms, id LIMIT ?"),
                       binds);
    while (q.next()) out << spendFrom(q);
    return out;
}

// ---------------- events ----------------

Write events::append(const QString& type, const QString& projectId, const QJsonObject& payload, qint64 tsMs) {
    Write w;
    w.tag = QStringLiteral("events.append");
    w.fn = [type, projectId, payload, tsMs](Tx& tx) {
        if (type.isEmpty()) return tx.fail(QStringLiteral("event type is empty"));
        if (tx.exec(QStringLiteral("INSERT INTO events (ts_ms, type, project_id, payload_json) VALUES (?, ?, ?, ?)"),
                    {tsMs, type, txt(projectId), jsonText(payload)}))
            tx.setValue(tx.lastInsertId());
    };
    return w;
}

QList<EventRow> events::since(const QSqlDatabase& db, qint64 seq, int limit, const QString& projectId) {
    QString sql = QStringLiteral("SELECT seq, ts_ms, type, project_id, payload_json FROM events WHERE seq > ?");
    QVariantList binds;
    binds << seq;
    if (!projectId.isEmpty()) { sql += QStringLiteral(" AND project_id = ?"); binds << projectId; }
    sql += QStringLiteral(" ORDER BY seq LIMIT ?");
    binds << qMax(0, limit);
    QList<EventRow> out;
    QSqlQuery q = read(db, sql, binds);
    while (q.next()) {
        EventRow e;
        e.seq = q.value(0).toLongLong();
        e.tsMs = q.value(1).toLongLong();
        e.type = q.value(2).toString();
        e.projectId = q.value(3).toString();
        e.payload = objectOf(q.value(4));
        out << e;
    }
    return out;
}

qint64 events::lastSeq(const QSqlDatabase& db) {
    QSqlQuery q = read(db, QStringLiteral("SELECT COALESCE(MAX(seq), 0) FROM events"), {});
    return q.next() ? q.value(0).toLongLong() : -1;
}

// ---------------- audit ----------------

Write audit::append(const QString& actor, const QString& action, const QString& target, const QJsonObject& detail,
                    qint64 tsMs) {
    Write w;
    w.tag = QStringLiteral("audit.append");
    w.fn = [actor, action, target, detail, tsMs](Tx& tx) {
        if (action.isEmpty()) return tx.fail(QStringLiteral("audit action is empty"));
        tx.exec(QStringLiteral("INSERT INTO audit (ts_ms, actor, action, target, detail_json) VALUES (?, ?, ?, ?, ?)"),
                {tsMs, txt(actor), action, txt(target), jsonText(detail)});
    };
    return w;
}

} // namespace studio::store
