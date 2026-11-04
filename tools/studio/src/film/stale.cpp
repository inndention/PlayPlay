#include "film/stale.h"

#include "core/json.h"

#include <QJsonArray>
#include <QSet>

#include <algorithm>

namespace studio::film {

namespace {

QStringList idsOf(const QJsonValue& v) {
    QStringList out;
    if (v.isString() && !v.toString().isEmpty()) out << v.toString();
    for (const QJsonValue& e : v.toArray())
        if (!e.toString().isEmpty()) out << e.toString();
    return out;
}

const Node* sceneOf(const Film& f, const Node& n) {
    const Node* p = f.parentOf(n);
    return p && p->kind == NodeKind::Scene ? p : nullptr;
}

// A shot shows its own characters / location, else its scene's (an empty list of its own means "the scene's").
QStringList shotCharacters(const Film& f, const Node& n) {
    QStringList own = idsOf(n.data.value(QStringLiteral("characters")));
    if (!own.isEmpty()) return own;
    const Node* s = sceneOf(f, n);
    return s ? idsOf(s->data.value(QStringLiteral("characters"))) : QStringList();
}
QString shotLocation(const Film& f, const Node& n) {
    const QString own = n.data.value(QStringLiteral("location")).toString();
    if (!own.isEmpty()) return own;
    const Node* s = sceneOf(f, n);
    return s ? s->data.value(QStringLiteral("location")).toString() : QString();
}
// A line is spoken with its own voice, else its speaker's.
QString lineVoice(const Film& f, const Node& n) {
    const QString own = n.data.value(QStringLiteral("voice")).toString();
    if (!own.isEmpty()) return own;
    const BibleEntry* sp = f.entry(n.data.value(QStringLiteral("speaker")).toString());
    return sp ? sp->data.value(QStringLiteral("voice")).toString() : QString();
}

bool usesStyle(NodeKind k) { return k == NodeKind::Sequence || k == NodeKind::Shot || k == NodeKind::Cue; }

QJsonValue one(HashCache& c, const QString& id) {
    if (id.isEmpty()) return QJsonValue(QJsonValue::Null);
    const QString h = c.entry(id);
    return QJsonArray{id, h.isEmpty() ? QStringLiteral("missing") : h};
}
QJsonArray many(HashCache& c, const QStringList& ids) {
    QJsonArray a;
    for (const QString& id : ids) a.append(one(c, id));
    return a;
}

} // namespace

QString HashCache::entry(const QString& id) {
    const auto it = m_entries.constFind(id);
    if (it != m_entries.constEnd()) return it.value();
    const BibleEntry* e = m_film.entry(id);
    const QString h = e ? entryHash(*e) : QString();
    m_entries.insert(id, h);
    return h;
}

QString HashCache::style() {
    if (m_styleDone) return m_style;
    QList<const BibleEntry*> styles = m_film.entries(BibleKind::Style);
    std::sort(styles.begin(), styles.end(), [](const BibleEntry* a, const BibleEntry* b) { return a->id < b->id; });
    QJsonArray a;
    for (const BibleEntry* e : styles) a.append(QJsonArray{e->id, entry(e->id)});
    m_style = QString::fromLatin1(json::stableHash(a));
    m_styleDone = true;
    return m_style;
}

QJsonObject inputsOf(const Film& f, const Node& n, HashCache* cache) {
    HashCache local(f);
    HashCache& c = cache ? *cache : local;
    QJsonObject o{
        {QStringLiteral("kind"), kindName(n.kind)},
        {QStringLiteral("f"), hashedFields(nodeFields(n.kind), n.data)},
    };
    if (usesStyle(n.kind)) o.insert(QStringLiteral("style"), c.style());
    switch (n.kind) {
    case NodeKind::Sequence:
        o.insert(QStringLiteral("aspect"), f.settings().aspect);
        break;
    case NodeKind::Shot: {
        const Node* seq = f.ancestorOfKind(n.id, NodeKind::Sequence);
        o.insert(QStringLiteral("characters"), many(c, shotCharacters(f, n)));
        o.insert(QStringLiteral("location"), one(c, shotLocation(f, n)));
        o.insert(QStringLiteral("props"), many(c, idsOf(n.data.value(QStringLiteral("props")))));
        o.insert(QStringLiteral("aspect"), f.settings().aspect);
        o.insert(QStringLiteral("video_model"),
                 seq ? seq->data.value(QStringLiteral("video_model")).toString() : QString());
        break;
    }
    case NodeKind::Line:
        o.insert(QStringLiteral("voice"), one(c, lineVoice(f, n)));
        o.insert(QStringLiteral("language"), f.settings().language);
        break;
    case NodeKind::Scene:
    case NodeKind::Beat:
    case NodeKind::Cue:
        break;
    }
    return o;
}

QString inputHash(const Film& f, const Node& n, HashCache* cache) {
    return QString::fromLatin1(json::stableHash(inputsOf(f, n, cache)));
}

QStringList entriesUsed(const Film& f, const Node& n) {
    QStringList out;
    switch (n.kind) {
    case NodeKind::Shot: {
        out << shotCharacters(f, n);
        const QString loc = shotLocation(f, n);
        if (!loc.isEmpty()) out << loc;
        out << idsOf(n.data.value(QStringLiteral("props")));
        break;
    }
    case NodeKind::Line: {
        const QString v = lineVoice(f, n);
        if (!v.isEmpty()) out << v;
        break;
    }
    default:
        break;
    }
    return out;
}

QStringList keysUsed(const Film& f, const Node& n) {
    QStringList out;
    for (const QString& id : entriesUsed(f, n)) out << QStringLiteral("bible:") + id;
    // a line reads its speaker's voice pointer
    const QString speaker = n.kind == NodeKind::Line ? n.data.value(QStringLiteral("speaker")).toString() : QString();
    if (!speaker.isEmpty()) out << QStringLiteral("bible:") + speaker;
    if (usesStyle(n.kind)) {
        out << QStringLiteral("style");
        for (const BibleEntry* e : f.entries(BibleKind::Style)) out << QStringLiteral("bible:") + e->id;
    }
    if (n.kind == NodeKind::Sequence || n.kind == NodeKind::Shot) out << QStringLiteral("aspect");
    if (n.kind == NodeKind::Line) out << QStringLiteral("language");
    out.removeDuplicates();
    return out;
}

namespace {

StaleReport rehash(Film& f, const QStringList& ids) {
    StaleReport r;
    QHash<QString, QString> fresh;                   // id -> new hash (computed before anything is written)
    {
        HashCache c(f);
        for (const QString& id : ids) {
            const Node* n = f.node(id);
            if (!n) continue;
            const QString h = inputHash(f, *n, &c);
            if (h == n->inputHash) continue;
            if (f.lockedAt(id)) {
                r.lockedDrift << id;                 // automation never touches a locked node
                continue;
            }
            fresh.insert(id, h);
            r.changed << id;
        }
    }
    for (const QString& id : std::as_const(r.changed)) {
        Node* m = f.nodeMut(id);
        m->inputHash = fresh.value(id);
        m->stale = true;
    }
    return r;
}

} // namespace

StaleReport propagate(Film& f) { return rehash(f, f.allIds()); }

Changes changesBetween(const Film& before, const Film& after) {
    Changes c;
    for (const QString& id : after.allIds()) {
        const Node* a = after.node(id);
        const Node* b = before.node(id);
        if (!b || b->kind != a->kind || b->parentId != a->parentId || b->locked != a->locked || b->data != a->data)
            c.roots << id;
    }
    auto entry = [&](const BibleEntry& e) {
        c.entries.insert(e.id);
        if (e.kind == BibleKind::Style) c.style = true;
    };
    for (const BibleEntry* e : before.entries()) {
        const BibleEntry* n = after.entry(e->id);
        if (!n) entry(*e);
        else if (entryHash(*e) != entryHash(*n)
                 || e->data.value(QStringLiteral("voice")) != n->data.value(QStringLiteral("voice"))) {
            entry(*e);
            entry(*n);
        }
    }
    for (const BibleEntry* e : after.entries())
        if (!before.entry(e->id)) entry(*e);
    c.aspect = before.settings().aspect != after.settings().aspect;
    c.language = before.settings().language != after.settings().language;
    return c;
}

StaleReport propagate(Film& f, const Changes& c) {
    if (c.empty()) return {};
    QStringList ids;
    QSet<QString> seen;
    auto add = [&](const QString& id) {
        if (!seen.contains(id)) {
            seen.insert(id);
            ids << id;
        }
    };
    for (const QString& r : c.roots)
        for (const QString& id : f.subtree(r)) add(id);
    if (c.style || c.aspect || c.language || !c.entries.isEmpty()) {
        for (const QString& id : f.allIds()) {
            const Node* n = f.node(id);
            if ((c.style && usesStyle(n->kind)) || (c.aspect && (n->kind == NodeKind::Sequence || n->kind == NodeKind::Shot))
                || (c.language && n->kind == NodeKind::Line)) {
                add(id);
                continue;
            }
            if (c.entries.isEmpty()) continue;
            // a line also reads its speaker (whose voice it is spoken with)
            if (n->kind == NodeKind::Line && c.entries.contains(n->data.value(QStringLiteral("speaker")).toString())) {
                add(id);
                continue;
            }
            for (const QString& e : entriesUsed(f, *n))
                if (c.entries.contains(e)) {
                    add(id);
                    break;
                }
        }
    }
    return rehash(f, ids);
}

bool markFresh(Film& f, const QString& id, const QString& usedHash) {
    const Node* n = f.node(id);
    if (!n || f.lockedAt(id) || usedHash.isEmpty()) return false;
    const QString h = inputHash(f, *n);
    if (h != usedHash) return false;
    Node* m = f.nodeMut(id);
    m->inputHash = h;
    m->stale = false;
    return true;
}

Users usersOf(const Film& f, const QString& entryId) {
    Users u;
    for (const QString& id : f.allIds()) {
        const Node* n = f.node(id);
        for (const FieldSpec& s : nodeFields(n->kind)) {
            if (s.type != FieldType::Ref && s.type != FieldType::Refs) continue;
            if (idsOf(n->data.value(s.name)).contains(entryId)) {
                u.nodes << id;
                break;
            }
        }
    }
    for (const BibleEntry* e : f.entries(BibleKind::Character))
        if (e->data.value(QStringLiteral("voice")).toString() == entryId) u.entries << e->id;
    return u;
}

QStringList affectedBy(const Film& f, const QString& entryId) {
    QStringList out;
    const BibleEntry* e = f.entry(entryId);
    const bool style = e && e->kind == BibleKind::Style;
    for (const QString& id : f.allIds()) {
        const Node* n = f.node(id);
        if ((style && usesStyle(n->kind)) || entriesUsed(f, *n).contains(entryId)) out << id;
    }
    return out;
}

} // namespace studio::film
