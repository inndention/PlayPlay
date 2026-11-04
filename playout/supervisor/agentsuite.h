#ifndef AGENTSUITE_H
#define AGENTSUITE_H

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QTimer>
#include <functional>

class StarClient;
class StarNodes;
class SuiteHttp;
namespace sched { class ScheduleStore; }

/**
 * AgentSuite — the StarAgent suite (docs/agents.md): one task runner over every agent this station can drive:
 * the station operator command table, StarAgent daemons (StarClient), enrolled Star nodes (StarNodes allowlist)
 * and an LLM assistant that answers a prompt with a tool loop over the operator commands.
 * Tasks persist (settings agents.tasks, newest 300); one task per agent runs at a time.
 */
class AgentSuite : public QObject {
    Q_OBJECT
public:
    using Reply = std::function<void(int status, const QJsonObject& body)>;
    using OpFn = std::function<void(const QString& command, const QJsonObject& args, Reply reply)>;
    AgentSuite(sched::ScheduleStore* store, StarClient* star, StarNodes* nodes, QObject* parent = nullptr);
    void setOperator(OpFn fn) { m_op = std::move(fn); }
    void start();

    QJsonObject config(bool withSecrets = false) const;
    QJsonObject setConfig(const QJsonObject& patch);
    QJsonArray agents() const;
    QJsonArray tasks(const QString& agent = QString(), const QString& state = QString(), int limit = 100) const;
    QJsonObject task(const QString& id, bool* found = nullptr) const;
    QJsonObject submit(const QJsonObject& body);
    bool removeTask(const QString& id);
    int clearFinished();
    void runOperator(const QString& command, const QJsonObject& args, Reply reply);   // /api/op/exec
    void llmTest(Reply reply);                                                         // /api/ai/test
    QJsonObject aiStatus() const;

signals:
    void event(const QString& type, const QJsonObject& payload);

private:
    struct Task { QJsonObject o; bool cancelled = false; };
    sched::ScheduleStore* m_store;
    StarClient* m_star;
    StarNodes* m_nodes;
    SuiteHttp* m_http;
    OpFn m_op;
    QList<Task*> m_tasks;
    QTimer m_pump, m_persist;
    QJsonObject m_usage;

    Task* find(const QString& id) const;
    QJsonObject llmConfig() const;
    void pump();
    void run(Task* t);
    void finish(Task* t, const QString& state, const QJsonValue& result, const QString& error = QString());
    void llmStep(Task* t, QJsonArray messages, int round);
    void changed(Task* t);
    void persist();
};

#endif // AGENTSUITE_H
