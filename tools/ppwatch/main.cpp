// PlayPlayWatchdog.exe (docs/plans/perf-supervisor-app.md §5.3, W19 = lane stability STB-6): an external, report-only
// watchdog for the playout stack. See tools/ppwatch/README.md.
//
// Why: on 10-24 / 10-25 the Supervisor froze for hours and nothing outside it noticed or kept evidence. STB-4 gave every
// process its own /healthz (answered from a thread of its own while the main thread is frozen); this tool polls those
// endpoints from outside and, when one process hangs, writes an incident bundle (thread wait reasons, netstat, log
// tails, the last /healthz bodies) and a minidump of THAT process. It acts only on processes it can identify from its
// configuration: the pid that owns the configured health port, cross-checked with the pinned pid / image name / the pid
// in the /healthz body. Restarting a hung Supervisor is opt-in (--restart-hung-supervisor-after, default off).
//
// It never writes to the console while watching (the outage class this lane removes): events go to
// <out>/ppwatch-YYYY-MM-DD.jsonl, incidents to <out>/incidents/. Only --once, --print-config and --help print.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include "healthprobe.h"
#include "incident.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QSet>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

using namespace ppwatch;

namespace {

constexpr quint32 kTerminateCode = 0xDEAD;     // same code as the in-process abort policy (loopwatchdog.h)
constexpr int kPollHistory = 30;
constexpr int kMaxRestartsPerHour = 3;

// --- configuration ----------------------------------------------------------------------------------------------------

struct Target {
    QString name, urlText, role = QStringLiteral("other"), exe;
    Url url;
    quint32 pin = 0;
    QStringList logs;
    bool dump = true;
    bool discovered = false;
    // runtime
    Tracker tracker;
    qint64 pinCreate = 0;
    quint32 verifiedPid = 0;     // last pid confirmed by a /healthz body naming the listener itself
    qint64 verifiedCreate = 0;
    quint32 listener = 0;
    bool pinGone = false;
    QJsonObject lastHealth;
    QJsonArray polls;
    std::deque<qint64> restarts; // wall ms of restarts (rate limit)
    bool remove = false;
};

struct Config {
    QString out;
    int intervalMs = 2000, timeoutMs = 1500, unresponsivePolls = 2;
    int restartAfterSec = 0;
    QString restartCommand;
    bool discoverEngines = false, dumpEngines = false, reportOrphans = false;
    QString engineExe = QStringLiteral("PlayPlayChannelEngine.exe"), supervisorExe = QStringLiteral("PlayPlaySupervisor.exe");
    QString engineLogDir;        // for discovered engines started without --log-dir
    QStringList logs;            // logs tailed into every incident (the app's)
    QList<quint16> ports;        // netstat filter (plus every target's health port)
    int tailLines = 500, maxDumpsPerHour = 2, maxIncidentsPerHour = 6, keepIncidents = 50;
    int runForSec = 0;
    std::vector<Target> targets;
};

QString isoNow() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }

void applyTrackerOptions(const Config& c, Target& t) {
    TrackerOptions o;
    o.unresponsivePolls = c.unresponsivePolls;
    o.supervisorRole = t.role == QLatin1String("supervisor");
    o.restartAfterMs = o.supervisorRole ? qint64(c.restartAfterSec) * 1000 : 0;
    t.tracker.setOptions(o);
}

// "name=supervisor,url=http://127.0.0.1:7709/healthz,pid=123,exe=PlayPlaySupervisor.exe,role=supervisor,logs=a;b,dump=0"
// A comma that does not start a known key belongs to the previous value (paths may contain commas).
bool parseTargetSpec(const QString& spec, Target* t, QString* error) {
    static const QStringList keys = {"name", "url", "pid", "exe", "role", "logs", "dump"};
    QList<QPair<QString, QString>> kv;
    for (const QString& tok : spec.split(',')) {
        const int eq = tok.indexOf('=');
        const QString k = eq > 0 ? tok.left(eq).trimmed().toLower() : QString();
        if (eq > 0 && keys.contains(k)) kv.append({k, tok.mid(eq + 1)});
        else if (!kv.isEmpty()) kv.last().second += ',' + tok;
        else { if (error) *error = QStringLiteral("target spec must start with key=value: ") + spec; return false; }
    }
    for (const auto& p : kv) {
        const QString v = p.second.trimmed();
        if (p.first == "name") t->name = v;
        else if (p.first == "url") t->urlText = v;
        else if (p.first == "pid") t->pin = v.toUInt();
        else if (p.first == "exe") t->exe = v;
        else if (p.first == "role") t->role = v.toLower();
        else if (p.first == "logs") t->logs = v.split(';', Qt::SkipEmptyParts);
        else if (p.first == "dump") t->dump = !(v == "0" || v.compare("false", Qt::CaseInsensitive) == 0 || v.compare("off", Qt::CaseInsensitive) == 0);
    }
    return true;
}

bool finishTarget(const Config& c, Target& t, QString* error) {
    if (!parseUrl(t.urlText, &t.url, error)) return false;
    t.urlText = t.url.text();
    if (t.name.isEmpty()) t.name = QStringLiteral("port-%1").arg(t.url.port);
    if (!QStringList{"supervisor", "engine", "app", "other"}.contains(t.role)) { if (error) *error = QStringLiteral("role must be supervisor, engine, app or other: ") + t.role; return false; }
    // The role's program name doubles as an identity check: a role=supervisor target never acts on anything else.
    if (t.exe.isEmpty() && t.role == "supervisor") t.exe = c.supervisorExe;
    if (t.exe.isEmpty() && t.role == "engine") t.exe = c.engineExe;
    applyTrackerOptions(c, t);
    return true;
}

QJsonObject targetJson(const Target& t) {
    QJsonObject o{{"name", t.name}, {"url", t.urlText}, {"role", t.role}, {"dump", t.dump}};
    if (t.pin) o.insert("pid", double(t.pin));
    if (!t.exe.isEmpty()) o.insert("exe", t.exe);
    if (!t.logs.isEmpty()) o.insert("logs", QJsonArray::fromStringList(t.logs));
    if (t.discovered) o.insert("discovered", true);
    return o;
}

QJsonObject configJson(const Config& c) {
    QJsonArray targets, ports;
    for (const Target& t : c.targets) targets.append(targetJson(t));
    for (quint16 p : c.ports) ports.append(int(p));
    return QJsonObject{{"out", QDir::toNativeSeparators(c.out)}, {"intervalMs", c.intervalMs}, {"timeoutMs", c.timeoutMs},
                       {"unresponsivePolls", c.unresponsivePolls}, {"restartHungSupervisorAfterSec", c.restartAfterSec},
                       {"restartCommand", redactCommandLine(c.restartCommand)},
                       {"discoverEngines", c.discoverEngines}, {"dumpEngines", c.dumpEngines}, {"reportOrphans", c.reportOrphans},
                       {"engineExe", c.engineExe}, {"supervisorExe", c.supervisorExe}, {"engineLogDir", c.engineLogDir},
                       {"logs", QJsonArray::fromStringList(c.logs)}, {"ports", ports}, {"tailLines", c.tailLines},
                       {"maxDumpsPerHour", c.maxDumpsPerHour}, {"maxIncidentsPerHour", c.maxIncidentsPerHour},
                       {"keepIncidents", c.keepIncidents}, {"targets", targets}};
}

bool loadConfigFile(const QString& path, Config& c, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { *error = QStringLiteral("cannot read ") + path; return false; }
    QJsonParseError pe;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll(), &pe).object();
    if (pe.error != QJsonParseError::NoError) { *error = path + ": " + pe.errorString(); return false; }
    auto i = [&o](const char* k, int def) { return o.contains(k) ? o.value(k).toInt(def) : def; };
    auto b = [&o](const char* k, bool def) { return o.contains(k) ? o.value(k).toBool(def) : def; };
    auto s = [&o](const char* k, const QString& def) { return o.contains(k) ? o.value(k).toString() : def; };
    c.out = s("out", c.out);
    c.intervalMs = i("intervalMs", c.intervalMs); c.timeoutMs = i("timeoutMs", c.timeoutMs);
    c.unresponsivePolls = i("unresponsivePolls", c.unresponsivePolls);
    c.restartAfterSec = i("restartHungSupervisorAfterSec", c.restartAfterSec);
    c.restartCommand = s("restartCommand", c.restartCommand);
    c.discoverEngines = b("discoverEngines", c.discoverEngines); c.dumpEngines = b("dumpEngines", c.dumpEngines);
    c.reportOrphans = b("reportOrphans", c.reportOrphans);
    c.engineExe = s("engineExe", c.engineExe); c.supervisorExe = s("supervisorExe", c.supervisorExe);
    c.engineLogDir = s("engineLogDir", c.engineLogDir);
    for (const QJsonValue& v : o.value("logs").toArray()) c.logs << v.toString();
    for (const QJsonValue& v : o.value("ports").toArray()) c.ports << quint16(v.toInt());
    c.tailLines = i("tailLines", c.tailLines); c.maxDumpsPerHour = i("maxDumpsPerHour", c.maxDumpsPerHour);
    c.maxIncidentsPerHour = i("maxIncidentsPerHour", c.maxIncidentsPerHour); c.keepIncidents = i("keepIncidents", c.keepIncidents);
    for (const QJsonValue& v : o.value("targets").toArray()) {
        const QJsonObject t = v.toObject();
        Target x;
        x.name = t.value("name").toString(); x.urlText = t.value("url").toString();
        x.pin = quint32(t.value("pid").toDouble()); x.exe = t.value("exe").toString();
        x.role = t.value("role").toString(QStringLiteral("other")).toLower();
        for (const QJsonValue& l : t.value("logs").toArray()) x.logs << l.toString();
        x.dump = t.value("dump").toBool(true);
        if (t.value("discovered").toBool()) continue;   // discovered at run time, not configured
        c.targets.push_back(x);
    }
    return true;
}

// The station: Supervisor /healthz on 7709 (STB-4), its engines discovered as its children, the live log folders.
void applyStation(Config& c, const QString& dataDirArg, const QString& appLogDirArg, int healthPort) {
    const QString appdata = QDir::fromNativeSeparators(qEnvironmentVariable("APPDATA"));
    const QString data = dataDirArg.isEmpty() ? appdata + "/PlayPlay/PLAY PLAY/supervisor" : QDir::fromNativeSeparators(dataDirArg);
    const QString logs = data + "/logs";
    const QString appLogs = appLogDirArg.isEmpty() ? appdata + "/PlayPlay/PLAY PLAY/logs" : QDir::fromNativeSeparators(appLogDirArg);
    const bool have = std::any_of(c.targets.begin(), c.targets.end(), [](const Target& t) { return t.name == "supervisor"; });
    if (!have) {
        Target s;
        s.name = "supervisor"; s.role = "supervisor"; s.exe = c.supervisorExe;
        s.urlText = QStringLiteral("http://127.0.0.1:%1/healthz").arg(healthPort);
        s.logs = {logs + "/supervisor-*.log", logs + "/watchdog-supervisor-*.jsonl"};
        c.targets.insert(c.targets.begin(), s);
    }
    c.logs << appLogs + "/playplay_*.log" << appLogs + "/watchdog-app-*.jsonl";
    for (quint16 p : {quint16(7700), quint16(7701), quint16(healthPort)}) if (!c.ports.contains(p)) c.ports << p;
    c.discoverEngines = true;
    c.reportOrphans = true;
    if (c.engineLogDir.isEmpty()) c.engineLogDir = logs;
    if (c.out.isEmpty()) c.out = logs;
}

// --- event log (never the console) ------------------------------------------------------------------------------------

class EventLog {
public:
    explicit EventLog(const QString& dir) : m_dir(dir) {}
    void write(QJsonObject o) {
        o.insert("ts", isoNow());
        const QByteArray line = QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
        std::lock_guard<std::mutex> lk(m_mu);
        const QString path = QDir(m_dir).filePath(QStringLiteral("ppwatch-%1.jsonl").arg(QDate::currentDate().toString(Qt::ISODate)));
#ifdef _WIN32
        // One open/append/close per line, like the in-process watchdog: no handle kept, nothing buffered, never stdio.
        HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD w = 0;
        WriteFile(h, line.constData(), DWORD(line.size()), &w, nullptr);
        CloseHandle(h);
#else
        QFile f(path);
        if (f.open(QIODevice::Append)) f.write(line);
#endif
    }
    void prune(int keepDays) {
        const QDate limit = QDate::currentDate().addDays(-keepDays);
        for (const QFileInfo& fi : QDir(m_dir).entryInfoList({"ppwatch-*.jsonl"}, QDir::Files)) {
            const QDate d = QDate::fromString(fi.completeBaseName().mid(8), Qt::ISODate);
            if (d.isValid() && d < limit) QFile::remove(fi.absoluteFilePath());
        }
    }
private:
    QString m_dir;
    std::mutex m_mu;
};

// --- incident / restart worker: one job at a time, FIFO (an incident is always written before its restart) -----------

struct Job {
    enum Kind { Incident, Restart } kind = Incident;
    IncidentRequest req;
    // restart
    QString target;
    Url url;
    quint32 pid = 0;
    qint64 createTime = 0;
    QString image, command;
    int timeoutMs = 1500;
};

class Worker {
public:
    Worker(EventLog& log, int keepIncidents) : m_log(log), m_keep(keepIncidents) { m_thread = std::thread([this]() { run(); }); }
    ~Worker() { stop(10000); }
    void post(Job j) { { std::lock_guard<std::mutex> lk(m_mu); m_q.push_back(std::move(j)); } m_cv.notify_one(); }
    /// Stops after the current job. False when a job is still stuck after waitMs (a dump of a process in a bad state):
    /// the thread is then left running and the caller must end the process without running destructors.
    bool stop(int waitMs) {
        if (!m_thread.joinable()) return true;
        { std::lock_guard<std::mutex> lk(m_mu); m_stop = true; m_q.clear(); }
        m_cv.notify_one();
        std::unique_lock<std::mutex> lk(m_mu);
        if (m_doneCv.wait_for(lk, std::chrono::milliseconds(waitMs), [this]() { return m_done; })) { lk.unlock(); m_thread.join(); return true; }
        lk.unlock();
        m_thread.detach();
        return false;
    }
private:
    void run() {
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(m_mu);
                m_cv.wait(lk, [this]() { return m_stop || !m_q.empty(); });
                if (m_stop) break;
                j = std::move(m_q.front());
                m_q.pop_front();
            }
            if (j.kind == Job::Incident) incident(j); else restart(j);
        }
        std::lock_guard<std::mutex> lk(m_mu);
        m_done = true;
        m_doneCv.notify_all();
    }
    void incident(const Job& j) {
        const IncidentResult r = writeIncident(j.req);
        QJsonObject o{{"event", "incident"}, {"target", j.req.target}, {"pid", double(j.req.pid)}, {"trigger", j.req.trigger},
                      {"dir", QDir::toNativeSeparators(r.dir)}, {"ok", r.ok}, {"ms", double(r.ms)}, {"summary", r.summary}};
        if (!r.dumpPath.isEmpty()) { o.insert("dump", QDir::toNativeSeparators(r.dumpPath)); o.insert("dumpMs", double(r.dumpMs)); }
        if (!r.dumpError.isEmpty()) o.insert("dumpError", r.dumpError);
        if (!j.req.dumpSkipped.isEmpty()) o.insert("dumpSkipped", j.req.dumpSkipped);
        if (!r.error.isEmpty()) o.insert("error", r.error);
        m_log.write(o);
        const int pruned = pruneIncidents(j.req.root, m_keep);
        if (pruned) m_log.write(QJsonObject{{"event", "prune"}, {"removed", pruned}, {"keep", m_keep}});
    }
    void restart(const Job& j) {
        QJsonObject o{{"event", "restart"}, {"target", j.target}, {"pid", double(j.pid)}, {"image", j.image}};
        // Re-check right before acting: still hung, still the same process on the same port.
        const ProbeResult again = probe(j.url, j.timeoutMs);
        if (again.answered() && !Tracker::isHangState(again.state())) {
            o.insert("ok", false); o.insert("error", QStringLiteral("recovered before the restart (state %1)").arg(again.state()));
            m_log.write(o);
            return;
        }
        if (listenerPid(tcpTable(), j.url.port) != j.pid) {
            o.insert("ok", false); o.insert("error", QStringLiteral("port %1 is no longer owned by pid %2").arg(j.url.port).arg(j.pid));
            m_log.write(o);
            return;
        }
        QString err;
        const bool ok = terminateProcess(j.pid, j.createTime, j.image, kTerminateCode, 5000, &err);
        o.insert("ok", ok); o.insert("exitCode", double(kTerminateCode));
        if (!ok) o.insert("error", err);
        if (ok && !j.command.isEmpty()) {
            QString lerr;
            const quint32 np = launchDetached(j.command, &lerr);
            o.insert("command", redactCommandLine(j.command));
            if (np) o.insert("newPid", double(np)); else o.insert("launchError", lerr);
        }
        m_log.write(o);
    }

    EventLog& m_log;
    int m_keep;
    std::thread m_thread;
    std::mutex m_mu;
    std::condition_variable m_cv, m_doneCv;
    std::deque<Job> m_q;
    bool m_stop = false, m_done = false;
};

// --- identity ---------------------------------------------------------------------------------------------------------

struct Identity {
    bool ok = false;
    quint32 pid = 0;
    qint64 createTime = 0;
    QString image, reason;
    QJsonObject json;
};

// Who may be dumped or restarted for this target: the owner of the configured port's listener, and only when the
// configuration confirms it (pinned pid, image name, or the pid the /healthz body itself names).
Identity verifyIdentity(const Target& t, quint32 listener, qint64 bodyPid) {
    Identity id;
    id.json = QJsonObject{{"port", int(t.url.port)}, {"listenerPid", double(listener)}, {"bodyPid", double(bodyPid)},
                          {"pinnedPid", double(t.pin)}, {"exe", t.exe}, {"lastVerifiedPid", double(t.verifiedPid)}};
    auto refuse = [&id](const QString& why) { id.reason = why; id.json.insert("verified", false); id.json.insert("reason", why); return id; };
    if (!listener) return refuse(QStringLiteral("nothing listens on port %1").arg(t.url.port));
    if (t.pin && listener != t.pin) return refuse(QStringLiteral("port %1 belongs to pid %2, not the configured pid %3").arg(t.url.port).arg(listener).arg(t.pin));
    if (bodyPid && bodyPid != qint64(listener)) return refuse(QStringLiteral("/healthz names pid %1, but port %2 belongs to pid %3").arg(bodyPid).arg(t.url.port).arg(listener));
    const ProcessId q = queryProcess(listener);
    if (!q.alive) return refuse(QStringLiteral("pid %1 cannot be opened or has exited").arg(listener));
    if (t.pin && t.pinCreate && q.createTime != t.pinCreate) return refuse(QStringLiteral("pid %1 was reused by another process").arg(listener));
    if (!t.exe.isEmpty() && !sameImage(q.image, t.exe)) return refuse(QStringLiteral("pid %1 is %2, not %3").arg(listener).arg(q.image, t.exe));
    QStringList by;
    if (t.pin) by << "pid";
    if (!t.exe.isEmpty()) by << "exe";
    if (bodyPid) by << "healthz-pid";
    if (!bodyPid && t.verifiedPid == listener && t.verifiedCreate == q.createTime) by << "earlier-healthz-pid";
    if (by.isEmpty()) return refuse(QStringLiteral("nothing confirms who owns port %1 (set pid= or exe=, or let it answer /healthz once)").arg(t.url.port));
    if (isProtectedProcess(listener, q.image)) return refuse(QStringLiteral("pid %1 (%2) is a protected system process").arg(listener).arg(q.image));
    id.ok = true; id.pid = listener; id.createTime = q.createTime; id.image = q.image;
    id.json.insert("verified", true);
    id.json.insert("by", QJsonArray::fromStringList(by));
    id.json.insert("image", q.image);
    return id;
}

// --- the watcher ------------------------------------------------------------------------------------------------------

class Watcher {
public:
    Watcher(Config& c, EventLog* log, Worker* worker) : m_c(c), m_log(log), m_worker(worker) { m_clock.start(); }

    QString incidentsRoot() const { return QDir(m_c.out).filePath("incidents"); }

    /// One poll of every target. dryRun: observe only (no incidents, no restarts), for --once.
    QJsonArray cycle(bool dryRun) {
        const std::vector<TcpRow> tcp = tcpTable();
        std::vector<Url> urls;
        std::vector<size_t> idx;
        for (size_t i = 0; i < m_c.targets.size(); ++i) {
            Target& t = m_c.targets[i];
            t.listener = listenerPid(tcp, t.url.port);
            t.pinGone = false;
            if (t.pin) {
                const ProcessId q = queryProcess(t.pin);
                if (!t.pinCreate && q.alive) t.pinCreate = q.createTime;
                t.pinGone = !q.alive || (t.pinCreate && q.createTime != t.pinCreate);
            }
            // Nothing listening: no connect at all (a connect to a closed loopback port takes ~2 s on Windows).
            if (!t.pinGone && t.listener) { urls.push_back(t.url); idx.push_back(i); }
        }
        const std::vector<ProbeResult> res = probeAll(urls, m_c.timeoutMs);
        QJsonArray status;
        const qint64 mono = m_clock.elapsed();
        for (size_t i = 0; i < m_c.targets.size(); ++i) {
            Target& t = m_c.targets[i];
            const auto it = std::find(idx.begin(), idx.end(), i);
            const ProbeResult r = it != idx.end() ? res[size_t(it - idx.begin())] : ProbeResult();
            const bool probed = it != idx.end();
            Observation ob;
            ob.tMs = mono;
            ob.pid = t.listener;
            if (t.pinGone) { ob.reach = Observation::PidGone; ob.pid = t.pin; }
            else if (!t.listener) ob.reach = Observation::NoListener;
            else if (r.answered()) { ob.reach = Observation::Answered; ob.state = r.state(); }
            else ob.reach = Observation::NoAnswer;
            const qint64 bodyPid = r.answered() ? r.pid() : 0;
            if (r.answered()) {
                t.lastHealth = r.json;
                if (bodyPid && bodyPid == qint64(t.listener) && (!t.pin || t.listener == t.pin) &&
                    (t.exe.isEmpty() || sameImage(queryProcess(t.listener).image, t.exe))) {
                    if (t.verifiedPid != t.listener) t.verifiedCreate = queryProcess(t.listener).createTime;
                    t.verifiedPid = t.listener;
                }
            }
            const Decision d = t.tracker.feed(ob);
            static const char* const reach[] = {"answered", "no-answer", "no-listener", "pid-gone"};
            QJsonObject poll{{"t", isoNow()}, {"reach", reach[ob.reach]}, {"state", t.tracker.state()}, {"pid", double(ob.pid)}};
            if (probed) { poll.insert("kind", ProbeResult::kindName(r.kind)); poll.insert("ms", double(r.ms)); }
            if (r.answered()) {
                poll.insert("bodyPid", double(bodyPid)); poll.insert("status", r.status);
                if (r.json.contains("lagMs")) poll.insert("lagMs", r.json.value("lagMs"));
                if (!r.json.value("activity").toString().isEmpty()) poll.insert("activity", r.json.value("activity"));
            } else if (probed && !r.error.isEmpty()) poll.insert("error", r.error);
            t.polls.append(poll);
            while (t.polls.size() > kPollHistory) t.polls.removeFirst();

            if (dryRun) {
                QJsonObject s = poll;
                s.insert("name", t.name); s.insert("url", t.urlText);
                // One look: "no answer" is reported as such (the tracker would wait for a second miss).
                s.insert("state", ob.reach == Observation::Answered ? ob.state : ob.reach == Observation::NoAnswer ? QStringLiteral("unresponsive")
                                  : ob.reach == Observation::NoListener ? QStringLiteral("down") : QStringLiteral("gone"));
                const Identity id = verifyIdentity(t, t.listener, bodyPid);
                s.insert("identity", id.json);
                if (r.answered()) s.insert("health", r.json);
                status.append(s);
                continue;
            }
            if (d.changed && m_log) {
                QJsonObject e{{"event", "state"}, {"target", t.name}, {"from", d.from}, {"to", d.to}, {"pid", double(ob.pid)}};
                for (const char* k : {"kind", "ms", "bodyPid", "lagMs", "activity", "error"}) if (poll.contains(k)) e.insert(k, poll.value(k));
                m_log->write(e);
            }
            if (d.incident) openIncident(t, d, bodyPid);
            if (d.restart) requestRestart(t, d, bodyPid);
            if (t.discovered && ob.reach == Observation::PidGone) t.remove = true;
        }
        if (!dryRun) {
            const size_t before = m_c.targets.size();
            m_c.targets.erase(std::remove_if(m_c.targets.begin(), m_c.targets.end(), [](const Target& t) { return t.remove; }), m_c.targets.end());
            if (before != m_c.targets.size() && m_log) m_log->write(QJsonObject{{"event", "forget"}, {"removed", int(before - m_c.targets.size())}});
        }
        return status;
    }

    /// Engines that are children of a verified supervisor target become targets of their own (their /healthz port is
    /// the --port the Supervisor gave them). Dumps of discovered engines only with --dump-engines.
    void discover() {
        if (!m_c.discoverEngines) return;
        const std::vector<ProcInfo> procs = processSnapshot(false);
        std::vector<Target> add;
        for (const Target& s : m_c.targets) {
            if (s.role != "supervisor" || !s.verifiedPid) continue;
            const ProcInfo* sp = findProcess(procs, s.verifiedPid);
            if (!sp || sp->createTime != s.verifiedCreate) continue;
            for (const ProcInfo& p : procs) {
                if (p.ppid != s.verifiedPid || p.createTime < sp->createTime || !sameImage(p.image, m_c.engineExe)) continue;
                const bool known = std::any_of(m_c.targets.begin(), m_c.targets.end(), [&p](const Target& t) { return t.pin == p.pid; });
                if (known) continue;
                const EngineArgs ea = parseEngineArgs(commandLine(p.pid));
                if (ea.port <= 0 || ea.port > 65535) continue;
                Target e;
                e.name = "engine-" + (ea.id.isEmpty() ? QString::number(p.pid) : ea.id);
                e.urlText = QStringLiteral("http://127.0.0.1:%1/healthz").arg(ea.port);
                e.role = "engine"; e.exe = m_c.engineExe; e.pin = p.pid; e.pinCreate = p.createTime;
                e.dump = m_c.dumpEngines; e.discovered = true;
                const QString ld = !ea.logDir.isEmpty() ? QDir::fromNativeSeparators(ea.logDir) : m_c.engineLogDir;
                if (!ld.isEmpty()) e.logs = {ld + "/" + e.name + "-*.log", ld + "/watchdog-" + e.name + "-*.jsonl"};
                QString err;
                if (!finishTarget(m_c, e, &err)) continue;
                if (m_log) m_log->write(QJsonObject{{"event", "discover"}, {"target", e.name}, {"pid", double(p.pid)}, {"port", ea.port},
                                                   {"supervisor", s.name}, {"supervisorPid", double(s.verifiedPid)}, {"dump", e.dump}});
                add.push_back(e);
            }
        }
        for (Target& e : add) {
            // A restarted engine keeps its name: the old (gone) target is replaced.
            m_c.targets.erase(std::remove_if(m_c.targets.begin(), m_c.targets.end(), [&e](const Target& t) { return t.discovered && t.name == e.name; }), m_c.targets.end());
            m_c.targets.push_back(e);
        }
    }

    void reportOrphans() {
        if (!m_c.reportOrphans || !m_log) return;
        const std::vector<ProcInfo> procs = processSnapshot(false);
        QSet<quint32> now;
        for (const Orphan& o : findOrphans(procs, m_c.engineExe, m_c.supervisorExe)) {
            const EngineArgs ea = parseEngineArgs(commandLine(o.pid));
            if (ea.standalone) continue;   // started by hand / by a test: nobody's orphan
            now.insert(o.pid);
            if (!m_orphans.contains(o.pid))
                m_log->write(QJsonObject{{"event", "orphan"}, {"pid", double(o.pid)}, {"ppid", double(o.ppid)}, {"reason", o.reason},
                                        {"id", ea.id}, {"port", ea.port}, {"action", "none (report only)"}});
        }
        m_orphans = now;
    }

private:
    QList<quint16> portsFor(const Target& t) const {
        QList<quint16> p = m_c.ports;
        for (const Target& x : m_c.targets) if (!p.contains(x.url.port)) p << x.url.port;
        if (!p.contains(t.url.port)) p << t.url.port;
        return p;
    }

    void openIncident(Target& t, const Decision& d, qint64 bodyPid) {
        const Identity id = verifyIdentity(t, t.listener, bodyPid);
        if (!id.ok) {
            m_log->write(QJsonObject{{"event", "refused"}, {"target", t.name}, {"action", "incident"}, {"state", d.to}, {"reason", id.reason}, {"identity", id.json}});
            return;
        }
        const QString root = incidentsRoot();
        const int recent = recentIncidents(root, t.name, 3600, false);
        if (recent >= m_c.maxIncidentsPerHour) {
            m_log->write(QJsonObject{{"event", "skipped"}, {"target", t.name}, {"pid", double(id.pid)},
                                     {"reason", QStringLiteral("rate limit (%1 incidents per hour)").arg(m_c.maxIncidentsPerHour)}});
            return;
        }
        Job j;
        IncidentRequest& r = j.req;
        r.root = root; r.target = t.name; r.url = t.urlText; r.role = t.role;
        r.pid = id.pid; r.createTime = id.createTime; r.image = id.image;
        r.trigger = d.to; r.episodeMs = d.episodeMs;
        r.lastHealth = t.lastHealth; r.polls = t.polls; r.identity = id.json;
        r.logGlobs = t.logs + m_c.logs;
        r.ports = portsFor(t);
        r.tailLines = m_c.tailLines;
        r.engineExe = m_c.engineExe; r.supervisorExe = m_c.supervisorExe;
        for (const Target& x : m_c.targets) if (!x.exe.isEmpty() && !r.images.contains(x.exe)) r.images << x.exe;
        if (!t.dump) { r.dump = false; r.dumpSkipped = QStringLiteral("dumping is off for this target"); }
        else if (recentIncidents(root, t.name, 3600, true) >= m_c.maxDumpsPerHour) {
            r.dump = false; r.dumpSkipped = QStringLiteral("rate limit (%1 dumps per hour)").arg(m_c.maxDumpsPerHour);
        }
        m_log->write(QJsonObject{{"event", "hang"}, {"target", t.name}, {"pid", double(id.pid)}, {"trigger", d.to},
                                 {"dump", r.dump}, {"identity", id.json}});
        m_worker->post(std::move(j));
    }

    void requestRestart(Target& t, const Decision& d, qint64 bodyPid) {
        const Identity id = verifyIdentity(t, t.listener, bodyPid);
        auto refuse = [&](const QString& why) {
            m_log->write(QJsonObject{{"event", "refused"}, {"target", t.name}, {"action", "restart"}, {"reason", why}, {"identity", id.json}});
        };
        if (!id.ok) return refuse(id.reason);
        if (t.exe.isEmpty() || !sameImage(id.image, t.exe)) return refuse(QStringLiteral("a restart needs the target's exe= to match"));
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        while (!t.restarts.empty() && now - t.restarts.front() > 3600 * 1000) t.restarts.pop_front();
        if (int(t.restarts.size()) >= kMaxRestartsPerHour) return refuse(QStringLiteral("restart limit (%1 per hour)").arg(kMaxRestartsPerHour));
        t.restarts.push_back(now);
        Job j;
        j.kind = Job::Restart;
        j.target = t.name; j.url = t.url; j.pid = id.pid; j.createTime = id.createTime; j.image = id.image;
        j.command = m_c.restartCommand; j.timeoutMs = m_c.timeoutMs;
        m_log->write(QJsonObject{{"event", "restart-due"}, {"target", t.name}, {"pid", double(id.pid)}, {"hungMs", double(d.episodeMs)},
                                 {"afterSec", m_c.restartAfterSec}});
        m_worker->post(std::move(j));
    }

    Config& m_c;
    EventLog* m_log;
    Worker* m_worker;
    QElapsedTimer m_clock;
    QSet<quint32> m_orphans;
};

#ifdef _WIN32
HANDLE g_stop = nullptr;
BOOL WINAPI onConsoleCtrl(DWORD) { if (g_stop) SetEvent(g_stop); return TRUE; }
#endif

void quietHandler(QtMsgType, const QMessageLogContext&, const QString&) {}   // never the console

void printOut(const QByteArray& s) { std::fwrite(s.constData(), 1, size_t(s.size()), stdout); std::fflush(stdout); }
void printErr(const QString& s) { const QByteArray b = (s + '\n').toUtf8(); std::fwrite(b.constData(), 1, size_t(b.size()), stderr); std::fflush(stderr); }

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("PlayPlayWatchdog"));
    app.setApplicationVersion(QStringLiteral("1.0 (STB-6)"));
    qInstallMessageHandler(quietHandler);

    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral(
        "External, report-only watchdog for the PlayPlay stack: polls /healthz, writes an incident bundle and a dump of a "
        "hung process. Restarting a hung Supervisor is opt-in. See tools/ppwatch/README.md."));
    p.addHelpOption();
    p.addVersionOption();
    const QCommandLineOption station("station", "Watch this station: Supervisor /healthz on 127.0.0.1:7709 (exe PlayPlaySupervisor.exe), its engines (discovered), the live log folders; incidents in <data-dir>/logs/incidents.");
    const QCommandLineOption dataDir("data-dir", "Supervisor data dir for --station (default %APPDATA%/PlayPlay/PLAY PLAY/supervisor).", "dir");
    const QCommandLineOption appLogDir("app-log-dir", "App log dir for --station (default %APPDATA%/PlayPlay/PLAY PLAY/logs).", "dir");
    const QCommandLineOption stationPort("station-health-port", "Supervisor /healthz port for --station (default 7709).", "port", "7709");
    const QCommandLineOption config("config", "JSON configuration (the shape --print-config prints).", "file");
    const QCommandLineOption target("target", "A process to watch: name=..,url=http://127.0.0.1:<port>/healthz[,pid=N][,exe=Name.exe][,role=supervisor|engine|app|other][,logs=glob;glob][,dump=0|1]. Repeatable.", "spec");
    const QCommandLineOption out("out", "Output folder: ppwatch-YYYY-MM-DD.jsonl and incidents/ (default %LOCALAPPDATA%/PlayPlay/watchdog).", "dir");
    const QCommandLineOption interval("interval-ms", "Poll interval (default 2000).", "ms");
    const QCommandLineOption timeout("timeout-ms", "Reply timeout per poll (default 1500).", "ms");
    const QCommandLineOption unresp("unresponsive-polls", "Polls without an answer before 'unresponsive' (default 2).", "n");
    const QCommandLineOption logOpt("log", "A log glob tailed into every incident. Repeatable.", "glob");
    const QCommandLineOption portsOpt("ports", "Extra ports for netstat.txt (comma separated).", "list");
    const QCommandLineOption discover("discover-engines", "Watch the engines that are children of a verified supervisor target.");
    const QCommandLineOption dumpEngines("dump-engines", "Also dump discovered engines on HANG (off: a dump pauses an engine's threads for a moment).");
    const QCommandLineOption orphans("report-orphans", "Report engines whose parent is not a live Supervisor (report only).");
    const QCommandLineOption engineExe("engine-exe", "Engine image name (default PlayPlayChannelEngine.exe).", "name");
    const QCommandLineOption supervisorExe("supervisor-exe", "Supervisor image name (default PlayPlaySupervisor.exe).", "name");
    const QCommandLineOption restartAfter("restart-hung-supervisor-after", "OPT-IN: terminate a supervisor-role target that has been hung this long (after its incident and dump). Default 0 = never.", "sec");
    const QCommandLineOption restartCmd("restart-command", "With --restart-hung-supervisor-after: command line to start after the kill (leave it out when the app relaunches the Supervisor).", "cmd");
    const QCommandLineOption maxDumps("max-dumps-per-hour", "Per target (default 2).", "n");
    const QCommandLineOption maxIncidents("max-incidents-per-hour", "Per target (default 6).", "n");
    const QCommandLineOption keep("keep-incidents", "Newest incident folders kept (default 50).", "n");
    const QCommandLineOption tail("tail-lines", "Lines per log tail (default 500).", "n");
    const QCommandLineOption runFor("run-for", "Stop after this many seconds (tests).", "sec");
    const QCommandLineOption once("once", "Poll every target once, print the result as JSON, exit (0 = all ok/stall).");
    const QCommandLineOption printConfig("print-config", "Print the effective configuration as JSON and exit (polls nothing).");
    for (const QCommandLineOption& o : {station, dataDir, appLogDir, stationPort, config, target, out, interval, timeout, unresp, logOpt,
                                        portsOpt, discover, dumpEngines, orphans, engineExe, supervisorExe, restartAfter, restartCmd,
                                        maxDumps, maxIncidents, keep, tail, runFor, once, printConfig})
        p.addOption(o);
    p.process(app);   // --help / --version / unknown options print and exit here

    Config c;
    QString err;
    auto bad = [](const QString& e) { printErr(QStringLiteral("PlayPlayWatchdog: ") + e); return 2; };
    if (p.isSet(engineExe)) c.engineExe = p.value(engineExe);
    if (p.isSet(supervisorExe)) c.supervisorExe = p.value(supervisorExe);
    if (p.isSet(config) && !loadConfigFile(p.value(config), c, &err)) return bad(err);
    for (const QString& spec : p.values(target)) {
        Target t;
        if (!parseTargetSpec(spec, &t, &err)) return bad(err);
        c.targets.push_back(t);
    }
    auto num = [&p](const QCommandLineOption& o, int def) { bool ok = false; const int v = p.value(o).toInt(&ok); return p.isSet(o) && ok ? v : def; };
    if (p.isSet(out)) c.out = QDir::fromNativeSeparators(p.value(out));
    if (p.isSet(station)) applyStation(c, p.value(dataDir), p.value(appLogDir), num(stationPort, 7709));
    c.intervalMs = std::max(200, num(interval, c.intervalMs));
    c.timeoutMs = std::clamp(num(timeout, c.timeoutMs), 100, std::max(100, c.intervalMs - 100));
    c.unresponsivePolls = std::max(1, num(unresp, c.unresponsivePolls));
    for (const QString& g : p.values(logOpt)) c.logs << QDir::fromNativeSeparators(g);
    for (const QString& s : p.value(portsOpt).split(',', Qt::SkipEmptyParts)) { const int v = s.trimmed().toInt(); if (v > 0 && v < 65536 && !c.ports.contains(quint16(v))) c.ports << quint16(v); }
    if (p.isSet(discover)) c.discoverEngines = true;
    if (p.isSet(dumpEngines)) c.dumpEngines = true;
    if (p.isSet(orphans)) c.reportOrphans = true;
    c.restartAfterSec = std::max(0, num(restartAfter, c.restartAfterSec));
    if (p.isSet(restartCmd)) c.restartCommand = p.value(restartCmd);
    c.maxDumpsPerHour = std::max(0, num(maxDumps, c.maxDumpsPerHour));
    c.maxIncidentsPerHour = std::max(0, num(maxIncidents, c.maxIncidentsPerHour));
    c.keepIncidents = std::max(1, num(keep, c.keepIncidents));
    c.tailLines = std::max(0, num(tail, c.tailLines));
    c.runForSec = std::max(0, num(runFor, 0));
    if (c.out.isEmpty()) c.out = QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA")) + "/PlayPlay/watchdog";
    if (!c.restartCommand.isEmpty() && !c.restartAfterSec) return bad(QStringLiteral("--restart-command needs --restart-hung-supervisor-after"));
    for (Target& t : c.targets) if (!finishTarget(c, t, &err)) return bad(err);
    QSet<QString> names;
    for (const Target& t : c.targets) { if (names.contains(t.name)) return bad(QStringLiteral("two targets are named ") + t.name); names.insert(t.name); }
    if (c.targets.empty()) return bad(QStringLiteral("nothing to watch: give --target, --config or --station (see --help)"));

    if (p.isSet(printConfig)) { printOut(QJsonDocument(configJson(c)).toJson(QJsonDocument::Indented)); return 0; }

    if (p.isSet(once)) {
        Watcher w(c, nullptr, nullptr);
        QJsonArray st = w.cycle(true);
        if (c.discoverEngines) { w.discover(); st = w.cycle(true); }
        bool allOk = true;
        for (const QJsonValue& v : st) { const QString s = v.toObject().value("state").toString(); allOk = allOk && (s == "ok" || s == "stall"); }
        printOut(QJsonDocument(QJsonObject{{"ts", isoNow()}, {"targets", st}}).toJson(QJsonDocument::Indented));
        return allOk ? 0 : 1;
    }

    // --- watch ---
    if (!QDir().mkpath(c.out)) return bad(QStringLiteral("cannot create ") + c.out);
#ifdef _WIN32
    // One watchdog per output folder (two would write the same incidents twice).
    const QByteArray key = QCryptographicHash::hash(QDir::cleanPath(QFileInfo(c.out).absoluteFilePath()).toLower().toUtf8(), QCryptographicHash::Sha1).toHex().left(12);
    HANDLE single = CreateMutexW(nullptr, TRUE, reinterpret_cast<LPCWSTR>(QStringLiteral("Local\\PlayPlayWatchdog-%1").arg(QString::fromLatin1(key)).utf16()));
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) return bad(QStringLiteral("another PlayPlayWatchdog already writes to ") + QDir::toNativeSeparators(c.out)) + 1;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
#endif
    EventLog log(c.out);
    log.prune(30);
    QJsonObject start = configJson(c);
    start.insert("event", "start");
    start.insert("pid", double(QCoreApplication::applicationPid()));
    start.insert("exe", QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
    start.insert("version", QCoreApplication::applicationVersion());
    log.write(start);

    Worker worker(log, c.keepIncidents);
    Watcher w(c, &log, &worker);
    QElapsedTimer run; run.start();
    QString stopReason = QStringLiteral("signal");
    for (qint64 n = 0;; ++n) {
        QElapsedTimer cyc; cyc.start();
        if (c.discoverEngines && n % 5 == 0) w.discover();
        if (c.reportOrphans && n % 15 == 0) w.reportOrphans();
        w.cycle(false);
        if (c.runForSec && run.elapsed() >= qint64(c.runForSec) * 1000) { stopReason = QStringLiteral("run-for"); break; }
        const qint64 left = std::max<qint64>(10, c.intervalMs - cyc.elapsed());
#ifdef _WIN32
        if (WaitForSingleObject(g_stop, DWORD(left)) == WAIT_OBJECT_0) break;
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(left));
#endif
    }
    const bool joined = worker.stop(10000);
    log.write(QJsonObject{{"event", "stop"}, {"reason", stopReason}, {"ranMs", double(run.elapsed())},
                          {"worker", joined ? "stopped" : "stuck (left behind)"}});
    if (!joined) std::_Exit(0);   // the stuck worker still uses objects that destructors would free
    return 0;
}
