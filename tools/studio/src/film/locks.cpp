#include "film/locks.h"

#include "core/clock.h"
#include "core/json.h"
#include "film/stale.h"

#include <QJsonArray>

#include <algorithm>

namespace studio::film {

namespace {

QString capitalised(QString s) {
    if (!s.isEmpty()) s[0] = s[0].toUpper();
    return s;
}
QString quoted(const QString& t) { return QChar(0x201C) + t + QChar(0x201D); }

/// A node's content as an edit sees it (runlocks.ts compares whole scenes, outputs included): kind, parent, data,
/// locked, stale and the chosen take. Its position among its siblings, its hash and stamps are not content.
bool sameContent(const Node& a, const Node& b) {
    return a.kind == b.kind && a.parentId == b.parentId && a.data == b.data && a.locked == b.locked
        && a.stale == b.stale && a.chosenTakeId == b.chosenTakeId;
}

/// Is `x` (a node of prev or next) held by a lock naming `nodes`: one of them, or under one of them?
bool covered(const QStringList& nodes, const QString& x, const Film& prev, const Film* next = nullptr) {
    for (const QString& a : nodes)
        if (a == x || prev.isAncestor(a, x) || (next && next->isAncestor(a, x))) return true;
    return false;
}

QString fieldWords(const QString& key, const Film* f) {
    if (key == QLatin1String("aspect")) return QStringLiteral("the aspect ratio");
    if (key == QLatin1String("language")) return QStringLiteral("the language");
    if (key == QLatin1String("target_seconds")) return QStringLiteral("the target length");
    if (key == QLatin1String("style")) return QStringLiteral("the style");
    if (key.startsWith(QLatin1String("bible:"))) {
        const QString id = key.mid(6);
        const BibleEntry* e = f ? f->entry(id) : nullptr;
        if (e && e->kind == BibleKind::Style) return QStringLiteral("the style");
        return e ? QStringLiteral("the ") + bibleKindName(e->kind) + QLatin1Char(' ') + quoted(e->name) : quoted(id);
    }
    if (key.startsWith(QLatin1String("anchor:"))) {
        const QString id = key.mid(7);
        return QStringLiteral("the anchor picture of ") + (f && f->node(id) ? f->nameOf(id) : QStringLiteral("sequence ") + id);
    }
    return quoted(key);
}

QString joinAnd(const QStringList& w) {
    if (w.size() <= 1) return w.value(0);
    return w.mid(0, w.size() - 1).join(QStringLiteral(", ")) + QStringLiteral(" and ") + w.last();
}

QString fieldMessage(const LockInfo& lock, const QStringList& fields, const Film* f) {
    QStringList w;
    for (const QString& k : fields) {
        const QString x = fieldWords(k, f);
        if (!w.contains(x)) w << x;
    }
    const bool many = w.size() > 1;
    return capitalised(QStringLiteral("%1 can't change while %2 is running: it uses %3 for the shots being made and would "
                                      "mix two versions in one video. Wait for it to finish, or press Stop.")
                           .arg(joinAnd(w), lock.label, many ? QStringLiteral("them") : QStringLiteral("it")));
}

QString nodeMessage(const LockInfo& lock, const QStringList& ids, const Film& f,
                    const QList<QPair<QString, LockInfo>>& others = {}) {
    QString s = QStringLiteral("%1 %2 busy (%3). Wait for it to finish, or press Stop; everything else can still be edited.")
                    .arg(namesForSentence(f, ids), ids.size() > 1 ? QStringLiteral("are") : QStringLiteral("is"), lock.label);
    if (!others.isEmpty()) {
        QStringList o;
        for (const auto& [id, by] : others) o << QStringLiteral("%1 (%2)").arg(f.nameOf(id), by.label);
        s += QStringLiteral(" Also busy: %1.").arg(o.join(QStringLiteral(", ")));
    }
    return s;
}

/// Nodes of a diff in a stable order: prev's pre-order, then what only next has.
QStringList ordered(const QSet<QString>& ids, const Film& prev, const Film& next) {
    QStringList out;
    QSet<QString> seen;
    auto add = [&](const QString& id) {
        if (ids.contains(id) && !seen.contains(id)) {
            seen.insert(id);
            out << id;
        }
    };
    for (const QString& id : prev.allIds()) add(id);
    for (const QString& id : next.allIds()) add(id);
    QStringList rest(ids.begin(), ids.end());   // unreachable nodes of a damaged film, last
    rest.sort();
    for (const QString& id : rest) add(id);
    return out;
}

/// Did an entry change in a way a generation can see? Its hash (kind, name, hashed fields, reference images) or, for a
/// character, the voice its lines are spoken with. Notes are not seen.
bool entrySeenChanged(const BibleEntry& a, const BibleEntry& b) {
    return entryHash(a) != entryHash(b)
        || a.data.value(QStringLiteral("voice")) != b.data.value(QStringLiteral("voice"));
}

} // namespace

// ================================================================ comparing films

bool isSharedWrite(QStringView key) { return key.startsWith(u"anchor:"); }

QString canon(const QJsonValue& v) { return QString::fromUtf8(json::stable(v)); }

QJsonObject nodeInputs(const Node& n) {
    return QJsonObject{
        {QStringLiteral("id"), n.id},
        {QStringLiteral("kind"), kindName(n.kind)},
        {QStringLiteral("parent"), n.parentId},
        {QStringLiteral("locked"), n.locked},
        {QStringLiteral("data"), withoutOutputs(nodeFields(n.kind), n.data)},
    };
}

bool sameInputs(const Node& a, const Node& b) { return canon(nodeInputs(a)) == canon(nodeInputs(b)); }

FilmDiff diffFilm(const Film& prev, const Film& next) {
    FilmDiff d;
    const QStringList prevIds = prev.allIds(), nextIds = next.allIds();
    for (const QString& id : prevIds) {
        const Node* a = prev.node(id);
        const Node* b = next.node(id);
        if (!b || !sameContent(*a, *b)) d.nodes.insert(id);
        if (a->kind == NodeKind::Sequence) {
            const QJsonValue x = a->data.value(QStringLiteral("anchor_asset"));
            const QJsonValue y = b ? b->data.value(QStringLiteral("anchor_asset")) : QJsonValue();
            if (x != y) d.fields.insert(QStringLiteral("anchor:") + id);
        }
    }
    for (const QString& id : nextIds) {
        if (prev.node(id)) continue;
        d.nodes.insert(id);
        const Node* b = next.node(id);
        if (b->kind == NodeKind::Sequence && b->data.contains(QStringLiteral("anchor_asset")))
            d.fields.insert(QStringLiteral("anchor:") + id);
    }
    // the order of the children both films share, per parent
    QStringList parents{QString()};
    parents << prevIds;
    for (const QString& p : parents) {
        const QStringList was = prev.children(p), now = next.children(p);
        if (was.isEmpty() || now.isEmpty()) continue;
        QStringList a, b;
        for (const QString& x : was)
            if (now.contains(x)) a << x;
        for (const QString& x : now)
            if (was.contains(x)) b << x;
        if (a != b) d.reorderedParents.insert(p);
    }
    auto bible = [&](const BibleEntry* e) {
        d.fields.insert(QStringLiteral("bible:") + e->id);
        if (e->kind == BibleKind::Style) d.fields.insert(QStringLiteral("style"));
    };
    for (const BibleEntry* e : prev.entries()) {
        const BibleEntry* n = next.entry(e->id);
        if (!n || entrySeenChanged(*e, *n)) {
            bible(e);
            if (n) bible(n);
        }
    }
    for (const BibleEntry* e : next.entries())
        if (!prev.entry(e->id)) bible(e);
    if (prev.settings().aspect != next.settings().aspect) d.fields.insert(QStringLiteral("aspect"));
    if (prev.settings().language != next.settings().language) d.fields.insert(QStringLiteral("language"));
    if (prev.settings().targetSeconds != next.settings().targetSeconds) d.fields.insert(QStringLiteral("target_seconds"));
    return d;
}

LockScope generationScope(const Film& f, const QStringList& nodes) {
    LockScope s;
    s.nodes = nodes;
    QStringList writes, reads;
    for (const QString& root : nodes)
        for (const QString& id : f.subtree(root)) {
            const Node* n = f.node(id);
            reads << keysUsed(f, *n);
            if (const Node* seq = f.ancestorOfKind(id, NodeKind::Sequence)) writes << QStringLiteral("anchor:") + seq->id;
        }
    writes.removeDuplicates();
    reads.removeDuplicates();
    writes.sort();
    reads.sort();
    s.writes = writes;
    s.reads = reads;
    return s;
}

QString namesForSentence(const Film& f, const QStringList& ids) {
    if (ids.isEmpty()) return {};
    const QStringList shown = ids.mid(0, 3);
    std::optional<NodeKind> kind;
    bool same = true;
    for (const QString& id : ids) {
        const Node* n = f.node(id);
        if (!n) {
            same = false;
            break;
        }
        if (!kind) kind = n->kind;
        else if (*kind != n->kind) same = false;
    }
    QStringList parts;
    QString s;
    if (same && kind) {
        for (const QString& id : shown) {
            const QString full = f.nameOf(id);              // "shot 1.2.3 “t”" -> "1.2.3 “t”"
            parts << full.mid(kindName(*kind).size() + 1);
        }
        s = kindName(*kind) + (ids.size() > 1 ? QStringLiteral("s ") : QStringLiteral(" ")) + parts.join(QStringLiteral(", "));
    } else {
        for (const QString& id : shown) parts << f.nameOf(id);
        s = parts.join(QStringLiteral(", "));
    }
    if (ids.size() > 3) s += QStringLiteral(" and %1 more").arg(ids.size() - 3);
    return capitalised(s);
}

QString wholeFilmMessage(const LockInfo& lock) {
    return QStringLiteral("The film can't be changed while %1 runs: it rewrites the whole film. Wait for it to finish, "
                          "or press Stop.")
        .arg(lock.label);
}

QJsonObject LockInfo::toJson() const {
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("projectId"), projectId},
        {QStringLiteral("owner"), owner},
        {QStringLiteral("kind"), kind == LockKind::Run ? QStringLiteral("run") : QStringLiteral("reserve")},
        {QStringLiteral("phase"), phase},
        {QStringLiteral("label"), label},
        {QStringLiteral("since"), double(since)},
        {QStringLiteral("film"), film},
        {QStringLiteral("nodes"), QJsonArray::fromStringList(nodes)},
        {QStringLiteral("writes"), QJsonArray::fromStringList(writes)},
        {QStringLiteral("reads"), QJsonArray::fromStringList(reads)},
    };
}

QJsonObject Conflict::toJson() const {
    static const char* kinds[] = {"film", "node", "field"};
    return QJsonObject{
        {QStringLiteral("kind"), QString::fromLatin1(kinds[int(kind)])},
        {QStringLiteral("lock"), lock.toJson()},
        {QStringLiteral("nodeIds"), QJsonArray::fromStringList(nodeIds)},
        {QStringLiteral("fields"), QJsonArray::fromStringList(fields)},
        {QStringLiteral("message"), message},
    };
}

// ================================================================ the registry

RunLocks::RunLocks(const Clock* clock) : m_clock(clock ? clock : &Clock::system()) {}

int RunLocks::onChange(std::function<void()> fn) {
    const int id = ++m_nextListener;
    m_listeners.emplace(id, std::move(fn));
    return id;
}

void RunLocks::removeListener(int id) { m_listeners.erase(id); }

void RunLocks::changed() {
    const auto copy = m_listeners;
    for (const auto& [id, fn] : copy) {
        try {
            if (fn) fn();
        } catch (...) {
            // a listener must never break a lock
        }
    }
}

LockInfo RunLocks::info(const Held& h) {
    LockInfo i;
    i.id = h.id;
    i.projectId = h.projectId;
    i.owner = h.owner;
    i.kind = h.kind;
    i.phase = h.phase;
    i.label = h.label;
    i.since = h.since;
    i.film = h.film;
    i.nodes = h.nodes;
    i.writes = h.writes;
    i.reads = h.reads;
    return i;
}

QList<LockInfo> RunLocks::list(const QString& projectId) const {
    QList<LockInfo> out;
    for (const auto& [n, h] : m_heldByN)
        if (projectId.isEmpty() || h.projectId == projectId) out << info(h);
    return out;
}

std::optional<LockInfo> RunLocks::get(const QString& id) const {
    const auto it = m_held.constFind(id);
    if (it == m_held.constEnd()) return std::nullopt;
    return info(m_heldByN.at(it.value()));
}

int RunLocks::runCount(const QString& projectId) const {
    int c = 0;
    for (const auto& [n, h] : m_heldByN)
        if (h.kind == LockKind::Run && (projectId.isEmpty() || h.projectId == projectId)) ++c;
    return c;
}

bool RunLocks::writerActive(const QString& projectId) const {
    for (const auto& [n, h] : m_heldByN)
        if (h.kind == LockKind::Run && h.projectId == projectId && writes(h)) return true;
    return false;
}

QHash<QString, QList<LockInfo>> RunLocks::heldNodes(const QString& projectId, const QString& forOwner) const {
    QHash<QString, QList<LockInfo>> out;
    for (const auto& [n, h] : m_heldByN) {
        if (h.projectId != projectId || (!forOwner.isEmpty() && ownReserve(h, forOwner))) continue;
        for (const QString& id : h.nodes) out[id] << info(h);
    }
    return out;
}

std::optional<LockInfo> RunLocks::filmLock(const QString& projectId) const {
    for (const auto& [n, h] : m_heldByN)
        if (h.projectId == projectId && h.film) return info(h);
    return std::nullopt;
}

std::optional<Conflict> RunLocks::conflictBetween(const Held& mine, const LockSpec& spec, const Held& h,
                                                  const Film* film) const {
    Conflict c;
    c.lock = info(h);
    if (mine.film || h.film) {
        const QString cap = capitalised(spec.label);
        c.kind = Conflict::Kind::Film;
        c.message = h.film ? QStringLiteral("%1 can't start while %2 is rewriting the whole film. Wait for it to finish, "
                                            "or press Stop.")
                                 .arg(cap, h.label)
                           : QStringLiteral("%1 rewrites the whole film, so it can't start while %2 is running. Wait for "
                                            "it to finish, or press Stop.")
                                 .arg(cap, h.label);
        return c;
    }
    QStringList sc;
    for (const QString& b : mine.nodes)
        for (const QString& a : h.nodes)
            if (a == b || (film && (film->isAncestor(a, b) || film->isAncestor(b, a)))) {
                sc << b;
                break;
            }
    if (!sc.isEmpty()) {
        c.kind = Conflict::Kind::Node;
        c.nodeIds = sc;
        c.message = film ? nodeMessage(c.lock, sc, *film)
                         : QStringLiteral("%1 %2 busy (%3). Wait for it to finish, or press Stop; everything else can still be edited.")
                               .arg(sc.join(QStringLiteral(", ")), sc.size() > 1 ? QStringLiteral("are") : QStringLiteral("is"), h.label);
        return c;
    }
    QStringList fl;
    for (const QString& f : mine.writes)
        if (!isSharedWrite(f) && (h.writes.contains(f) || h.reads.contains(f)) && !fl.contains(f)) fl << f;
    for (const QString& f : mine.reads)
        if (h.writes.contains(f) && !isSharedWrite(f) && !fl.contains(f)) fl << f;
    if (!fl.isEmpty()) {
        c.kind = Conflict::Kind::Field;
        c.fields = fl;
        c.message = fieldMessage(c.lock, fl, film);
        return c;
    }
    return std::nullopt;
}

std::optional<Conflict> RunLocks::canAcquire(const LockSpec& spec, const Film* film) const {
    Held mine;
    mine.film = spec.scope.film;
    mine.nodes = spec.scope.nodes;
    mine.writes = spec.scope.writes;
    mine.reads = spec.scope.reads;
    if (!writes(mine)) return std::nullopt;   // a step that only reads is never in the way
    for (const auto& [n, h] : m_heldByN) {
        if (h.projectId != spec.projectId || !writes(h)) continue;
        // a side task's reservation and its own steps share the nodes
        if (h.owner == spec.owner && h.kind != spec.kind) continue;
        if (std::optional<Conflict> c = conflictBetween(mine, spec, h, film)) return c;
    }
    return std::nullopt;
}

Acquired RunLocks::acquire(const LockSpec& spec, const Film* film) {
    Acquired r;
    if (std::optional<Conflict> c = canAcquire(spec, film)) {
        r.conflict = *c;
        return r;
    }
    Held h;
    h.projectId = spec.projectId;
    h.owner = spec.owner;
    h.kind = spec.kind;
    h.phase = spec.phase;
    h.label = spec.label;
    h.since = m_clock->nowMs();
    h.film = spec.scope.film;
    h.nodes = spec.scope.nodes;
    h.writes = spec.scope.writes;
    h.reads = spec.scope.reads;
    h.nodes.removeDuplicates();
    h.writes.removeDuplicates();
    h.reads.removeDuplicates();
    const int n = ++m_n;
    h.id = QStringLiteral("lk%1").arg(n);
    m_heldByN.emplace(n, h);
    m_held.insert(h.id, n);
    changed();
    r.ok = true;
    r.id = h.id;
    return r;
}

bool RunLocks::release(const QString& id) {
    const auto it = m_held.find(id);
    if (it == m_held.end()) return false;
    m_heldByN.erase(it.value());
    m_held.erase(it);
    changed();
    return true;
}

bool RunLocks::releaseNodes(const QString& id, const QStringList& nodeIds) {
    const auto it = m_held.constFind(id);
    if (it == m_held.constEnd()) return false;
    Held& h = m_heldByN.at(it.value());
    bool any = false;
    for (const QString& s : nodeIds) any = h.nodes.removeAll(s) > 0 || any;
    if (any) changed();
    return any;
}

void RunLocks::narrowNodes(const QString& id, const QStringList& keep) {
    const auto it = m_held.constFind(id);
    if (it == m_held.constEnd()) return;
    Held& h = m_heldByN.at(it.value());
    const qsizetype before = h.nodes.size();
    h.nodes.erase(std::remove_if(h.nodes.begin(), h.nodes.end(), [&](const QString& s) { return !keep.contains(s); }),
                  h.nodes.end());
    if (h.nodes.size() != before) changed();
}

std::optional<Conflict> RunLocks::checkEdit(const QString& projectId, const Film& prev, const Film& next,
                                            const QString& owner) const {
    QList<const Held*> others;
    for (const auto& [n, h] : m_heldByN)
        if (h.projectId == projectId && !ownReserve(h, owner) && writes(h)) others << &h;
    if (others.isEmpty()) return std::nullopt;
    const FilmDiff d = diffFilm(prev, next);
    if (d.empty()) return std::nullopt;
    for (const Held* h : others)
        if (h->film) {
            Conflict c;
            c.kind = Conflict::Kind::Film;
            c.lock = info(*h);
            c.message = wholeFilmMessage(c.lock);
            return c;
        }
    // held nodes that the edit changes, adds something under, or removes
    const QStringList changedNodes = ordered(d.nodes, prev, next);
    QList<QPair<const Held*, QStringList>> groups;
    for (const Held* h : others) {
        QStringList ids;
        for (const QString& x : changedNodes)
            if (covered(h->nodes, x, prev, &next)) ids << x;
        if (!ids.isEmpty()) groups << qMakePair(h, ids);
    }
    if (!groups.isEmpty()) {
        Conflict c;
        c.kind = Conflict::Kind::Node;
        c.lock = info(*groups[0].first);
        QList<QPair<QString, LockInfo>> also;
        for (int g = 0; g < groups.size(); ++g)
            for (const QString& id : groups[g].second) {
                if (!c.nodeIds.contains(id)) c.nodeIds << id;
                if (g > 0) also << qMakePair(id, info(*groups[g].first));
            }
        // name nodes as the person saw them (prev); a node only next has is named from next
        QStringList known, added;
        for (const QString& id : groups[0].second) (prev.node(id) ? known : added) << id;
        c.message = nodeMessage(c.lock, known.isEmpty() ? added : known, known.isEmpty() ? next : prev, also);
        return c;
    }
    // film-level keys a step writes or reads (the anchor is shared by runs, not by edits)
    for (const Held* h : others) {
        QStringList fl;
        for (const QString& f : d.fields)
            if (h->writes.contains(f) || h->reads.contains(f)) fl << f;
        if (!fl.isEmpty()) {
            fl.sort();
            Conflict c;
            c.kind = Conflict::Kind::Field;
            c.lock = info(*h);
            c.fields = fl;
            c.message = fieldMessage(c.lock, fl, &prev);
            return c;
        }
    }
    // the tree rule: a held node's inputs never change under its step (a bible entry it shows, the scene location it
    // inherits, its sequence's video model), whether or not the step listed them
    HashCache cp(prev), cn(next);
    for (const Held* h : others)
        for (const QString& root : h->nodes)
            for (const QString& x : prev.subtree(root)) {
                const Node* a = prev.node(x);
                const Node* b = next.node(x);
                if (!b || inputHash(prev, *a, &cp) == inputHash(next, *b, &cn)) continue;
                Conflict c;
                c.lock = info(*h);
                c.nodeIds = {x};
                QStringList keys;
                for (const QString& k : keysUsed(prev, *a) + keysUsed(next, *b))
                    if (d.fields.contains(k) && !keys.contains(k)) keys << k;
                if (!keys.isEmpty()) {
                    keys.sort();
                    c.kind = Conflict::Kind::Field;
                    c.fields = keys;
                    c.message = fieldMessage(c.lock, keys, &prev) + QStringLiteral(" (%1 uses it.)").arg(prev.nameOf(x));
                    return c;
                }
                QStringList causes;
                for (const QString& anc : changedNodes)
                    if (prev.isAncestor(anc, x)) causes << prev.nameOf(anc);
                c.kind = Conflict::Kind::Node;
                c.message = QStringLiteral("%1 is busy (%2), and this edit changes what it is made from%3. Wait for it to "
                                           "finish, or press Stop; everything else can still be edited.")
                                .arg(namesForSentence(prev, {x}), h->label,
                                     causes.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(causes.join(QStringLiteral(", "))));
                return c;
            }
    return std::nullopt;
}

std::optional<Conflict> RunLocks::checkNodes(const QString& projectId, const QStringList& nodeIds,
                                             const QString& owner, const Film* film) const {
    for (const auto& [n, h] : m_heldByN) {
        if (h.projectId != projectId || ownReserve(h, owner) || !writes(h)) continue;
        Conflict c;
        c.lock = info(h);
        if (h.film) {
            c.kind = Conflict::Kind::Film;
            c.message = wholeFilmMessage(c.lock);
            return c;
        }
        QStringList sc;
        for (const QString& x : nodeIds)
            for (const QString& a : h.nodes)
                if (a == x || (film && (film->isAncestor(a, x) || film->isAncestor(x, a)))) {
                    sc << x;
                    break;
                }
        if (!sc.isEmpty()) {
            c.kind = Conflict::Kind::Node;
            c.nodeIds = sc;
            c.message = film ? nodeMessage(c.lock, sc, *film)
                             : QStringLiteral("%1 %2 busy (%3). Wait for it to finish, or press Stop; everything else can still be edited.")
                                   .arg(sc.join(QStringLiteral(", ")), sc.size() > 1 ? QStringLiteral("are") : QStringLiteral("is"), h.label);
            return c;
        }
    }
    return std::nullopt;
}

} // namespace studio::film
