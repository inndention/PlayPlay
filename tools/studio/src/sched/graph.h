#pragma once
// The dynamic graph scheduler (ST-09, docs/plans/starmedia-studio.md §4.3 layer 4): a port of starmedia src/sched.ts
// runGraph, rewritten around a ready queue so a 10k-node film graph costs O(log N) per event instead of a rescan of
// every node per completion, and extended with run-time expansion and per-node persistence hooks.
//
// It knows nothing about models, money or files. The caller's hooks decide those things, with the TS names:
//   skip(n)     decided as soon as a node's dependencies finished, before any capacity is looked at (and again when the
//               node is looked at for a start: a required step failed meanwhile → skip). Timing differs from the TS,
//               which asks every round for every dep-satisfied node: here a node whose kind is at cap, whose lane is
//               busy or that waits for a step is not looked at until there is room, so its skipped() (and its
//               dependents' skips) may land a round later and after that round's afterRound(). It never runs either
//               way; progress reporting (ST-12) should not count on a skip landing in the same round as the failure.
//   defer(n)    true = not now: the node stays in the ready queue and is looked at again after the next node settles
//               (the executor holds back parallel paid steps until a first one has shown the estimates are right)
//   admit(n)    decided when there is room to start the node, just before it starts (the budget ceiling)
//   reserve(n)  runs synchronously the moment the node is admitted, before the next node is looked at, so the next
//               admit() already counts what this one will spend
//   run(n)      starts the work. It is called synchronously from the scheduling round (so its bookkeeping lands before
//               the next node is looked at) and must not block: it starts a request / a process / a timer and later
//               calls settle(). It must not throw; if it does, failed(n, error) records it and the node counts as
//               finished, the others go on.
//   skipped / failed / settled / afterRound / stopped   as in the TS (graph.h Hooks below)
//   stateChanged(n, state)   new: every transition, so the executor persists each node (jobs.state) and a restart can
//               rebuild the ready queue from the DB: feed the rows back as GraphNode{state = persisted} (below).
//
// Rules (the TS rules, kept):
//   - a node starts only after all its dependencies finished (finished = done, failed, skipped, cancelled or unknown);
//     a dependency that is not part of the graph counts as finished;
//   - at most `steps` nodes run at once, at most `perKind[kind]` of one kind (a node whose kind is full is passed over
//     and the next one is looked at, so 12 narration lines cannot keep 12 slots while the images wait), at most one per
//     `lane`;
//   - nodes are looked at in a deterministic order: higher priority first, then orderKey, then the order they were
//     added in. With no priorities and no order keys that is the list order, as in the TS, so the order they START in
//     (and which of them a budget ceiling cuts) is deterministic;
//   - one node failing never stops another; Stop (the AbortSignal) starts nothing new, lets the running ones wind down
//     (they hold the signal too and cancel their provider calls), then ends the graph with the caller's stopped() error;
//   - a node is started at most once;
//   - a graph with nothing running, nothing to start and nodes left ends with "scheduler stalled (dependency cycle?)".
//
// New here:
//   - expand(parent, children): a running writing node adds scene / shot nodes. Children may depend on the parent, on
//     each other and on any node; what already waited on the parent (an "assemble" node) also waits on the children
//     (dependentsWait), so the film DAG grows under a fixed tail. Children of an earlier expand() of the same parent
//     are siblings, not the tail: a second expansion (shots, then the dialogue lines that depend on the shots) gives
//     them no edge from the new children. Each node remembers its parent in GraphNode::parent (persist it with the
//     row, so a rebuild keeps the distinction).
//   - persistence: GraphNode::state carries the persisted state. Terminal rows count as finished and never run again;
//     running / polling rows (the process died with them in flight) are offered to run() again with that state visible,
//     so the executor resumes polling instead of paying twice (ST-12).
//
// Timing: everything happens on the Graph's thread, in the round that start(), settle() or expand() triggers. Like the
// TS (where a completion is a microtask after the synchronous round), a settle() called from inside run() is queued and
// applied after the round. finished() may therefore fire from inside start() (an empty or fully synchronous graph).
// settleLater() is the thread-safe entry for workers.
#include "provider/gates.h"   // AbortSignal

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <deque>
#include <functional>
#include <optional>
#include <set>
#include <vector>

namespace studio::sched {

/// §4.3: pending → ready → admitted → running → (polling) → done | failed | skipped | cancelled | unknown.
enum class NodeState { Pending, Ready, Admitted, Running, Polling, Done, Failed, Skipped, Cancelled, Unknown };
QString stateName(NodeState s);
bool parseState(QStringView text, NodeState* out);
/// done | failed | skipped | cancelled | unknown (the same list as store jobs::isTerminal).
bool isTerminal(NodeState s);
/// admitted | running | polling: the node holds a slot.
bool isInFlight(NodeState s);

constexpr int kBaseSteps = 4;              // concurrency.mjs BASE.steps
/// TS clampSteps: `n` when it is >= 1, else `d`.
int clampSteps(int n, int d);

struct GraphNode {
    QString id;
    QStringList deps;                      // ids that must finish first (unknown ids count as finished)
    QString kind;                          // perKind cap key ("" = no kind)
    QString lane;                          // at most one running per lane ("" = none)
    int priority = 0;                      // higher first
    QString orderKey;                      // then this (plain string order), then the order given
    QString parent;                        // the node expand() added it under ("" = none); set by expand(), honoured
                                           // by add() on a rebuild (an unknown id counts as none)
    NodeState state = NodeState::Pending;  // the persisted state on a rebuild; Pending for a new node
    QVariant user;                         // the caller's payload (a job id, a plan stage); never read here
};

struct GraphLimits {
    int steps = kBaseSteps;
    QHash<QString, int> perKind;           // kind → cap (a cap < 1 means `steps`, as in the TS)
};

/// How a graph ended.
struct Outcome {
    bool ok = false;
    bool stopped = false;                  // the AbortSignal ended it (error = stopped())
    QString error;                         // empty when ok
};

struct Hooks {
    /// A reason → the node finishes as skipped without running (skipped(n, why) is called).
    std::function<std::optional<QString>(const GraphNode&)> skip;
    std::function<bool(const GraphNode&)> defer;
    /// A reason → skipped, decided with a slot free, just before the start.
    std::function<std::optional<QString>(const GraphNode&)> admit;
    std::function<void(const GraphNode&)> reserve;
    /// Required. Starts the work; the node settles later through settle() / settleLater().
    std::function<void(const GraphNode&)> run;
    /// The node finished without running; `why` is the reason from skip() or admit().
    std::function<void(const GraphNode&, const QString& why)> skipped;
    /// run() threw (it should not): the error text; the node then settles as failed.
    std::function<void(const GraphNode&, const QString& error)> failed;
    /// Called once a node's run() has settled, before its slot is given back.
    std::function<void(const GraphNode&)> settled;
    /// Called after every round of starts that followed a completion; a non-empty text ends the run with that error
    /// (the running nodes wind down first).
    std::function<QString()> afterRound;
    /// The error text when the signal fired (default "aborted").
    std::function<QString()> stopped;
    /// Every state transition of every node (persistence). Terminal rows fed in at add() are not reported.
    std::function<void(const GraphNode&, NodeState)> stateChanged;
    /// The graph ended (once). It may delete the graph, synchronously or with deleteLater(): the hook and the outcome
    /// are copied before the call and nothing of the graph is touched after it.
    std::function<void(const Outcome&)> finished;
};

/// Process-wide counters (TS schedMetrics).
struct KindStat {
    int running = 0;
    int started = 0;
};
struct Metrics {
    int runsActive = 0;                    // graphs in progress right now
    int stepsRunning = 0;                  // nodes running right now, across graphs
    int peakStepsRunning = 0;
    qint64 started = 0, finished = 0, skipped = 0;
    qint64 failed = 0;                     // settled as failed, or run() threw (the TS counts only the throws)
    QHash<QString, KindStat> byKind;
};
Metrics metrics();
/// Starts the counters over (tests); the live gauges (runsActive, stepsRunning) are kept.
void resetMetrics();
/// The limits a graph uses when it is not given its own (ST-10's profile sets them). Junk steps fall back to
/// kBaseSteps.
void setDefaults(const GraphLimits& l);
GraphLimits defaults();

class Graph : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Running, Finished };

    /// `limits`: nullopt = the process defaults at construction time.
    explicit Graph(Hooks hooks, std::optional<GraphLimits> limits = std::nullopt, provider::AbortSignal signal = {},
                   QObject* parent = nullptr);
    ~Graph() override;

    /// Adds nodes (before start(), or at any time while running: nodes without a parent). All or nothing: an empty
    /// or duplicate id refuses the whole list with `error`. Terminal persisted states count as finished at once.
    bool add(const QList<GraphNode>& nodes, QString* error = nullptr);
    bool add(const GraphNode& node, QString* error = nullptr) { return add(QList<GraphNode>{node}, error); }
    /// Run-time expansion: adds `children` under `parentId` (which must exist and not have finished). With
    /// `dependentsWait`, every node that depends on the parent and has not started also gets an edge from each child,
    /// except the parent's earlier children (siblings). All or nothing, like add().
    bool expand(const QString& parentId, const QList<GraphNode>& children, QString* error = nullptr,
                bool dependentsWait = true);

    /// Runs the first round. A signal already aborted ends the graph at once with stopped(), nothing started.
    void start();
    /// The node's run() finished with `state` (Done, Failed, Cancelled or Unknown; anything else is read as Done).
    /// Graph thread only. False when the node is not in flight (never started, settled already): nothing happens.
    bool settle(const QString& id, NodeState state = NodeState::Done, const QString& error = {});
    /// settle() from any thread (queued to the graph's thread).
    void settleLater(const QString& id, NodeState state = NodeState::Done, const QString& error = {});
    /// Running → Polling (the provider accepted a long job; ST-12 persists the provider id first). Graph thread only.
    bool setPolling(const QString& id);

    State state() const { return m_state; }
    bool isFinished() const { return m_state == State::Finished; }
    const Outcome& outcome() const { return m_outcome; }
    const provider::AbortSignal& signal() const { return m_signal; }
    const GraphLimits& limits() const { return m_limits; }

    const GraphNode* node(const QString& id) const;
    std::optional<NodeState> nodeState(const QString& id) const;
    /// Dependencies in the graph that have not finished (-1 for an unknown id).
    int indegree(const QString& id) const;
    /// The ready queue in the order the next round looks at it (diagnostics, tests).
    QStringList readyIds() const;
    int size() const { return int(m_nodes.size()); }
    int running() const { return m_running; }
    int unfinished() const { return m_unfinished; }
    /// Ids of the nodes in flight (admitted, running, polling), in start order.
    QStringList inFlightIds() const;

private:
    struct Key {
        int negPriority;
        QString orderKey;
        int seq;
        bool operator<(const Key& o) const {
            if (negPriority != o.negPriority) return negPriority < o.negPriority;
            if (orderKey != o.orderKey) return orderKey < o.orderKey;
            return seq < o.seq;
        }
    };
    struct Node {
        GraphNode def;
        int seq = 0;
        NodeState state = NodeState::Pending;
        int indegree = 0;
        std::vector<int> dependents;
        int parent = -1;                   // the node expand() added it under (def.parent resolved)
        int cls = 0;
        bool inReady = false;
        int deferredRound = -1;
        qint64 startOrder = -1;
    };
    struct Class {                         // the ready nodes of one (kind, lane): skipped whole when the kind is full
        QString kind, lane;                // or the lane busy, so a round costs O(examined · log classes)
        std::set<Key> ready;
        std::set<Key>::iterator cursor;
    };
    struct Completion {
        int idx;
        NodeState state;
        QString error;
    };

    int classFor(const QString& kind, const QString& lane);
    bool validate(const QList<GraphNode>& nodes, QString* error) const;
    void insert(const QList<GraphNode>& nodes, int parent, bool dependentsWait);
    void makeReady(int idx, std::vector<Completion>& work);
    void finishNodes(std::vector<Completion> work);
    void putReady(int idx);
    void takeReady(int idx);
    bool kindFull(const QString& kind) const;
    bool round();
    void pump();
    void applyQueued();
    void startNode(Node& n);
    void finish(Outcome o);
    void setState(Node& n, NodeState s);
    QString stoppedText() const;

    Hooks m_hooks;
    GraphLimits m_limits;
    int m_steps = kBaseSteps;
    provider::AbortSignal m_signal;
    provider::AbortSignal::ListenerId m_abortListener = 0;
    State m_state = State::Idle;
    Outcome m_outcome;
    std::deque<Node> m_nodes;              // stable references: a hook may add nodes while one is being looked at
    QHash<QString, int> m_index;
    std::deque<Class> m_classes;           // stable too: run() may expand the graph with a new (kind, lane)
    QHash<QString, int> m_classIndex;      // kind + '\0' + lane
    QHash<QString, int> m_kindRunning;
    QHash<QString, int> m_laneRunning;
    int m_running = 0;
    int m_unfinished = 0;
    int m_round = 0;
    qint64 m_startCounter = 0;
    bool m_pumping = false;
    bool m_repump = false;
    bool m_dirty = false;                  // a ready set changed under a round: rebuild its cursors
    bool m_afterRoundDue = false;
    QString m_endError;                    // afterRound asked for the end
    std::vector<Completion> m_queued;      // settle() calls made during a round
};

} // namespace studio::sched
