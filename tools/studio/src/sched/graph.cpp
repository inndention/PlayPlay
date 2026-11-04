// The dynamic graph scheduler (ST-09). See graph.h for the rules; this file keeps the TS runGraph structure (a round
// of starts, then wait for a completion, then the next round) on top of per-class ready sets.
#include "sched/graph.h"

#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <algorithm>
#include <exception>
#include <mutex>

#include "core/log.h"

namespace studio::sched {

namespace {

constexpr QStringView kCat = u"sched";

struct Global {
    std::mutex mutex;
    Metrics m;
    GraphLimits defaults;
};
Global& global() {
    static Global* g = new Global;        // leaked on purpose: graphs may still settle while statics are destroyed
    return *g;
}

QString stoppedDefault() { return QStringLiteral("aborted"); }

} // namespace

// ---------------- states and limits ----------------

QString stateName(NodeState s) {
    switch (s) {
    case NodeState::Pending: return QStringLiteral("pending");
    case NodeState::Ready: return QStringLiteral("ready");
    case NodeState::Admitted: return QStringLiteral("admitted");
    case NodeState::Running: return QStringLiteral("running");
    case NodeState::Polling: return QStringLiteral("polling");
    case NodeState::Done: return QStringLiteral("done");
    case NodeState::Failed: return QStringLiteral("failed");
    case NodeState::Skipped: return QStringLiteral("skipped");
    case NodeState::Cancelled: return QStringLiteral("cancelled");
    case NodeState::Unknown: return QStringLiteral("unknown");
    }
    return QStringLiteral("pending");
}

bool parseState(QStringView text, NodeState* out) {
    static const NodeState all[] = {NodeState::Pending, NodeState::Ready, NodeState::Admitted, NodeState::Running,
                                    NodeState::Polling, NodeState::Done, NodeState::Failed, NodeState::Skipped,
                                    NodeState::Cancelled, NodeState::Unknown};
    for (NodeState s : all) {
        if (text == stateName(s)) {
            if (out) *out = s;
            return true;
        }
    }
    return false;
}

bool isTerminal(NodeState s) {
    return s == NodeState::Done || s == NodeState::Failed || s == NodeState::Skipped || s == NodeState::Cancelled
        || s == NodeState::Unknown;
}

bool isInFlight(NodeState s) {
    return s == NodeState::Admitted || s == NodeState::Running || s == NodeState::Polling;
}

int clampSteps(int n, int d) { return n >= 1 ? n : d; }

Metrics metrics() {
    Global& g = global();
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.m;
}

void resetMetrics() {
    Global& g = global();
    std::lock_guard<std::mutex> lock(g.mutex);
    g.m.started = g.m.finished = g.m.skipped = g.m.failed = 0;
    g.m.peakStepsRunning = g.m.stepsRunning;
    for (auto it = g.m.byKind.begin(); it != g.m.byKind.end(); ++it) it->started = 0;
}

void setDefaults(const GraphLimits& l) {
    Global& g = global();
    std::lock_guard<std::mutex> lock(g.mutex);
    g.defaults.steps = clampSteps(l.steps, kBaseSteps);
    g.defaults.perKind = l.perKind;
}

GraphLimits defaults() {
    Global& g = global();
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.defaults;
}

// ---------------- Graph ----------------

Graph::Graph(Hooks hooks, std::optional<GraphLimits> limits, provider::AbortSignal signal, QObject* parent)
    : QObject(parent), m_hooks(std::move(hooks)), m_limits(limits ? *limits : defaults()), m_signal(std::move(signal)) {
    m_steps = clampSteps(m_limits.steps, kBaseSteps);
    Q_ASSERT_X(bool(m_hooks.run), "sched::Graph", "the run hook is required");
    if (m_signal.valid() && !m_signal.aborted()) {
        // The abort may come from any thread; the pump runs on ours. A running node settles later anyway (it holds
        // the signal), so this only covers a Stop with nothing in flight.
        QPointer<Graph> self(this);
        m_abortListener = m_signal.onAbort([self] {
            if (!self) return;
            QMetaObject::invokeMethod(self.data(), [self] { if (self && self->m_state == State::Running) self->pump(); },
                                      Qt::QueuedConnection);
        });
    }
}

Graph::~Graph() {
    if (m_abortListener) m_signal.removeListener(m_abortListener);
    if (m_state == State::Running) {
        // the graph goes away with nodes in flight (shutdown): the gauges must not count them for ever
        Global& g = global();
        std::lock_guard<std::mutex> lock(g.mutex);
        g.m.runsActive = std::max(0, g.m.runsActive - 1);
        g.m.stepsRunning = std::max(0, g.m.stepsRunning - m_running);
        for (auto it = m_kindRunning.cbegin(); it != m_kindRunning.cend(); ++it) {
            auto& ks = g.m.byKind[it.key()];
            ks.running = std::max(0, ks.running - it.value());
        }
    }
}

int Graph::classFor(const QString& kind, const QString& lane) {
    const QString key = kind + QChar(u'\0') + lane;
    auto it = m_classIndex.constFind(key);
    if (it != m_classIndex.cend()) return it.value();
    Class c;
    c.kind = kind;
    c.lane = lane;
    c.cursor = c.ready.end();
    m_classes.push_back(std::move(c));
    const int idx = int(m_classes.size()) - 1;
    m_classes[idx].cursor = m_classes[idx].ready.begin();
    m_classIndex.insert(key, idx);
    return idx;
}

bool Graph::validate(const QList<GraphNode>& nodes, QString* error) const {
    QSet<QString> seen;
    for (const GraphNode& n : nodes) {
        if (n.id.isEmpty()) {
            if (error) *error = QStringLiteral("a node has no id");
            return false;
        }
        if (m_index.contains(n.id) || seen.contains(n.id)) {
            if (error) *error = QStringLiteral("duplicate node id %1").arg(n.id);
            return false;
        }
        seen.insert(n.id);
    }
    return true;
}

bool Graph::add(const QList<GraphNode>& nodes, QString* error) {
    if (m_state == State::Finished) {
        if (error) *error = QStringLiteral("the graph has finished");
        return false;
    }
    if (!validate(nodes, error)) return false;
    insert(nodes, -1, false);
    return true;
}

bool Graph::expand(const QString& parentId, const QList<GraphNode>& children, QString* error, bool dependentsWait) {
    if (m_state == State::Finished) {
        if (error) *error = QStringLiteral("the graph has finished");
        return false;
    }
    const auto pit = m_index.constFind(parentId);
    if (pit == m_index.cend()) {
        if (error) *error = QStringLiteral("unknown parent %1").arg(parentId);
        return false;
    }
    if (isTerminal(m_nodes[size_t(pit.value())].state)) {
        if (error) *error = QStringLiteral("parent %1 has finished (%2)").arg(parentId, stateName(m_nodes[size_t(pit.value())].state));
        return false;
    }
    if (!validate(children, error)) return false;
    insert(children, pit.value(), dependentsWait);
    return true;
}

void Graph::insert(const QList<GraphNode>& nodes, int parent, bool dependentsWait) {
    const int first = int(m_nodes.size());
    // 1. the rows, so deps among the new nodes resolve whatever their order in the list
    for (const GraphNode& def : nodes) {
        Node n;
        n.def = def;
        n.seq = int(m_nodes.size());
        // a terminal persisted row never runs again; a ready / in-flight row (the process died with it in flight) is
        // scheduled again from pending, with the persisted state left in def.state for run() to see
        n.state = isTerminal(def.state) ? def.state : NodeState::Pending;
        n.cls = classFor(def.kind, def.lane);
        if (parent >= 0) {                    // expand(): the parent is the one given
            n.parent = parent;
            n.def.parent = m_nodes[size_t(parent)].def.id;
        } else if (!def.parent.isEmpty()) {   // add() of a persisted row: the parent it was expanded under, if known
            n.parent = m_index.value(def.parent, -1);
        }
        m_nodes.push_back(std::move(n));
        m_index.insert(def.id, m_nodes.back().seq);
    }
    // 2. the edges and in-degrees
    for (int i = first; i < int(m_nodes.size()); ++i) {
        Node& n = m_nodes[size_t(i)];
        if (isTerminal(n.state)) continue;
        ++m_unfinished;
        QSet<QString> seenDeps;
        for (const QString& d : n.def.deps) {
            if (seenDeps.contains(d)) continue;   // a repeated dep is one edge
            seenDeps.insert(d);
            const auto dit = m_index.constFind(d);
            if (dit == m_index.cend()) continue;  // not part of the graph: counts as finished
            Node& dep = m_nodes[size_t(dit.value())];
            if (dit.value() == i) {               // a self-dependency can never be satisfied (TS: never starts): the
                ++n.indegree;                     // node waits for ever and the graph reports a stall
                continue;
            }
            if (isTerminal(dep.state)) continue;
            dep.dependents.push_back(i);
            ++n.indegree;
        }
    }
    // 3. what waited on the parent also waits on its expansion
    if (parent >= 0 && dependentsWait) {
        Node& p = m_nodes[size_t(parent)];
        const std::vector<int> waiting = p.dependents;   // copy: the loop below grows dependents of the children
        for (int w : waiting) {
            if (w >= first) continue;              // a child that depends on the parent is not "what waited on it"
            Node& d = m_nodes[size_t(w)];
            if (d.parent == parent) continue;      // nor is a child of an earlier expand() of this parent: a sibling
                                                   // (shots, then dialogue lines that depend on the shots: an edge
                                                   // from the lines back to the shots would be a cycle)
            if (d.state != NodeState::Pending && d.state != NodeState::Ready) continue;   // started already: too late
            for (int i = first; i < int(m_nodes.size()); ++i) {
                Node& c = m_nodes[size_t(i)];
                if (isTerminal(c.state) || i == w) continue;
                c.dependents.push_back(w);
                ++d.indegree;
            }
            if (d.state == NodeState::Ready && d.indegree > 0) {   // cannot happen while the parent is unfinished; kept safe
                takeReady(w);
                setState(d, NodeState::Pending);
            }
        }
    }
    // 4. before start() the first round makes the ready nodes ready (in seq order); after it, now
    if (m_state == State::Running) {
        std::vector<Completion> work;
        for (int i = first; i < int(m_nodes.size()); ++i) {
            Node& n = m_nodes[size_t(i)];
            if (n.state == NodeState::Pending && n.indegree == 0) makeReady(i, work);
        }
        if (!work.empty()) finishNodes(std::move(work));
        m_dirty = true;
        if (!m_pumping) pump();
        else m_repump = true;
    }
}

void Graph::setState(Node& n, NodeState s) {
    n.state = s;
    if (m_hooks.stateChanged) {
        try { m_hooks.stateChanged(n.def, s); } catch (...) { /* a recorder that breaks does not stop the graph */ }
    }
}

void Graph::putReady(int idx) {
    Node& n = m_nodes[size_t(idx)];
    if (n.inReady) return;
    Class& c = m_classes[size_t(n.cls)];
    c.ready.insert(Key{-n.def.priority, n.def.orderKey, n.seq});
    n.inReady = true;
    m_dirty = true;
}

void Graph::takeReady(int idx) {
    Node& n = m_nodes[size_t(idx)];
    if (!n.inReady) return;
    Class& c = m_classes[size_t(n.cls)];
    c.ready.erase(Key{-n.def.priority, n.def.orderKey, n.seq});
    n.inReady = false;
    m_dirty = true;
}

void Graph::makeReady(int idx, std::vector<Completion>& work) {
    Node& n = m_nodes[size_t(idx)];
    setState(n, NodeState::Ready);
    if (m_hooks.skip) {
        std::optional<QString> why;
        try { why = m_hooks.skip(n.def); } catch (...) { why.reset(); }
        if (why) {
            // decided as soon as the dependencies finished, before any capacity is looked at
            if (m_hooks.skipped) {
                try { m_hooks.skipped(n.def, *why); } catch (...) {}
            }
            {
                Global& g = global();
                std::lock_guard<std::mutex> lock(g.mutex);
                ++g.m.skipped;
            }
            work.push_back(Completion{idx, NodeState::Skipped, *why});
            return;
        }
    }
    putReady(idx);
}

void Graph::finishNodes(std::vector<Completion> work) {
    // A worklist, not recursion: a chain of 10k skips must not need 10k stack frames.
    while (!work.empty()) {
        const Completion c = std::move(work.back());
        work.pop_back();
        Node& n = m_nodes[size_t(c.idx)];
        if (isTerminal(n.state)) continue;
        const bool wasInFlight = isInFlight(n.state);
        if (n.inReady) takeReady(c.idx);
        if (wasInFlight) {
            --m_running;
            {
                auto kit = m_kindRunning.find(n.def.kind);
                if (kit != m_kindRunning.end() && --kit.value() <= 0) m_kindRunning.erase(kit);
            }
            if (!n.def.lane.isEmpty()) {
                auto lit = m_laneRunning.find(n.def.lane);
                if (lit != m_laneRunning.end() && --lit.value() <= 0) m_laneRunning.erase(lit);
            }
            {
                Global& g = global();
                std::lock_guard<std::mutex> lock(g.mutex);
                --g.m.stepsRunning;
                ++g.m.finished;
                auto& ks = g.m.byKind[n.def.kind];
                ks.running = std::max(0, ks.running - 1);
            }
            if (m_hooks.settled) {
                try { m_hooks.settled(n.def); } catch (...) {}
            }
        }
        --m_unfinished;
        setState(n, c.state);
        for (int d : n.dependents) {
            Node& dn = m_nodes[size_t(d)];
            if (--dn.indegree == 0 && dn.state == NodeState::Pending && m_state == State::Running) makeReady(d, work);
        }
    }
}

bool Graph::kindFull(const QString& kind) const {
    const auto cap = m_limits.perKind.constFind(kind);
    if (cap == m_limits.perKind.cend()) return false;
    return m_kindRunning.value(kind, 0) >= clampSteps(cap.value(), m_steps);
}

QString Graph::stoppedText() const {
    if (m_hooks.stopped) {
        try {
            const QString t = m_hooks.stopped();
            if (!t.isEmpty()) return t;
        } catch (...) {}
    }
    return stoppedDefault();
}

void Graph::start() {
    if (m_state != State::Idle) return;
    m_state = State::Running;
    {
        Global& g = global();
        std::lock_guard<std::mutex> lock(g.mutex);
        ++g.m.runsActive;
    }
    if (m_signal.valid() && m_signal.aborted()) {
        // Stop before anything started: nothing runs at all
        Outcome o;
        o.stopped = true;
        o.error = stoppedText();
        finish(std::move(o));
        return;
    }
    std::vector<Completion> work;
    for (int i = 0; i < int(m_nodes.size()); ++i) {
        Node& n = m_nodes[size_t(i)];
        if (n.state == NodeState::Pending && n.indegree == 0) makeReady(i, work);
    }
    if (!work.empty()) finishNodes(std::move(work));
    pump();
}

bool Graph::settle(const QString& id, NodeState state, const QString& error) {
    Q_ASSERT_X(QThread::currentThread() == thread(), "sched::Graph::settle", "graph thread only (use settleLater)");
    const auto it = m_index.constFind(id);
    if (it == m_index.cend()) return false;
    Node& n = m_nodes[size_t(it.value())];
    if (!isInFlight(n.state)) return false;
    NodeState final = state;
    if (final != NodeState::Failed && final != NodeState::Cancelled && final != NodeState::Unknown) final = NodeState::Done;
    // settled twice before the round applied the first call: the first one counts
    for (const Completion& c : m_queued) {
        if (c.idx == it.value()) return false;
    }
    if (final == NodeState::Failed) {
        Global& g = global();
        std::lock_guard<std::mutex> lock(g.mutex);
        ++g.m.failed;
    }
    m_queued.push_back(Completion{it.value(), final, error});
    if (m_pumping) m_repump = true;
    else pump();
    return true;
}

void Graph::settleLater(const QString& id, NodeState state, const QString& error) {
    QPointer<Graph> self(this);
    QMetaObject::invokeMethod(this, [self, id, state, error] { if (self) self->settle(id, state, error); },
                              Qt::QueuedConnection);
}

bool Graph::setPolling(const QString& id) {
    const auto it = m_index.constFind(id);
    if (it == m_index.cend()) return false;
    Node& n = m_nodes[size_t(it.value())];
    if (n.state != NodeState::Running) return false;
    setState(n, NodeState::Polling);
    return true;
}

void Graph::startNode(Node& n) {
    takeReady(n.seq);
    setState(n, NodeState::Admitted);
    if (m_hooks.reserve) {
        try { m_hooks.reserve(n.def); } catch (...) {}
    }
    if (!n.def.lane.isEmpty()) ++m_laneRunning[n.def.lane];
    ++m_kindRunning[n.def.kind];
    ++m_running;
    n.startOrder = m_startCounter++;
    {
        Global& g = global();
        std::lock_guard<std::mutex> lock(g.mutex);
        ++g.m.started;
        ++g.m.stepsRunning;
        g.m.peakStepsRunning = std::max(g.m.peakStepsRunning, g.m.stepsRunning);
        auto& ks = g.m.byKind[n.def.kind];
        ++ks.running;
        ++ks.started;
    }
    setState(n, NodeState::Running);
    QString threw;
    bool failed = false;
    try {
        m_hooks.run(n.def);
    } catch (const std::exception& e) {
        failed = true;
        threw = QString::fromUtf8(e.what());
    } catch (...) {
        failed = true;
        threw = QStringLiteral("run() threw");
    }
    if (failed) {
        if (threw.isEmpty()) threw = QStringLiteral("run() threw");
        {
            Global& g = global();
            std::lock_guard<std::mutex> lock(g.mutex);
            ++g.m.failed;
        }
        if (m_hooks.failed) {
            try { m_hooks.failed(n.def, threw); } catch (...) { /* the recorder broke: the node still counts as finished */ }
        }
        bool queued = false;
        for (const Completion& c : m_queued) queued = queued || c.idx == n.seq;   // run() may have settled before it threw
        if (!queued) m_queued.push_back(Completion{n.seq, NodeState::Failed, threw});
    }
}

// One round of starts: the TS for-loop over the nodes, on the per-class ready sets. Returns whether a node started.
bool Graph::round() {
    ++m_round;
    bool started = false;
    // a min-heap of (key, class) over the classes that may start something now: the class cursors walk their sets
    std::set<std::pair<Key, int>> heads;
    auto rebuild = [&] {
        heads.clear();
        for (int c = 0; c < int(m_classes.size()); ++c) {
            Class& cls = m_classes[size_t(c)];
            cls.cursor = cls.ready.begin();
            if (cls.cursor != cls.ready.end()) heads.insert({*cls.cursor, c});
        }
        m_dirty = false;
    };
    auto pushNext = [&](int c) {
        Class& cls = m_classes[size_t(c)];
        if (cls.cursor != cls.ready.end()) heads.insert({*cls.cursor, c});
    };
    rebuild();
    // m_dirty with an empty heap: a skip cascade just made nodes ready (the heap was built before); rebuild and go on
    while (m_running < m_steps && (!heads.empty() || m_dirty)) {
        if (m_signal.valid() && m_signal.aborted()) break;   // Stop pressed while nodes were being started
        if (m_dirty) { rebuild(); continue; }
        const auto head = *heads.begin();
        heads.erase(heads.begin());
        const int c = head.second;
        Class& cls = m_classes[size_t(c)];
        if (cls.cursor == cls.ready.end()) continue;   // cannot happen after a rebuild; kept safe
        if (kindFull(cls.kind) || (!cls.lane.isEmpty() && m_laneRunning.value(cls.lane, 0) > 0)) continue;   // the whole class waits this round
        Node& n = m_nodes[size_t(cls.cursor->seq)];
        if (n.deferredRound == m_round) {   // looked at already this round
            ++cls.cursor;
            pushNext(c);
            continue;
        }
        // skip, looked at again now that the node is about to start (a required step may have failed meanwhile)
        if (m_hooks.skip) {
            std::optional<QString> why;
            try { why = m_hooks.skip(n.def); } catch (...) { why.reset(); }
            if (why) {
                if (m_hooks.skipped) {
                    try { m_hooks.skipped(n.def, *why); } catch (...) {}
                }
                {
                    Global& g = global();
                    std::lock_guard<std::mutex> lock(g.mutex);
                    ++g.m.skipped;
                }
                finishNodes({Completion{n.seq, NodeState::Skipped, *why}});   // sets m_dirty: the dependents may be ready now
                continue;
            }
        }
        if (m_hooks.defer) {
            bool later = false;
            try { later = m_hooks.defer(n.def); } catch (...) { later = false; }
            if (later) {
                n.deferredRound = m_round;
                ++cls.cursor;
                pushNext(c);
                continue;
            }
        }
        if (m_hooks.admit) {
            std::optional<QString> no;
            try { no = m_hooks.admit(n.def); } catch (...) { no.reset(); }
            if (no) {
                if (m_hooks.skipped) {
                    try { m_hooks.skipped(n.def, *no); } catch (...) {}
                }
                {
                    Global& g = global();
                    std::lock_guard<std::mutex> lock(g.mutex);
                    ++g.m.skipped;
                }
                finishNodes({Completion{n.seq, NodeState::Skipped, *no}});
                continue;
            }
        }
        // start it: startNode erases the node from its set; the cursor must not point at it afterwards
        const Key startedKey = *cls.cursor;
        ++cls.cursor;
        cls.ready.erase(startedKey);   // erasing another element keeps the cursor valid (std::set)
        n.inReady = false;             // so takeReady() inside startNode does nothing
        startNode(n);
        started = true;
        if (m_dirty) {       // run() expanded the graph: rebuild on the next turn
            continue;
        }
        if (!(kindFull(cls.kind) || (!cls.lane.isEmpty() && m_laneRunning.value(cls.lane, 0) > 0))) pushNext(c);
    }
    m_dirty = false;
    return started;
}

void Graph::applyQueued() {
    if (m_queued.empty()) return;
    std::vector<Completion> work;
    work.reserve(m_queued.size());
    // finishNodes pops from the back; keep the settle order
    for (auto it = m_queued.rbegin(); it != m_queued.rend(); ++it) work.push_back(*it);
    m_queued.clear();
    finishNodes(std::move(work));
    m_afterRoundDue = true;
}

void Graph::pump() {
    if (m_pumping) {
        m_repump = true;
        return;
    }
    m_pumping = true;
    QPointer<Graph> self(this);                // finished() may delete the graph
    while (m_state == State::Running) {
        m_repump = false;
        applyQueued();
        if (m_afterRoundDue && m_hooks.afterRound && m_endError.isEmpty()) {
            m_afterRoundDue = false;
            QString err;
            try { err = m_hooks.afterRound(); } catch (const std::exception& e) { err = QString::fromUtf8(e.what()); if (err.isEmpty()) err = QStringLiteral("afterRound threw"); } catch (...) { err = QStringLiteral("afterRound threw"); }
            if (!err.isEmpty()) m_endError = err;
        }
        m_afterRoundDue = false;
        const bool aborted = m_signal.valid() && m_signal.aborted();
        if (aborted || !m_endError.isEmpty()) {
            // start nothing new; the running nodes wind down, then the graph ends
            if (!m_queued.empty()) continue;   // a settle made under afterRound(): apply it first
            if (m_running == 0) {
                Outcome o;
                if (aborted) {
                    o.stopped = true;
                    o.error = stoppedText();
                } else {
                    o.error = m_endError;
                }
                finish(std::move(o));
            }
            break;
        }
        const bool started = round();
        // Stop fired inside a hook of this round (TS: the loop head re-checks aborted() after the for-loop): the head
        // ends the run with stopped() once nothing runs, instead of reading "nothing started" as a stall
        if (m_signal.valid() && m_signal.aborted()) continue;
        if (!m_queued.empty() || m_repump) continue;   // a synchronous settle or an expansion: the next round, now
        if (m_running == 0) {
            if (m_unfinished == 0) {
                Outcome o;
                o.ok = true;
                finish(std::move(o));
            } else if (!started) {
                Outcome o;
                o.error = QStringLiteral("scheduler stalled (dependency cycle?)");
                finish(std::move(o));
            }
            break;
        }
        break;   // wait for a completion
    }
    if (self) m_pumping = false;               // not after finished() deleted the graph
}

void Graph::finish(Outcome o) {
    if (m_state == State::Finished) return;
    m_state = State::Finished;
    m_outcome = std::move(o);
    {
        Global& g = global();
        std::lock_guard<std::mutex> lock(g.mutex);
        g.m.runsActive = std::max(0, g.m.runsActive - 1);
    }
    if (m_abortListener) {
        m_signal.removeListener(m_abortListener);
        m_abortListener = 0;
    }
    if (!m_outcome.ok) log::info(kCat, QStringLiteral("graph ended: %1").arg(m_outcome.error));
    if (m_hooks.finished) {
        // copies: the hook may delete the graph (and m_hooks, m_outcome with it); nothing of `this` is read after it
        const auto fin = m_hooks.finished;
        const Outcome o = m_outcome;
        try { fin(o); } catch (...) {}
    }
}

// ---------------- introspection ----------------

const GraphNode* Graph::node(const QString& id) const {
    const auto it = m_index.constFind(id);
    return it == m_index.cend() ? nullptr : &m_nodes[size_t(it.value())].def;
}

std::optional<NodeState> Graph::nodeState(const QString& id) const {
    const auto it = m_index.constFind(id);
    if (it == m_index.cend()) return std::nullopt;
    return m_nodes[size_t(it.value())].state;
}

int Graph::indegree(const QString& id) const {
    const auto it = m_index.constFind(id);
    return it == m_index.cend() ? -1 : m_nodes[size_t(it.value())].indegree;
}

QStringList Graph::readyIds() const {
    std::vector<std::pair<Key, int>> all;
    for (const Class& c : m_classes) {
        for (const Key& k : c.ready) all.push_back({k, k.seq});
    }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    QStringList out;
    out.reserve(int(all.size()));
    for (const auto& e : all) out.push_back(m_nodes[size_t(e.second)].def.id);
    return out;
}

QStringList Graph::inFlightIds() const {
    std::vector<const Node*> live;
    for (const Node& n : m_nodes) {
        if (isInFlight(n.state)) live.push_back(&n);
    }
    std::sort(live.begin(), live.end(), [](const Node* a, const Node* b) { return a->startOrder < b->startOrder; });
    QStringList out;
    for (const Node* n : live) out.push_back(n->def.id);
    return out;
}

} // namespace studio::sched
