#include "agentsuite.h"
#include "starclient.h"
#include "starnodes.h"
#include "suitehttp.h"
#include "suitelogic.h"
#include "aimodels.h"
#include "../schedule/schedulestore.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QUrlQuery>
#include <QUuid>
#include <QSet>
#include <memory>

using namespace sched;

static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
static bool finishedState(const QString& s) { return s == "done" || s == "failed" || s == "cancelled"; }
static const QStringList kStarAgentCommands = {"health", "status", "nowplaying", "list", "search", "exec"};

AgentSuite::AgentSuite(ScheduleStore* store, StarClient* star, StarNodes* nodes, QObject* parent)
    : QObject(parent), m_store(store), m_star(star), m_nodes(nodes), m_http(new SuiteHttp(this)) {
    m_persist.setSingleShot(true);
    connect(&m_persist, &QTimer::timeout, this, &AgentSuite::persist);
    connect(&m_pump, &QTimer::timeout, this, &AgentSuite::pump);
}

void AgentSuite::start() {
    for (const QJsonValue& v : m_store->settingJson("agents.tasks").value("tasks").toArray()) {
        Task* t = new Task; t->o = v.toObject();
        if (!finishedState(t->o.value("state").toString())) { t->o["state"] = "failed"; t->o["error"] = "interrupted by a Supervisor restart"; t->o["finishedMs"] = double(nowMs()); }
        m_tasks.append(t);
    }
    m_pump.start(500);
    // StarClient marks an agent online only after a health probe: probe at start and every minute so the agent list is truthful
    if (m_star) {
        auto probe = [this]() { m_star->probeAll([](int, const QJsonObject&) {}); };
        QTimer::singleShot(2000, this, probe);
        auto* t = new QTimer(this); connect(t, &QTimer::timeout, this, probe); t->start(60000);
    }
}

QJsonObject AgentSuite::config(bool withSecrets) const {
    QJsonObject c = m_store->settingJson("agents.config");
    if (!c.contains("allowExec")) c["allowExec"] = false;
    if (!c.contains("allowActions")) c["allowActions"] = false;
    c["model"] = AiModels::model(m_store, c.value("model").toString());
    const QJsonObject k = AiModels::keyStatus(m_store);           // the key is shared (env OPENROUTER_API_KEY, or the ai.config override)
    c["hasKey"] = k.value("hasKey"); c["keySource"] = k.value("keySource"); c["envVar"] = k.value("envVar");
    c.remove("apiKey"); Q_UNUSED(withSecrets);
    return c;
}

QJsonObject AgentSuite::setConfig(const QJsonObject& patch) {
    QJsonObject c = m_store->settingJson("agents.config");
    for (const char* k : {"allowExec", "allowActions"}) if (patch.contains(k)) c[k] = patch.value(k);
    if (patch.contains("model")) c["model"] = suites::resolveAiModel(patch.value("model").toString());
    m_store->setSettingJson("agents.config", c);
    if (!patch.value("apiKey").toString().trimmed().isEmpty() || patch.value("clearKey").toBool()) {   // a typed key is the shared override
        QJsonObject ai = m_store->settingJson("ai.config");
        if (patch.value("clearKey").toBool()) ai.remove("apiKey"); else ai["apiKey"] = patch.value("apiKey").toString().trimmed();
        m_store->setSettingJson("ai.config", ai);
    }
    return config();
}

QJsonObject AgentSuite::llmConfig() const {
    QJsonObject o; o["apiKey"] = AiModels::apiKey(m_store);
    o["model"] = AiModels::model(m_store, m_store->settingJson("agents.config").value("model").toString());
    o["baseUrl"] = AiModels::baseUrl(m_store);
    return o;
}

QJsonArray AgentSuite::agents() const {
    QJsonArray a; QJsonArray cmds; for (const QJsonValue& v : suites::operatorCommands()) cmds.append(v.toObject().value("name"));
    a.append(QJsonObject{{"id", "station"}, {"kind", "operator"}, {"name", "This station"}, {"online", true}, {"commands", cmds}});
    if (m_star) for (const QJsonValue& v : m_star->agents()) {
        const QJsonObject s = v.toObject(); QJsonArray c; for (const QString& x : kStarAgentCommands) if (x != "exec" || config().value("allowExec").toBool()) c.append(x);
        a.append(QJsonObject{{"id", "star:" + s.value("node").toString()}, {"kind", "staragent"}, {"name", s.value("name").toString(s.value("node").toString())}, {"online", s.value("online").toBool()}, {"url", s.value("url")}, {"commands", c}});
    }
    if (m_nodes) for (const QJsonValue& v : m_nodes->nodes()) {
        const QJsonObject n = v.toObject(); QJsonArray acts; for (const QString& x : suites::starNodeActions()) acts.append(x);
        a.append(QJsonObject{{"id", "node:" + n.value("id").toString()}, {"kind", "starnode"}, {"name", n.value("name")}, {"online", n.value("health").toObject().value("ok").toBool()}, {"commands", acts}, {"actions", acts}});
    }
    const QJsonObject l = llmConfig();
    a.append(QJsonObject{{"id", "assistant"}, {"kind", "llm"}, {"name", "Assistant (LLM)"}, {"online", !l.value("apiKey").toString().isEmpty()}, {"available", !l.value("apiKey").toString().isEmpty()}, {"model", l.value("model")}});
    return a;
}

AgentSuite::Task* AgentSuite::find(const QString& id) const { for (Task* t : m_tasks) if (t->o.value("id").toString() == id) return t; return nullptr; }

QJsonArray AgentSuite::tasks(const QString& agent, const QString& state, int limit) const {
    QJsonArray a;
    for (int i = m_tasks.size() - 1; i >= 0 && a.size() < limit; --i) { const QJsonObject& o = m_tasks[i]->o; if ((!agent.isEmpty() && o.value("agent").toString() != agent) || (!state.isEmpty() && o.value("state").toString() != state)) continue; a.append(o); }
    return a;
}

QJsonObject AgentSuite::task(const QString& id, bool* found) const { Task* t = find(id); if (found) *found = t != nullptr; return t ? t->o : QJsonObject(); }

QJsonObject AgentSuite::submit(const QJsonObject& b) {
    QJsonObject e; e["status"] = 400;
    const QString agent = b.value("agent").toString("station");
    QJsonObject ag; for (const QJsonValue& v : agents()) if (v.toObject().value("id").toString() == agent) ag = v.toObject();
    if (ag.isEmpty()) { e["error"] = "unknown agent " + agent; e["status"] = 404; return e; }
    const QString kind = ag.value("kind").toString(), cmd = b.value("command").toString();
    QString risk = "read";
    if (kind == "llm") { if (b.value("prompt").toString().trimmed().isEmpty()) { e["error"] = "prompt required"; return e; } if (!ag.value("available").toBool()) { e["error"] = "no OpenRouter key: set the OPENROUTER_API_KEY environment variable (or save one under AI settings)"; e["status"] = 409; return e; } }
    else if (!ag.value("commands").toArray().contains(cmd)) { e["error"] = "command not available on this agent: " + cmd; e["status"] = 403; return e; }
    if (kind == "operator" && suites::operatorCommandIsAction(cmd)) risk = "action";
    if (kind == "staragent" && cmd == "exec") risk = "action";
    if (kind == "starnode" && QStringList{"exec", "service.restart", "station.call", "sync.run"}.contains(cmd)) risk = "action";
    Task* t = new Task;
    QJsonObject o; o["id"] = "at-" + QUuid::createUuid().toString(QUuid::Id128).left(8); o["agent"] = agent; o["agentKind"] = kind; o["agentName"] = ag.value("name");
    o["command"] = kind == "llm" ? QString("prompt") : cmd; o["args"] = b.value("args").toObject(); if (kind == "llm") o["prompt"] = b.value("prompt").toString().left(4000);
    o["risk"] = risk; o["state"] = "queued"; o["createdMs"] = double(nowMs()); o["actor"] = b.value("actor").toString("operator");
    t->o = o; m_tasks.append(t);
    while (m_tasks.size() > 300 && finishedState(m_tasks.first()->o.value("state").toString())) delete m_tasks.takeFirst();
    changed(t); QTimer::singleShot(0, this, &AgentSuite::pump);
    return o;
}

bool AgentSuite::removeTask(const QString& id) {
    Task* t = find(id); if (!t) return false;
    if (!finishedState(t->o.value("state").toString())) { t->cancelled = true; finish(t, "cancelled", QJsonValue(), "cancelled by the operator"); return true; }
    m_tasks.removeOne(t); delete t; emit event("agents", QJsonObject{{"removed", id}}); m_persist.start(500); return true;
}

int AgentSuite::clearFinished() {
    int n = 0; for (int i = m_tasks.size() - 1; i >= 0; --i) if (finishedState(m_tasks[i]->o.value("state").toString())) { delete m_tasks.takeAt(i); ++n; }
    m_persist.start(500); emit event("agents", QJsonObject{{"cleared", n}}); return n;
}

void AgentSuite::changed(Task* t) { emit event("agents", QJsonObject{{"task", t->o}}); m_persist.start(800); }

void AgentSuite::persist() {
    QJsonArray a; for (Task* t : m_tasks) a.append(t->o);
    m_store->setSettingJson("agents.tasks", QJsonObject{{"tasks", a}});
}

void AgentSuite::pump() {
    QSet<QString> busy; for (Task* t : m_tasks) if (t->o.value("state").toString() == "running") busy.insert(t->o.value("agent").toString());
    for (Task* t : m_tasks) {
        if (t->o.value("state").toString() != "queued") continue;
        const QString a = t->o.value("agent").toString(); if (busy.contains(a)) continue;
        busy.insert(a); run(t);
    }
}

void AgentSuite::finish(Task* t, const QString& state, const QJsonValue& result, const QString& error) {
    if (finishedState(t->o.value("state").toString())) return;
    t->o["state"] = state; t->o["finishedMs"] = double(nowMs());
    if (!result.isNull() && !result.isUndefined()) t->o["result"] = result;
    if (!error.isEmpty()) t->o["error"] = error;
    changed(t);
}

void AgentSuite::run(Task* t) {
    t->o["state"] = "running"; t->o["startedMs"] = double(nowMs()); changed(t);
    const QString kind = t->o.value("agentKind").toString(), cmd = t->o.value("command").toString(), agent = t->o.value("agent").toString();
    const QJsonObject args = t->o.value("args").toObject();
    const QString tid = t->o.value("id").toString();
    auto reply = [this, tid](int status, const QJsonObject& body) {
        Task* t2 = find(tid); if (!t2 || t2->cancelled) return;
        if (status >= 400 || status == 0) finish(t2, "failed", body, body.value("error").toString(QString("HTTP %1").arg(status)));
        else finish(t2, "done", body);
    };
    if (kind == "operator") {
        if (suites::operatorCommandIsAction(cmd) && !config().value("allowActions").toBool() && t->o.value("actor").toString() == "assistant") { reply(403, QJsonObject{{"error", "action commands are off for the assistant (agents.config.allowActions)"}}); return; }
        if (!m_op) { reply(503, QJsonObject{{"error", "operator commands not wired"}}); return; }
        m_op(cmd, args, reply); return;
    }
    if (kind == "staragent") {
        const QString node = agent.mid(5);
        if (cmd == "health") m_star->health(node, reply);
        else if (cmd == "status") m_star->status(node, reply);
        else if (cmd == "nowplaying") m_star->nowPlaying(node, reply);
        else if (cmd == "list") m_star->list(node, args.value("path").toString(), reply);
        else if (cmd == "search") m_star->search(node, args.value("path").toString(), args.value("q").toString(), reply);
        else if (cmd == "exec") {
            if (!config().value("allowExec").toBool()) { reply(403, QJsonObject{{"error", "exec on StarAgents is off (agents.config.allowExec)"}}); return; }
            QJsonObject b; b["command"] = args.value("command").toString(); b["shell"] = args.value("shell").toString("powershell"); b["timeoutMs"] = qBound(1000, args.value("timeoutMs").toInt(60000), 600000);
            if (b.value("command").toString().trimmed().isEmpty()) { reply(400, QJsonObject{{"error", "command required"}}); return; }
            m_star->call(node, "POST", "/exec", b, reply, 620000);
        } else reply(400, QJsonObject{{"error", "unknown command"}});
        return;
    }
    if (kind == "starnode") { m_nodes->action(agent.mid(5), cmd, args, reply); return; }
    if (kind == "llm") {
        QJsonArray messages;
        messages.append(QJsonObject{{"role", "system"}, {"content", "You are the operator assistant of PLAY PLAY, a TV playout station. Answer from the tool results only; never guess what is on air. "
                                                          "Use tools to look things up. Keep answers short and concrete. Action tools change the broadcast: use one only when the operator explicitly asks for that change."}});
        messages.append(QJsonObject{{"role", "user"}, {"content", t->o.value("prompt").toString()}});
        llmStep(t, messages, 0); return;
    }
    reply(400, QJsonObject{{"error", "unknown agent kind"}});
}

void AgentSuite::llmStep(Task* t, QJsonArray messages, int round) {
    const QString tid = t->o.value("id").toString();
    const QJsonObject l = llmConfig(); const bool allowActions = config().value("allowActions").toBool();
    QJsonArray tools;
    for (const QJsonValue& v : suites::operatorCommands()) {
        const QJsonObject c = v.toObject(); if (c.value("risk").toString() == "action" && !allowActions) continue;
        QJsonObject props; const QJsonObject p = c.value("params").toObject(); for (auto it = p.begin(); it != p.end(); ++it) props[it.key()] = QJsonObject{{"type", "string"}, {"description", it.value().toString()}};
        QJsonObject params; params["type"] = "object"; params["properties"] = props;
        QJsonObject fn; fn["name"] = c.value("name"); fn["parameters"] = params;
        fn["description"] = c.value("desc").toString() + (c.value("risk").toString() == "action" ? QString(" (CHANGES THE BROADCAST)") : QString());
        QJsonObject tool; tool["type"] = "function"; tool["function"] = fn; tools.append(tool);
    }
    QJsonObject body; body["model"] = l.value("model"); body["messages"] = messages; body["tools"] = tools; body["max_tokens"] = 900; body["temperature"] = 0.2;
    m_http->json("POST", l.value("baseUrl").toString() + "/chat/completions", {{"Authorization", "Bearer " + l.value("apiKey").toString().toUtf8()}, {"X-Title", "PLAY PLAY agents"}}, body, 90000,
                 [this, tid, messages, round](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) mutable {
        Task* t = find(tid); if (!t || t->cancelled) return;
        if (http != 200) { finish(t, "failed", QJsonValue(), "LLM: " + SuiteHttp::errorText(http, j, raw, e)); return; }
        m_usage["lastModel"] = j.value("model"); m_usage["calls"] = m_usage.value("calls").toInt() + 1;
        const QJsonObject msg = j.value("choices").toArray().at(0).toObject().value("message").toObject();
        const QJsonArray calls = msg.value("tool_calls").toArray();
        if (calls.isEmpty() || round >= 6) { QJsonObject r; r["answer"] = msg.value("content").toString(); r["model"] = j.value("model"); r["rounds"] = round; finish(t, "done", r); return; }
        messages.append(msg);
        auto left = std::make_shared<int>(calls.size()); auto results = std::make_shared<QJsonArray>();
        for (const QJsonValue& cv : calls) {
            const QJsonObject c = cv.toObject(); const QString name = c.value("function").toObject().value("name").toString();
            const QJsonObject args = QJsonDocument::fromJson(c.value("function").toObject().value("arguments").toString().toUtf8()).object();
            const QString callId = c.value("id").toString();
            QJsonArray steps = t->o.value("steps").toArray(); steps.append(QJsonObject{{"round", round}, {"tool", name}, {"args", args}}); t->o["steps"] = steps; changed(t);
            auto onResult = [this, tid, callId, name, left, results, messages, round](int status, const QJsonObject& res) mutable {
                Task* t2 = find(tid); if (!t2 || t2->cancelled) return;
                QString content = QString::fromUtf8(QJsonDocument(res).toJson(QJsonDocument::Compact)); if (content.size() > 6000) content = content.left(6000) + "...(truncated)";
                if (status >= 400) content = "ERROR: " + res.value("error").toString(content);
                results->append(QJsonObject{{"role", "tool"}, {"tool_call_id", callId}, {"name", name}, {"content", content}});
                if (--*left == 0) { QJsonArray next = messages; for (const QJsonValue& r : *results) next.append(r); llmStep(t2, next, round + 1); }
            };
            if (suites::operatorCommandIsAction(name) && !config().value("allowActions").toBool()) { onResult(403, QJsonObject{{"error", "action commands are off for the assistant"}}); continue; }
            if (!m_op) { onResult(503, QJsonObject{{"error", "operator commands not wired"}}); continue; }
            m_op(name, args, onResult);
        }
    });
}

void AgentSuite::runOperator(const QString& command, const QJsonObject& args, Reply reply) {
    bool known = false; for (const QJsonValue& v : suites::operatorCommands()) if (v.toObject().value("name").toString() == command) known = true;
    if (!known) { reply(400, QJsonObject{{"error", "unknown command " + command}}); return; }
    QJsonObject b; b["agent"] = "station"; b["command"] = command; b["args"] = args; b["actor"] = "station-api";
    const QJsonObject t = submit(b);
    if (t.contains("error")) { reply(t.value("status").toInt(400), t); return; }
    const QString id = t.value("id").toString();
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(this, &AgentSuite::event, this, [this, id, reply, conn](const QString&, const QJsonObject& p) {
        const QJsonObject tk = p.value("task").toObject(); if (tk.value("id").toString() != id || !finishedState(tk.value("state").toString())) return;
        disconnect(*conn);
        if (tk.value("state").toString() == "done") reply(200, tk.value("result").toObject()); else reply(502, QJsonObject{{"error", "command failed: " + tk.value("error").toString()}});
    });
}

void AgentSuite::llmTest(Reply reply) {
    const QJsonObject l = llmConfig();
    if (l.value("apiKey").toString().isEmpty()) { reply(400, QJsonObject{{"error", "OpenRouter key not configured"}}); return; }
    QJsonObject body; body["model"] = l.value("model"); body["max_tokens"] = 50; body["messages"] = QJsonArray{QJsonObject{{"role", "user"}, {"content", "Reply with exactly: PLAYPLAY OK"}}};
    m_http->json("POST", l.value("baseUrl").toString() + "/chat/completions", {{"Authorization", "Bearer " + l.value("apiKey").toString().toUtf8()}}, body, 60000,
                 [this, reply](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) {
        if (http != 200) { reply(502, QJsonObject{{"error", "AI test failed: " + SuiteHttp::errorText(http, j, raw, e)}}); return; }
        m_usage["lastModel"] = j.value("model");
        reply(200, QJsonObject{{"ok", true}, {"reply", j.value("choices").toArray().at(0).toObject().value("message").toObject().value("content").toString().trimmed().left(80)}, {"model", j.value("model")}});
    });
}

QJsonObject AgentSuite::aiStatus() const {
    const QJsonObject l = llmConfig();
    return QJsonObject{{"available", !l.value("apiKey").toString().isEmpty()}, {"key_source", AiModels::keyStatus(m_store).value("keySource")},
                       {"model", l.value("model")}, {"analysis_chain", QJsonArray{l.value("model")}}, {"report_chain", QJsonArray{l.value("model")}}, {"auto_enrich", false}, {"usage", m_usage}};
}
