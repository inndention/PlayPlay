#include "film/model.h"

#include "core/ids.h"
#include "core/json.h"
#include "film/locks.h"
#include "film/stale.h"

#include <QJsonDocument>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>

namespace studio::film {

namespace {

constexpr std::array<const char*, 6> kKinds{"sequence", "scene", "shot", "beat", "line", "cue"};
constexpr std::array<const char*, 6> kIdPrefix{"sq", "sc", "sh", "bt", "ln", "cu"};
constexpr std::array<const char*, 5> kBiblePrefix{"ch", "lo", "pr", "st", "vo"};
constexpr qint64 kMaxKey = 999999999999LL;

FieldSpec text(const char* name, int maxLen, bool hashed, QStringList aliases = {}) {
    FieldSpec f;
    f.name = QString::fromLatin1(name);
    f.maxLen = maxLen;
    f.hashed = hashed;
    f.aliases = std::move(aliases);
    return f;
}
FieldSpec number(const char* name, double min, double max, bool hashed, QStringList aliases = {}) {
    FieldSpec f = text(name, 0, hashed, std::move(aliases));
    f.type = FieldType::Number;
    f.min = min;
    f.max = max;
    return f;
}
FieldSpec choice(const char* name, QStringList values, bool hashed, QStringList aliases = {}) {
    FieldSpec f = text(name, 0, hashed, std::move(aliases));
    f.type = FieldType::Enum;
    f.values = std::move(values);
    return f;
}
FieldSpec ref(const char* name, BibleKind k, bool hashed, QStringList aliases = {}) {
    FieldSpec f = text(name, 0, hashed, std::move(aliases));
    f.type = FieldType::Ref;
    f.refKinds = {k};
    return f;
}
FieldSpec refs(const char* name, BibleKind k, int maxItems, bool hashed, QStringList aliases = {}) {
    FieldSpec f = text(name, maxItems, hashed, std::move(aliases));
    f.type = FieldType::Refs;
    f.refKinds = {k};
    return f;
}
FieldSpec output(const char* name, FieldType t, double max = 0) {
    FieldSpec f = text(name, 0, false);
    f.type = t;
    f.max = max;
    f.output = true;
    return f;
}

QString cut28(QString t) {
    t = t.simplified();
    return t.size() > 28 ? t.left(27) + QChar(0x2026) : t;
}
QString quoted(const QString& t) { return QChar(0x201C) + t + QChar(0x201D); }
QString capitalised(QString s) {
    if (!s.isEmpty()) s[0] = s[0].toUpper();
    return s;
}

QVariant txt(const QString& s) { return s.isNull() ? QVariant(QStringLiteral("")) : QVariant(s); }
QString jsonText(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }
QString jsonText(const QStringList& l) {
    return QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(l)).toJson(QJsonDocument::Compact));
}

} // namespace

// ================================================================ kinds and schema

QString kindName(NodeKind k) { return QString::fromLatin1(kKinds[size_t(k)]); }

std::optional<NodeKind> parseKind(QStringView s) {
    const QString t = s.trimmed().toString().toLower();
    for (size_t i = 0; i < kKinds.size(); ++i) {
        const QString n = QString::fromLatin1(kKinds[i]);
        if (t == n || t == n + QLatin1Char('s')) return NodeKind(i);
    }
    return std::nullopt;
}

const QList<NodeKind>& allKinds() {
    static const QList<NodeKind> all{NodeKind::Sequence, NodeKind::Scene, NodeKind::Shot,
                                     NodeKind::Beat,     NodeKind::Line,  NodeKind::Cue};
    return all;
}

bool canContain(std::optional<NodeKind> parent, NodeKind child) {
    switch (child) {
    case NodeKind::Sequence: return !parent;
    case NodeKind::Scene: return parent == NodeKind::Sequence;
    case NodeKind::Shot:
    case NodeKind::Beat: return parent == NodeKind::Scene;
    case NodeKind::Line: return parent == NodeKind::Scene || parent == NodeKind::Shot;
    case NodeKind::Cue: return parent == NodeKind::Sequence || parent == NodeKind::Scene || parent == NodeKind::Shot;
    }
    return false;
}

QString containmentRule(NodeKind child) {
    switch (child) {
    case NodeKind::Sequence: return QStringLiteral("a sequence is top level");
    case NodeKind::Scene: return QStringLiteral("a scene goes in a sequence");
    case NodeKind::Shot: return QStringLiteral("a shot goes in a scene");
    case NodeKind::Beat: return QStringLiteral("a beat goes in a scene");
    case NodeKind::Line: return QStringLiteral("a line goes in a scene or a shot");
    case NodeKind::Cue: return QStringLiteral("a cue goes in a sequence, a scene or a shot");
    }
    return {};
}

const QStringList& transitions() {
    static const QStringList t{QStringLiteral("cut"),   QStringLiteral("fade"),  QStringLiteral("dissolve"),
                               QStringLiteral("dip"),   QStringLiteral("slide"), QStringLiteral("wipe"),
                               QStringLiteral("zoom")};
    return t;
}
const QStringList& sources() {
    static const QStringList s{QStringLiteral("ai-video"), QStringLiteral("ai-image"), QStringLiteral("reference"),
                               QStringLiteral("upload"), QStringLiteral("text-card")};
    return s;
}
const QStringList& aspects() {
    static const QStringList a{QStringLiteral("9:16"), QStringLiteral("16:9"), QStringLiteral("1:1"),
                               QStringLiteral("4:5")};
    return a;
}

const QList<FieldSpec>& nodeFields(NodeKind k) {
    const QStringList name{QStringLiteral("name")};
    static const QList<FieldSpec> sequence{
        text("title", 200, false, name),
        text("summary", 4000, true, {QStringLiteral("description"), QStringLiteral("synopsis")}),
        text("video_model", 120, true, {QStringLiteral("model"), QStringLiteral("video")}),
        output("anchor_asset", FieldType::Asset),
    };
    static const QList<FieldSpec> scene{
        text("title", 200, false, name),
        text("summary", 4000, true,
             {QStringLiteral("description"), QStringLiteral("synopsis"), QStringLiteral("what_happens")}),
        ref("location", BibleKind::Location, true, {QStringLiteral("place"), QStringLiteral("setting")}),
        refs("characters", BibleKind::Character, 12, true, {QStringLiteral("cast"), QStringLiteral("who")}),
        text("time_of_day", 40, true, {QStringLiteral("time")}),
        text("mood", 200, true, {QStringLiteral("tone")}),
    };
    // characters / location / props of a shot are inputs through stale.cpp (their bible entries, or the scene's)
    static const QList<FieldSpec> shot{
        text("title", 200, false, name),
        text("visual", 4000, true,
             {QStringLiteral("description"), QStringLiteral("image"), QStringLiteral("picture"),
              QStringLiteral("prompt"), QStringLiteral("shot_description")}),
        text("camera", 400, true, {QStringLiteral("camera_move"), QStringLiteral("angle")}),
        number("duration", 1.5, 20, true,
               {QStringLiteral("seconds"), QStringLiteral("length"), QStringLiteral("length_seconds"),
                QStringLiteral("dur")}),
        choice("transition", transitions(), false),
        choice("source", sources(), true),
        refs("characters", BibleKind::Character, 12, false, {QStringLiteral("cast"), QStringLiteral("who")}),
        ref("location", BibleKind::Location, false, {QStringLiteral("place"), QStringLiteral("setting")}),
        refs("props", BibleKind::Prop, 12, false, {QStringLiteral("objects")}),
        text("on_screen", 300, false,
             {QStringLiteral("onscreen"), QStringLiteral("on_screen_text"), QStringLiteral("text_on_screen"),
              QStringLiteral("overlay"), QStringLiteral("headline")}),
    };
    static const QList<FieldSpec> beat{
        text("title", 200, false, name),
        text("summary", 2000, true, {QStringLiteral("description")}),
    };
    // speaker / voice are inputs through stale.cpp (the voice the line is spoken with)
    static const QList<FieldSpec> line{
        ref("speaker", BibleKind::Character, false,
            {QStringLiteral("character"), QStringLiteral("by"), QStringLiteral("speaker_id")}),
        text("text", 2000, true,
             {QStringLiteral("dialogue"), QStringLiteral("words"), QStringLiteral("says"), QStringLiteral("vo"),
              QStringLiteral("narration")}),
        text("delivery", 200, true, {QStringLiteral("how"), QStringLiteral("direction"), QStringLiteral("tone")}),
        ref("voice", BibleKind::Voice, false, {QStringLiteral("voice_id")}),
        output("voice_asset", FieldType::Asset),
        output("voice_duration", FieldType::Number, 600),
    };
    static const QList<FieldSpec> cue{
        choice("cue_kind", {QStringLiteral("music"), QStringLiteral("sfx"), QStringLiteral("ambience")}, true,
               {QStringLiteral("sound"), QStringLiteral("category")}),
        text("description", 1000, true, {QStringLiteral("prompt")}),
        number("at", 0, 86400, false, {QStringLiteral("start"), QStringLiteral("start_seconds")}),
        number("duration", 0, 600, true, {QStringLiteral("length"), QStringLiteral("seconds"), QStringLiteral("dur")}),
        output("audio_asset", FieldType::Asset),
    };
    switch (k) {
    case NodeKind::Sequence: return sequence;
    case NodeKind::Scene: return scene;
    case NodeKind::Shot: return shot;
    case NodeKind::Beat: return beat;
    case NodeKind::Line: return line;
    case NodeKind::Cue: return cue;
    }
    return shot;
}

bool sameRow(const Node& a, const Node& b) {
    return a.id == b.id && a.projectId == b.projectId && a.parentId == b.parentId && a.kind == b.kind
        && a.orderKey == b.orderKey && a.data == b.data && a.inputHash == b.inputHash && a.locked == b.locked
        && a.stale == b.stale && a.chosenTakeId == b.chosenTakeId;
}

QJsonObject Settings::toJson() const {
    QJsonObject o{{QStringLiteral("aspect"), aspect}, {QStringLiteral("language"), language}};
    if (targetSeconds > 0) o.insert(QStringLiteral("target_seconds"), targetSeconds);
    return o;
}

// ================================================================ order keys

namespace order {
QString key(qint64 v) { return QStringLiteral("%1").arg(v, kWidth, 10, QLatin1Char('0')); }
qint64 value(const QString& key) {
    if (key.size() != kWidth) return -1;
    for (QChar c : key)
        if (c < QLatin1Char('0') || c > QLatin1Char('9')) return -1;
    return key.toLongLong();
}
} // namespace order

// ================================================================ Film: reads

const Node* Film::node(const QString& id) const {
    const auto it = m_nodes.constFind(id);
    return it == m_nodes.constEnd() ? nullptr : &it.value();
}

Node* Film::nodeMut(const QString& id) {
    const auto it = m_nodes.find(id);
    return it == m_nodes.end() ? nullptr : &it.value();
}

const Node* Film::ancestorOfKind(const QString& id, NodeKind k) const {
    const Node* n = node(id);
    for (int guard = 0; n && guard < 64; ++guard) {
        if (n->kind == k) return n;
        n = parentOf(*n);
    }
    return nullptr;
}

bool Film::isAncestor(const QString& a, const QString& b) const {
    const Node* n = node(b);
    for (int guard = 0; n && !n->parentId.isEmpty() && guard < 64; ++guard) {
        if (n->parentId == a) return true;
        n = node(n->parentId);
    }
    return false;
}

void Film::collect(const QString& id, QStringList& out) const {
    out << id;
    const auto it = m_children.constFind(id);
    if (it == m_children.constEnd()) return;
    for (const QString& c : it.value())
        if (c != id && out.size() < m_nodes.size() + 1) collect(c, out);   // the size guard stops a damaged cycle
}

QStringList Film::subtree(const QString& id) const {
    QStringList out;
    if (node(id)) collect(id, out);
    return out;
}

QStringList Film::allIds() const {
    QStringList out;
    out.reserve(m_nodes.size());
    for (const QString& top : m_children.value(QString()))
        if (node(top)) collect(top, out);
    return out;
}

int Film::indexOf(const QString& id) const {
    const Node* n = node(id);
    return n ? int(m_children.value(n->parentId).indexOf(id)) : -1;
}

QString Film::pathOf(const QString& id) const {
    QStringList parts;
    const Node* n = node(id);
    for (int guard = 0; n && guard < 64; ++guard) {
        parts.prepend(QString::number(indexOf(n->id) + 1));
        n = parentOf(*n);
    }
    return parts.join(QLatin1Char('.'));
}

QString Film::nameOf(const QString& id) const {
    const Node* n = node(id);
    if (!n) return QStringLiteral("node ") + id;
    const QString t = cut28(n->title());
    QString s = kindName(n->kind) + QLatin1Char(' ') + pathOf(id);
    if (!t.isEmpty()) s += QLatin1Char(' ') + quoted(t);
    return s;
}

const Node* Film::lockedAt(const QString& id) const {
    const Node* n = node(id);
    for (int guard = 0; n && guard < 64; ++guard) {
        if (n->locked) return n;
        n = parentOf(*n);
    }
    return nullptr;
}

const BibleEntry* Film::entry(const QString& id) const {
    const auto it = m_bible.constFind(id);
    return it == m_bible.constEnd() ? nullptr : &it.value();
}

QList<const BibleEntry*> Film::entries(std::optional<BibleKind> kind) const {
    QList<const BibleEntry*> out;
    for (auto it = m_bible.constBegin(); it != m_bible.constEnd(); ++it)
        if (!kind || it->kind == *kind) out << &it.value();
    std::sort(out.begin(), out.end(), [](const BibleEntry* a, const BibleEntry* b) {
        if (a->kind != b->kind) return int(a->kind) < int(b->kind);
        const int c = a->name.compare(b->name, Qt::CaseInsensitive);
        return c != 0 ? c < 0 : a->id < b->id;
    });
    return out;
}

QString Film::entryName(const QString& id) const {
    const BibleEntry* e = entry(id);
    if (!e) return QStringLiteral("bible entry ") + id;
    return QStringLiteral("the ") + bibleKindName(e->kind) + QLatin1Char(' ') + quoted(e->name);
}

// ================================================================ Film: references

Film::Found Film::resolve(QStringView refView) const {
    Found f;
    const QString r = refView.trimmed().toString();
    if (r.isEmpty()) {
        f.error = QStringLiteral("a node is required (an id, a position like 1.2.3 or a title)");
        return f;
    }
    if ((f.node = node(r))) return f;
    const Node* ci = nullptr;
    int ciCount = 0;
    for (auto it = m_nodes.constBegin(); it != m_nodes.constEnd(); ++it)
        if (it.key().compare(r, Qt::CaseInsensitive) == 0) {
            ci = &it.value();
            ++ciCount;
        }
    if (ciCount == 1) {
        f.node = ci;
        return f;
    }
    // a position: "1.2.3", "#1.2", "shot 1.2.3"; with a kind word, missing leading levels are filled in where there
    // is only one choice ("scene 2" of a film with one sequence)
    static const QRegularExpression pos(
        QStringLiteral("^(?:(sequence|scene|shot|beat|line|cue)s?\\s*#?\\s*|#)?(\\d{1,5}(?:\\.\\d{1,5}){0,4})$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = pos.match(r);
    if (m.hasMatch()) {
        const std::optional<NodeKind> want = m.captured(1).isEmpty() ? std::nullopt : parseKind(m.captured(1));
        QStringList parts = m.captured(2).split(QLatin1Char('.'));
        auto walk = [&](const QStringList& ps) -> const Node* {
            QString parent;
            const Node* n = nullptr;
            for (const QString& p : ps) {
                const QStringList kids = m_children.value(parent);
                const int i = p.toInt() - 1;
                if (i < 0 || i >= kids.size()) return nullptr;
                n = node(kids[i]);
                if (!n) return nullptr;
                parent = n->id;
            }
            return n;
        };
        const Node* n = walk(parts);
        for (int fill = 0; want && (!n || n->kind != *want) && fill < 3; ++fill) {
            // one more leading level, only where it has a single node
            QStringList prefix;
            QString p;
            for (int lvl = 0; lvl <= fill; ++lvl) {
                const QStringList kids = m_children.value(p);
                if (kids.size() != 1) {
                    prefix.clear();
                    break;
                }
                prefix << QStringLiteral("1");
                p = kids[0];
            }
            if (prefix.isEmpty()) break;
            n = walk(prefix + parts);
        }
        if (n && (!want || n->kind == *want)) {
            f.node = n;
            return f;
        }
    }
    // a title
    QList<const Node*> titled;
    for (auto it = m_nodes.constBegin(); it != m_nodes.constEnd(); ++it)
        if (!it->title().isEmpty() && it->title().trimmed().compare(r, Qt::CaseInsensitive) == 0) titled << &it.value();
    if (titled.size() == 1) {
        f.node = titled[0];
        return f;
    }
    if (titled.size() > 1) {
        QStringList names;
        for (const Node* n : titled) names << QStringLiteral("%1 (%2)").arg(nameOf(n->id), n->id);
        names.sort();
        f.error = QStringLiteral("several nodes are called %1: %2; name one by id or position")
                      .arg(quoted(r), names.mid(0, 4).join(QStringLiteral(", ")));
        return f;
    }
    QStringList some;
    for (const QString& id : allIds()) {
        if (some.size() == 8) break;
        some << pathOf(id) + QLatin1Char('=') + id;
    }
    f.error = QStringLiteral("no node \"%1\" (%2)")
                  .arg(r.left(80),
                       some.isEmpty() ? QStringLiteral("the film is empty")
                                      : QStringLiteral("nodes: ") + some.join(QStringLiteral(", "))
                                            + (size() > some.size() ? QStringLiteral(" and %1 more").arg(size() - some.size())
                                                                    : QString()));
    return f;
}

QString Film::resolveEntry(const QString& refText, const QList<BibleKind>& kinds, QString* error) const {
    const QString r = refText.trimmed();
    auto kindsText = [&] {
        QStringList w;
        for (BibleKind k : kinds) w << bibleKindName(k);
        return w.join(QStringLiteral(" or "));
    };
    auto wrongKind = [&](const BibleEntry& e) {
        if (error)
            *error = QStringLiteral("%1 is a %2, not a %3").arg(quoted(e.name), bibleKindName(e.kind), kindsText());
        return QString();
    };
    if (r.isEmpty()) {
        if (error) *error = QStringLiteral("a %1 is required").arg(kindsText());
        return {};
    }
    if (const BibleEntry* e = entry(r)) return kinds.contains(e->kind) ? e->id : wrongKind(*e);
    QList<const BibleEntry*> byId, byName, otherKind;
    for (auto it = m_bible.constBegin(); it != m_bible.constEnd(); ++it) {
        if (it.key().compare(r, Qt::CaseInsensitive) == 0) byId << &it.value();
        if (it->name.compare(r, Qt::CaseInsensitive) == 0) (kinds.contains(it->kind) ? byName : otherKind) << &it.value();
    }
    if (byId.size() == 1) return kinds.contains(byId[0]->kind) ? byId[0]->id : wrongKind(*byId[0]);
    if (byName.size() == 1) return byName[0]->id;
    if (byName.size() > 1) {
        if (error) *error = QStringLiteral("several bible entries are called %1; name one by id").arg(quoted(r));
        return {};
    }
    if (!otherKind.isEmpty()) return wrongKind(*otherKind[0]);
    if (error) {
        QStringList names;
        for (const BibleEntry* e : entries())
            if (kinds.contains(e->kind) && names.size() < 8) names << e->name;
        *error = QStringLiteral("no %1 \"%2\" in the bible (%3)")
                     .arg(kindsText(), r.left(80),
                          names.isEmpty() ? QStringLiteral("there is none yet; add it first")
                                          : QStringLiteral("it has: ") + names.join(QStringLiteral(", ")));
    }
    return {};
}

RefResolver Film::refResolver() const {
    return [this](const QString& r, const QList<BibleKind>& kinds, QString* error) { return resolveEntry(r, kinds, error); };
}

// ================================================================ Film: mutation

void Film::placeKey(const QString& parentId, int index) {
    QStringList& sib = m_children[parentId];
    if (index < 0 || index >= sib.size()) return;
    auto val = [&](int i) { return order::value(m_nodes.value(sib[i]).orderKey); };
    const qint64 lo = index > 0 ? val(index - 1) : 0;
    const bool last = index + 1 >= sib.size();
    const qint64 hi = last ? -1 : val(index + 1);
    if (lo >= 0 && last && lo + order::kStep <= kMaxKey) {
        m_nodes[sib[index]].orderKey = order::key(lo + order::kStep);
        return;
    }
    if (lo >= 0 && !last && hi >= 0 && hi - lo >= 2) {
        m_nodes[sib[index]].orderKey = order::key(lo + (hi - lo) / 2);
        return;
    }
    // no room (or keys this module did not write): renumber the siblings, evenly spaced
    for (int i = 0; i < sib.size(); ++i) m_nodes[sib[i]].orderKey = order::key(qint64(i + 1) * order::kStep);
}

void Film::insertNode(Node n, int index) {
    n.projectId = m_project;
    const QString id = n.id, parent = n.parentId;
    m_nodes.insert(id, std::move(n));
    QStringList& sib = m_children[parent];
    if (index < 0 || index > sib.size()) index = int(sib.size());
    sib.insert(index, id);
    placeKey(parent, index);
}

void Film::removeSubtree(const QString& id) {
    const Node* n = node(id);
    if (!n) return;
    const QStringList ids = subtree(id);
    m_children[n->parentId].removeAll(id);
    for (const QString& x : ids) {
        m_nodes.remove(x);
        m_children.remove(x);
    }
}

void Film::moveNode(const QString& id, const QString& parentId, int index) {
    Node* n = nodeMut(id);
    if (!n) return;
    m_children[n->parentId].removeAll(id);
    n->parentId = parentId;
    QStringList& sib = m_children[parentId];
    if (index < 0 || index > sib.size()) index = int(sib.size());
    sib.insert(index, id);
    placeKey(parentId, index);
}

void Film::putEntry(BibleEntry e) {
    e.projectId = m_project;
    const QString id = e.id;
    m_bible.insert(id, std::move(e));
}

void Film::removeEntry(const QString& id) { m_bible.remove(id); }

bool Film::setOutputs(const QString& id, const QJsonObject& outputs, QString* error) {
    auto fail = [&](const QString& e) {
        if (error) *error = e;
        return false;
    };
    const Node* n = node(id);
    if (!n) return fail(QStringLiteral("no node \"%1\"").arg(id));
    QJsonObject data = n->data;
    for (auto it = outputs.constBegin(); it != outputs.constEnd(); ++it) {
        const FieldSpec* f = nullptr;
        for (const FieldSpec& s : nodeFields(n->kind))
            if (s.name == it.key()) f = &s;
        if (!f || !f->output)
            return fail(QStringLiteral("\"%1\" is not an output of a %2").arg(it.key(), kindName(n->kind)));
        const QJsonValue v = it.value();
        if (v.isNull() || v.isUndefined() || (v.isString() && v.toString().isEmpty())) {
            data.remove(f->name);
            continue;
        }
        if (f->type == FieldType::Asset) {
            const QString s = v.toString().trimmed().toLower();
            if (!isSha256(s)) return fail(QStringLiteral("%1 must be an artifact sha256").arg(f->name));
            data.insert(f->name, s);
        } else {
            const double d = v.toDouble(-1);
            if (!v.isDouble() || !std::isfinite(d) || d < 0 || d > f->max)
                return fail(QStringLiteral("%1 must be a number from 0 to %2").arg(f->name, json::jsNumber(f->max)));
            data.insert(f->name, std::round(d * 1000) / 1000);
        }
    }
    nodeMut(id)->data = data;
    return true;
}

bool Film::setChosenTake(const QString& id, const QString& takeId) {
    Node* n = nodeMut(id);
    if (!n || (!takeId.isEmpty() && !ids::isSafe(takeId))) return false;
    n->chosenTakeId = takeId;
    return true;
}

QStringList Film::problems() const {
    QStringList out;
    QHash<QString, int> listed;
    for (auto it = m_children.constBegin(); it != m_children.constEnd(); ++it)
        for (const QString& c : it.value()) {
            listed[c] += 1;
            const Node* n = node(c);
            if (n && n->parentId != it.key()) out << QStringLiteral("%1 is listed under the wrong parent").arg(c);
        }
    for (auto it = m_nodes.constBegin(); it != m_nodes.constEnd(); ++it) {
        const Node& n = it.value();
        if (n.projectId != m_project) out << QStringLiteral("node %1 belongs to project %2").arg(n.id, n.projectId);
        const Node* p = n.parentId.isEmpty() ? nullptr : node(n.parentId);
        if (!n.parentId.isEmpty() && !p) {
            out << QStringLiteral("%1 %2 has no parent (%3 is missing)").arg(kindName(n.kind), n.id, n.parentId);
            continue;
        }
        if (!canContain(p ? std::optional<NodeKind>(p->kind) : std::nullopt, n.kind))
            out << QStringLiteral("%1 %2 is %3: %4")
                       .arg(kindName(n.kind), n.id,
                            p ? QStringLiteral("in %1 %2").arg(kindName(p->kind), p->id) : QStringLiteral("at the top level"),
                            containmentRule(n.kind));
        int depth = 0;
        for (const Node* a = p; a && depth < 16; a = parentOf(*a)) ++depth;
        if (depth >= 16) out << QStringLiteral("node %1 is in a cycle").arg(n.id);
        if (listed.value(n.id) != 1) out << QStringLiteral("node %1 is listed %2 times").arg(n.id).arg(listed.value(n.id));
        for (const FieldSpec& f : nodeFields(n.kind)) {
            if (f.type != FieldType::Ref && f.type != FieldType::Refs) continue;
            QStringList named;
            const QJsonValue v = n.data.value(f.name);
            if (v.isString()) named << v.toString();
            for (const QJsonValue& e : v.toArray()) named << e.toString();
            for (const QString& id : named) {
                const BibleEntry* e = entry(id);
                if (!e || !f.refKinds.contains(e->kind))
                    out << QStringLiteral("%1 %2 names a missing %3 \"%4\"")
                               .arg(kindName(n.kind), n.id, bibleKindName(f.refKinds.value(0)), id);
            }
        }
    }
    int styles = 0;
    for (auto it = m_bible.constBegin(); it != m_bible.constEnd(); ++it) {
        const BibleEntry& e = it.value();
        if (e.projectId != m_project) out << QStringLiteral("bible entry %1 belongs to project %2").arg(e.id, e.projectId);
        if (e.kind == BibleKind::Style) ++styles;
        const QString v = e.data.value(QStringLiteral("voice")).toString();
        if (e.kind == BibleKind::Character && !v.isEmpty() && (!entry(v) || entry(v)->kind != BibleKind::Voice))
            out << QStringLiteral("character %1 names a missing voice \"%2\"").arg(e.id, v);
    }
    if (styles > 1) out << QStringLiteral("the film has %1 style entries (one is allowed)").arg(styles);
    return out;
}

// ================================================================ JSON

QJsonObject Film::nodeJson(const QString& id, int depth) const {
    const Node* n = node(id);
    if (!n) return {};
    QJsonObject o{
        {QStringLiteral("id"), n->id},
        {QStringLiteral("kind"), kindName(n->kind)},
        {QStringLiteral("parent"), n->parentId},
        {QStringLiteral("path"), pathOf(id)},
        {QStringLiteral("data"), n->data},
        {QStringLiteral("locked"), n->locked},
        {QStringLiteral("stale"), n->stale},
        {QStringLiteral("inputHash"), n->inputHash},
        {QStringLiteral("chosenTake"), n->chosenTakeId},
    };
    const QStringList kids = children(id);
    if (depth > 0 && !kids.isEmpty()) {
        QJsonArray a;
        for (const QString& c : kids) a.append(nodeJson(c, depth - 1));
        o.insert(QStringLiteral("children"), a);
    } else {
        o.insert(QStringLiteral("childCount"), int(kids.size()));
    }
    return o;
}

QJsonObject Film::toJson(int depth) const {
    QJsonArray bible, top;
    for (const BibleEntry* e : entries()) bible.append(entryJson(*e));
    for (const QString& id : children()) top.append(nodeJson(id, depth - 1));
    return QJsonObject{
        {QStringLiteral("project"), m_project},
        {QStringLiteral("settings"), m_settings.toJson()},
        {QStringLiteral("bible"), bible},
        {QStringLiteral("sequences"), top},
        {QStringLiteral("nodes"), size()},
    };
}

// ================================================================ load / save

std::optional<Film> Film::load(const QSqlDatabase& db, const QString& projectId, QString* error) {
    auto fail = [&](const QString& e) {
        if (error) *error = e;
        return std::optional<Film>();
    };
    auto run = [&](QSqlQuery& q, const QString& sql, const QVariantList& binds) {
        q.setForwardOnly(true);
        if (!q.prepare(sql)) return false;
        for (int i = 0; i < binds.size(); ++i) q.bindValue(i, binds[i]);
        return q.exec();
    };
    Film f(projectId);
    {
        QSqlQuery q(db);
        if (!run(q, QStringLiteral("SELECT id, parent_id, kind, order_key, data_json, input_hash, locked, stale, "
                                   "chosen_take_id, updated_ms FROM film_nodes WHERE project_id = ? "
                                   "ORDER BY parent_id, order_key, id"),
                 {projectId}))
            return fail(QStringLiteral("reading film_nodes: %1").arg(q.lastError().text()));
        while (q.next()) {
            Node n;
            n.id = q.value(0).toString();
            n.projectId = projectId;
            n.parentId = q.value(1).toString();
            const std::optional<NodeKind> k = parseKind(q.value(2).toString());
            if (!k || q.value(2).toString() != kindName(*k))
                return fail(QStringLiteral("film node %1 has an unknown kind \"%2\"").arg(n.id, q.value(2).toString()));
            n.kind = *k;
            n.orderKey = q.value(3).toString();
            QString perr;
            const QJsonValue d = json::parse(q.value(4).toString().toUtf8(), &perr);
            if (!d.isObject()) return fail(QStringLiteral("film node %1: data_json is not an object").arg(n.id));
            n.data = d.toObject();
            n.inputHash = q.value(5).toString();
            n.locked = q.value(6).toLongLong() != 0;
            n.stale = q.value(7).toLongLong() != 0;
            n.chosenTakeId = q.value(8).toString();
            n.updatedMs = q.value(9).toLongLong();
            f.m_children[n.parentId] << n.id;   // the query orders siblings by order_key, then id
            f.m_nodes.insert(n.id, n);
        }
        if (q.lastError().isValid()) return fail(QStringLiteral("reading film_nodes: %1").arg(q.lastError().text()));
    }
    {
        QSqlQuery q(db);
        if (!run(q, QStringLiteral("SELECT id, kind, name, data_json, ref_artifacts_json, updated_ms FROM bible "
                                   "WHERE project_id = ? ORDER BY id"),
                 {projectId}))
            return fail(QStringLiteral("reading bible: %1").arg(q.lastError().text()));
        while (q.next()) {
            BibleEntry e;
            e.id = q.value(0).toString();
            e.projectId = projectId;
            const std::optional<BibleKind> k = parseBibleKind(q.value(1).toString());
            if (!k || q.value(1).toString() != bibleKindName(*k))
                return fail(QStringLiteral("bible entry %1 has an unknown kind \"%2\"").arg(e.id, q.value(1).toString()));
            e.kind = *k;
            e.name = q.value(2).toString();
            const QJsonValue d = json::parse(q.value(3).toString().toUtf8());
            if (!d.isObject()) return fail(QStringLiteral("bible entry %1: data_json is not an object").arg(e.id));
            e.data = d.toObject();
            QString rerr;
            if (!checkRefs(json::parse(q.value(4).toString().toUtf8()), &e.refs, &rerr))
                return fail(QStringLiteral("bible entry %1: %2").arg(e.id, rerr));
            e.updatedMs = q.value(5).toLongLong();
            f.m_bible.insert(e.id, e);
        }
        if (q.lastError().isValid()) return fail(QStringLiteral("reading bible: %1").arg(q.lastError().text()));
    }
    {
        QSqlQuery q(db);
        if (!run(q, QStringLiteral("SELECT brief_json FROM projects WHERE id = ?"), {projectId}))
            return fail(QStringLiteral("reading projects: %1").arg(q.lastError().text()));
        if (q.next()) {
            const QJsonObject b = json::parseObject(q.value(0).toString().toUtf8());
            // values another writer put there are taken only when valid; the defaults stay otherwise
            const QString a = b.value(QStringLiteral("aspect")).toString();
            if (aspects().contains(a)) f.m_settings.aspect = a;
            const QString l = b.value(QStringLiteral("language")).toString();
            static const QRegularExpression lang(QStringLiteral("^[A-Za-z]{2,3}(-[A-Za-z0-9]{2,8}){0,3}$"));
            if (lang.match(l).hasMatch()) f.m_settings.language = l;
            const double t = b.value(QStringLiteral("target_seconds")).toDouble(0);
            if (std::isfinite(t) && t > 0) f.m_settings.targetSeconds = t;
        }
    }
    const QStringList bad = f.problems();
    if (!bad.isEmpty())
        return fail(QStringLiteral("the film of project %1 is damaged: %2%3")
                        .arg(projectId, bad.mid(0, 3).join(QStringLiteral("; ")),
                             bad.size() > 3 ? QStringLiteral(" (and %1 more)").arg(bad.size() - 3) : QString()));
    return f;
}

store::Write saveWrite(const Film& prev, const Film& next) {
    struct Rows {
        QString project;
        QList<Node> nodes;
        QStringList removedNodes;
        QList<BibleEntry> entries;
        QStringList removedEntries;
        bool settings = false;
        Settings s;
    };
    Rows c;
    c.project = next.projectId();
    for (const QString& id : next.allIds()) {
        const Node* n = next.node(id);
        const Node* p = prev.node(id);
        if (!p || !sameRow(*p, *n)) c.nodes << *n;
    }
    for (const QString& id : prev.allIds())
        if (!next.node(id)) c.removedNodes << id;
    for (const BibleEntry* e : next.entries()) {
        const BibleEntry* p = prev.entry(e->id);
        if (!p || !sameEntry(*p, *e)) c.entries << *e;
    }
    for (const BibleEntry* e : prev.entries())
        if (!next.entry(e->id)) c.removedEntries << e->id;
    const Settings& a = prev.settings();
    c.s = next.settings();
    c.settings = a.aspect != c.s.aspect || a.language != c.s.language || a.targetSeconds != c.s.targetSeconds
              || (prev.isEmpty() && !next.isEmpty());

    store::Write w;
    w.tag = QStringLiteral("film.save");
    w.durability = store::Durability::Batched;
    w.fn = [c](store::Tx& tx) {
        const qint64 now = tx.nowMs();
        for (const QString& id : c.removedNodes) {
            if (!tx.exec(QStringLiteral("DELETE FROM film_nodes WHERE id = ? AND project_id = ?"), {id, c.project}))
                return;
            if (tx.rowsAffected() == 1 && !tx.exec(QStringLiteral("DELETE FROM takes WHERE film_node_id = ?"), {id}))
                return;
        }
        for (const QString& id : c.removedEntries)
            if (!tx.exec(QStringLiteral("DELETE FROM bible WHERE id = ? AND project_id = ?"), {id, c.project})) return;
        for (const Node& n : c.nodes) {
            // an id that belongs to another project is never overwritten: the WHERE makes the upsert a no-op, which
            // fails the whole write
            if (!tx.exec(QStringLiteral(
                             "INSERT INTO film_nodes (id, project_id, parent_id, kind, order_key, data_json, input_hash, "
                             "locked, stale, chosen_take_id, updated_ms) VALUES (?,?,?,?,?,?,?,?,?,?,?) "
                             "ON CONFLICT(id) DO UPDATE SET parent_id = excluded.parent_id, kind = excluded.kind, "
                             "order_key = excluded.order_key, data_json = excluded.data_json, "
                             "input_hash = excluded.input_hash, locked = excluded.locked, stale = excluded.stale, "
                             "chosen_take_id = excluded.chosen_take_id, updated_ms = excluded.updated_ms "
                             "WHERE film_nodes.project_id = excluded.project_id"),
                         {n.id, c.project, txt(n.parentId), kindName(n.kind), txt(n.orderKey), jsonText(n.data),
                          txt(n.inputHash), n.locked ? 1 : 0, n.stale ? 1 : 0, txt(n.chosenTakeId), now}))
                return;
            if (tx.rowsAffected() != 1) {
                tx.fail(QStringLiteral("film node id \"%1\" belongs to another project").arg(n.id));
                return;
            }
        }
        for (const BibleEntry& e : c.entries) {
            if (!tx.exec(QStringLiteral(
                             "INSERT INTO bible (id, project_id, kind, name, data_json, ref_artifacts_json, updated_ms) "
                             "VALUES (?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET kind = excluded.kind, "
                             "name = excluded.name, data_json = excluded.data_json, "
                             "ref_artifacts_json = excluded.ref_artifacts_json, updated_ms = excluded.updated_ms "
                             "WHERE bible.project_id = excluded.project_id"),
                         {e.id, c.project, bibleKindName(e.kind), txt(e.name), jsonText(e.data), jsonText(e.refs), now}))
                return;
            if (tx.rowsAffected() != 1) {
                tx.fail(QStringLiteral("bible entry id \"%1\" belongs to another project").arg(e.id));
                return;
            }
        }
        if (c.settings) {
            // json_set keeps every other key of the brief; malformed brief JSON fails the write (nothing is lost)
            const QVariant target = c.s.targetSeconds > 0 ? QVariant(c.s.targetSeconds) : QVariant();
            QJsonObject brief = c.s.toJson();
            tx.exec(QStringLiteral(
                        "INSERT INTO projects (id, kind, brief_json, created_ms, updated_ms) VALUES (?, 'film', ?, ?, ?) "
                        "ON CONFLICT(id) DO UPDATE SET brief_json = json_set(projects.brief_json, '$.aspect', ?, "
                        "'$.language', ?, '$.target_seconds', ?), updated_ms = excluded.updated_ms"),
                    {c.project, jsonText(brief), now, now, c.s.aspect, c.s.language, target});
        }
    };
    return w;
}

// ================================================================ operations: normalisation

const QStringList& opNames() {
    static const QStringList ops{
        QStringLiteral("update_node"), QStringLiteral("insert_node"), QStringLiteral("delete_node"),
        QStringLiteral("move_node"),   QStringLiteral("lock"),        QStringLiteral("unlock"),
        QStringLiteral("regenerate"),  QStringLiteral("add_bible"),   QStringLiteral("update_bible"),
        QStringLiteral("remove_bible"), QStringLiteral("set_style"),  QStringLiteral("set_aspect"),
        QStringLiteral("set_language"), QStringLiteral("set_length"),
    };
    return ops;
}

namespace {

struct Canon {
    QString op;
    std::optional<NodeKind> kind;
    std::optional<BibleKind> bibleKind;
};

// Models phrase operations in many ways ("edit_scene", "add_shot", "update_character"): anything that maps
// unambiguously is accepted (ops.ts OP_ALIASES, generalised to verb_<kind>).
Canon canonicalOp(const QString& raw) {
    const QString s = normaliseKey(raw);
    Canon c;
    if (opNames().contains(s)) {
        c.op = s;
        if (s == QLatin1String("set_style")) c.bibleKind = BibleKind::Style;
        return c;
    }
    static const QHash<QString, QString> exact{
        {QStringLiteral("update"), QStringLiteral("update_node")},   {QStringLiteral("edit"), QStringLiteral("update_node")},
        {QStringLiteral("modify"), QStringLiteral("update_node")},   {QStringLiteral("change"), QStringLiteral("update_node")},
        {QStringLiteral("rewrite"), QStringLiteral("update_node")},  {QStringLiteral("patch"), QStringLiteral("update_node")},
        {QStringLiteral("insert"), QStringLiteral("insert_node")},   {QStringLiteral("add"), QStringLiteral("insert_node")},
        {QStringLiteral("new"), QStringLiteral("insert_node")},      {QStringLiteral("create"), QStringLiteral("insert_node")},
        {QStringLiteral("append"), QStringLiteral("insert_node")},   {QStringLiteral("delete"), QStringLiteral("delete_node")},
        {QStringLiteral("remove"), QStringLiteral("delete_node")},   {QStringLiteral("cut"), QStringLiteral("delete_node")},
        {QStringLiteral("drop"), QStringLiteral("delete_node")},     {QStringLiteral("move"), QStringLiteral("move_node")},
        {QStringLiteral("reorder"), QStringLiteral("move_node")},    {QStringLiteral("regen"), QStringLiteral("regenerate")},
        {QStringLiteral("redo"), QStringLiteral("regenerate")},      {QStringLiteral("style"), QStringLiteral("set_style")},
        {QStringLiteral("change_style"), QStringLiteral("set_style")}, {QStringLiteral("set_preset"), QStringLiteral("set_style")},
        {QStringLiteral("language"), QStringLiteral("set_language")}, {QStringLiteral("aspect"), QStringLiteral("set_aspect")},
        {QStringLiteral("aspect_ratio"), QStringLiteral("set_aspect")}, {QStringLiteral("set_aspect_ratio"), QStringLiteral("set_aspect")},
        {QStringLiteral("length"), QStringLiteral("set_length")},    {QStringLiteral("set_target_length"), QStringLiteral("set_length")},
        {QStringLiteral("set_target_seconds"), QStringLiteral("set_length")},
        {QStringLiteral("add_entry"), QStringLiteral("add_bible")},  {QStringLiteral("add_bible_entry"), QStringLiteral("add_bible")},
        {QStringLiteral("new_entry"), QStringLiteral("add_bible")},  {QStringLiteral("update_entry"), QStringLiteral("update_bible")},
        {QStringLiteral("edit_entry"), QStringLiteral("update_bible")}, {QStringLiteral("update_bible_entry"), QStringLiteral("update_bible")},
        {QStringLiteral("remove_entry"), QStringLiteral("remove_bible")}, {QStringLiteral("delete_entry"), QStringLiteral("remove_bible")},
        {QStringLiteral("remove_bible_entry"), QStringLiteral("remove_bible")},
    };
    if (exact.contains(s)) return canonicalOp(exact.value(s));
    const int us = int(s.indexOf(QLatin1Char('_')));
    if (us <= 0) return c;
    const QString verb = s.left(us), noun = s.mid(us + 1);
    static const QHash<QString, char> verbs{
        {QStringLiteral("update"), 'U'}, {QStringLiteral("edit"), 'U'},    {QStringLiteral("modify"), 'U'},
        {QStringLiteral("change"), 'U'}, {QStringLiteral("rewrite"), 'U'}, {QStringLiteral("set"), 'U'},
        {QStringLiteral("patch"), 'U'},  {QStringLiteral("insert"), 'I'},  {QStringLiteral("add"), 'I'},
        {QStringLiteral("new"), 'I'},    {QStringLiteral("create"), 'I'},  {QStringLiteral("append"), 'I'},
        {QStringLiteral("delete"), 'D'}, {QStringLiteral("remove"), 'D'},  {QStringLiteral("cut"), 'D'},
        {QStringLiteral("drop"), 'D'},   {QStringLiteral("move"), 'M'},    {QStringLiteral("reorder"), 'M'},
        {QStringLiteral("lock"), 'L'},   {QStringLiteral("unlock"), 'X'},  {QStringLiteral("regenerate"), 'R'},
        {QStringLiteral("regen"), 'R'},  {QStringLiteral("redo"), 'R'},
    };
    if (!verbs.contains(verb)) return c;
    const char v = verbs.value(verb);
    const QString n = noun == QLatin1String("node") || noun == QLatin1String("nodes") ? QString() : noun;
    if (n.isEmpty() || parseKind(n)) {
        static const QHash<char, QString> nodeOps{{'U', QStringLiteral("update_node")}, {'I', QStringLiteral("insert_node")},
                                                  {'D', QStringLiteral("delete_node")}, {'M', QStringLiteral("move_node")},
                                                  {'L', QStringLiteral("lock")},        {'X', QStringLiteral("unlock")},
                                                  {'R', QStringLiteral("regenerate")}};
        c.op = nodeOps.value(v);
        if (!n.isEmpty()) c.kind = parseKind(n);
        return c;
    }
    if (const std::optional<BibleKind> bk = parseBibleKind(n)) {
        c.bibleKind = bk;
        if (*bk == BibleKind::Style) c.op = (v == 'U' || v == 'I') ? QStringLiteral("set_style") : QString();
        else if (v == 'U') c.op = QStringLiteral("update_bible");
        else if (v == 'I') c.op = QStringLiteral("add_bible");
        else if (v == 'D') c.op = QStringLiteral("remove_bible");
        return c;
    }
    return c;
}

} // namespace

Op normalizeOp(const QJsonValue& raw) {
    Op o;
    if (!raw.isObject()) {
        o.raw = raw.isString() ? raw.toString() : QString();
        return o;
    }
    const QJsonObject src = raw.toObject();
    static const QStringList nested{QStringLiteral("changes"), QStringLiteral("fields"), QStringLiteral("patch"),
                                    QStringLiteral("set"),     QStringLiteral("data"),   QStringLiteral("update"),
                                    QStringLiteral("values")};
    QJsonObject flat;
    for (auto it = src.constBegin(); it != src.constEnd(); ++it) {
        const QString k = normaliseKey(it.key());
        if (!(nested.contains(k) && it.value().isObject())) flat.insert(k, it.value());
    }
    // nested patches: {op, node, changes:{...}} (they win over flat keys, like Object.assign in ops.ts)
    for (auto it = src.constBegin(); it != src.constEnd(); ++it) {
        const QString k = normaliseKey(it.key());
        if (!nested.contains(k) || !it.value().isObject()) continue;
        const QJsonObject inner = it.value().toObject();
        for (auto j = inner.constBegin(); j != inner.constEnd(); ++j) flat.insert(normaliseKey(j.key()), j.value());
    }
    QString opRaw;
    for (const char* k : {"op", "action", "operation"})
        if (flat.contains(QLatin1String(k))) {
            if (opRaw.isEmpty()) opRaw = flat.value(QLatin1String(k)).toString();
            flat.remove(QLatin1String(k));
        }
    // "kind" / "type" name what to insert; "type" is the op only when no op was given and it names one
    for (const char* k : {"kind", "type", "node_kind", "entry_kind", "bible_kind"}) {
        const QLatin1String key(k);
        if (!flat.value(key).isString()) continue;
        const QString v = flat.value(key).toString();
        if (const std::optional<NodeKind> nk = parseKind(v)) {
            if (!o.kind) o.kind = nk;
            flat.remove(key);
        } else if (const std::optional<BibleKind> bk = parseBibleKind(v)) {
            if (!o.bibleKind) o.bibleKind = bk;
            flat.remove(key);
        } else if (opRaw.isEmpty() && !canonicalOp(v).op.isEmpty()) {
            opRaw = v;
            flat.remove(key);
        }
    }
    if (opRaw.isEmpty()) {
        // ops.ts: a style alone means set_style, a node alone means update
        const bool hasNode = flat.contains(QStringLiteral("node")) || flat.contains(QStringLiteral("node_id"))
                          || flat.contains(QStringLiteral("scene_id")) || flat.contains(QStringLiteral("shot_id"))
                          || flat.contains(QStringLiteral("sequence_id")) || flat.contains(QStringLiteral("id"));
        if (!hasNode && (flat.contains(QStringLiteral("style_id")) || flat.contains(QStringLiteral("style"))))
            opRaw = QStringLiteral("set_style");
        else if (hasNode)
            opRaw = QStringLiteral("update_node");
    }
    o.raw = opRaw;
    const Canon c = canonicalOp(opRaw);
    o.name = c.op;
    if (c.kind) o.kind = c.kind;
    if (c.bibleKind) o.bibleKind = c.bibleKind;
    o.args = flat;
    return o;
}

// ================================================================ operations: applying

namespace {

struct Effects {
    QStringList applied, errors, unchanged, touched, bible, staled;
};

// Keys that only label an op (ignored everywhere).
bool isRemark(const QString& k) {
    return k == QLatin1String("note") || k == QLatin1String("reason") || k == QLatin1String("why")
        || k == QLatin1String("comment") || k == QLatin1String("label");
}

class Applier {
public:
    Applier(Film& f, const Limits& l, Effects& e) : film(f), limits(l), fx(e) {}

    void run(const Op& o, int index) {
        if (o.name.isEmpty()) {
            if (!o.raw.isNull() || !o.args.isEmpty())
                fx.errors << QStringLiteral("unknown op \"%1\" (use one of: %2)")
                                 .arg(o.raw.left(60), opNames().join(QStringLiteral(", ")));
            else
                fx.errors << QStringLiteral("operation %1 is not an object with an \"op\"").arg(index + 1);
            return;
        }
        a = o.args;
        note = takeString({"note", "label"});
        const QString& n = o.name;
        if (n == QLatin1String("update_node")) updateNode(o);
        else if (n == QLatin1String("insert_node")) insertNode(o);
        else if (n == QLatin1String("delete_node")) deleteNode(o);
        else if (n == QLatin1String("move_node")) moveNode(o);
        else if (n == QLatin1String("lock") || n == QLatin1String("unlock")) lock(o, n == QLatin1String("lock"));
        else if (n == QLatin1String("regenerate")) regenerate(o);
        else if (n == QLatin1String("add_bible")) addBible(o);
        else if (n == QLatin1String("update_bible")) updateBible(o);
        else if (n == QLatin1String("remove_bible")) removeBible(o);
        else if (n == QLatin1String("set_style")) setStyle();
        else if (n == QLatin1String("set_aspect")) setAspect();
        else if (n == QLatin1String("set_language")) setLanguage();
        else if (n == QLatin1String("set_length")) setLength();
    }

private:
    Film& film;
    const Limits& limits;
    Effects& fx;
    QJsonObject a;                         // the op's remaining keys
    QString note;

    // ---- argument helpers
    static QStringList keyList(std::initializer_list<const char*> keys) {
        QStringList out;
        for (const char* k : keys) out << QString::fromLatin1(k);
        return out;
    }
    bool has(const QStringList& keys) const {
        for (const QString& k : keys)
            if (a.contains(k)) return true;
        return false;
    }
    bool has(std::initializer_list<const char*> keys) const { return has(keyList(keys)); }
    /// Removes every key of `keys` and returns the first present value (as text; numbers are written as JS does).
    QString takeString(const QStringList& keys) {
        QString out;
        bool got = false;
        for (const QString& key : keys) {
            if (!a.contains(key)) continue;
            const QJsonValue v = a.take(key);
            if (got) continue;
            if (v.isString()) out = v.toString();
            else if (v.isDouble()) out = json::jsNumber(v.toDouble());
            else continue;
            got = true;
        }
        return out;
    }
    QString takeString(std::initializer_list<const char*> keys) { return takeString(keyList(keys)); }
    QJsonValue takeValue(std::initializer_list<const char*> keys) {
        QJsonValue out;
        for (const char* k : keys) {
            const QLatin1String key(k);
            if (!a.contains(key)) continue;
            const QJsonValue v = a.take(key);
            if (out.isUndefined() || out.isNull()) out = v;
        }
        return out;
    }
    QString takeTarget() {
        return takeString({"node", "node_id", "target", "id", "sequence_id", "scene_id", "shot_id", "beat_id",
                           "line_id", "cue_id", "sequence", "scene", "shot", "beat", "line", "cue"});
    }
    struct After {
        bool given = false;
        QString ref;
    };
    After takeAfter() {
        After r;
        r.given = has({"after", "after_id", "after_node", "after_node_id", "after_sequence_id", "after_scene_id",
                       "after_shot_id", "after_beat_id", "after_line_id", "after_cue_id", "after_scene", "after_shot"});
        r.ref = takeString({"after", "after_id", "after_node", "after_node_id", "after_sequence_id", "after_scene_id",
                            "after_shot_id", "after_beat_id", "after_line_id", "after_cue_id", "after_scene",
                            "after_shot"});
        return r;
    }
    QString takeParent() { return takeString({"parent", "parent_id", "in", "into", "under", "to"}); }
    /// Keys left that nobody asked for (remarks ignored).
    QStringList leftovers() const {
        QStringList out;
        for (auto it = a.constBegin(); it != a.constEnd(); ++it)
            if (!isRemark(it.key())) out << it.key();
        return out;
    }

    // ---- fields
    struct Patch {
        QList<QPair<const FieldSpec*, FieldCheck>> items;
        bool ok = true;
    };
    Patch parseFields(const QList<FieldSpec>& specs, const QString& what) {
        Patch p;
        QStringList unknown, problems;
        const RefResolver refs = film.refResolver();
        for (auto it = a.constBegin(); it != a.constEnd(); ++it) {
            if (isRemark(it.key())) continue;
            const FieldSpec* f = findField(specs, it.key());
            if (!f) {
                unknown << it.key();
                continue;
            }
            const FieldCheck c = checkField(*f, it.value(), refs, what);
            if (!c.ok) {
                problems << c.error;
                continue;
            }
            bool dup = false;
            for (auto& x : p.items)
                if (x.first == f) dup = true;
            if (!dup) p.items << qMakePair(f, c);
        }
        if (!unknown.isEmpty())
            problems.prepend(QStringLiteral("unknown field%1 %2 for a %3 (fields: %4)")
                                 .arg(unknown.size() > 1 ? QStringLiteral("s") : QString(),
                                      QStringLiteral("\"") + unknown.join(QStringLiteral("\", \"")) + QStringLiteral("\""),
                                      what, fieldList(specs)));
        if (!problems.isEmpty()) {
            fx.errors << problems.join(QStringLiteral("; "));
            p.ok = false;
        }
        return p;
    }
    /// Applies a patch to `data`; returns the names of the fields that changed.
    static QStringList applyPatch(QJsonObject& data, const Patch& p) {
        QStringList changed;
        for (const auto& [f, c] : p.items) {
            if (!c.given) continue;
            if (c.clear) {
                if (data.contains(f->name)) {
                    data.remove(f->name);
                    changed << f->name;
                }
            } else if (data.value(f->name) != c.value) {
                data.insert(f->name, c.value);
                changed << f->name;
            }
        }
        return changed;
    }
    static bool anyGiven(const Patch& p) {
        for (const auto& x : p.items)
            if (x.second.given) return true;
        return false;
    }

    // ---- nodes
    const Node* findNode(const QString& ref, const char* opName, std::optional<NodeKind> want) {
        if (ref.trimmed().isEmpty()) {
            fx.errors << QStringLiteral("%1 needs the node to change (node: an id, a position like 1.2.3 or a title)")
                             .arg(QLatin1String(opName));
            return nullptr;
        }
        const Film::Found f = film.resolve(ref);
        if (!f.node) {
            fx.errors << f.error;
            return nullptr;
        }
        if (want && f.node->kind != *want) {
            fx.errors << QStringLiteral("%1 is a %2, not a %3").arg(capitalised(film.nameOf(f.node->id)),
                                                                   kindName(f.node->kind), kindName(*want));
            return nullptr;
        }
        return f.node;
    }
    bool refuseLocked(const QString& id, const QString& what = {}) {
        const Node* l = film.lockedAt(id);
        if (!l) return false;
        const QString name = capitalised(what.isEmpty() ? film.nameOf(id) : what);
        if (l->id == id) fx.errors << QStringLiteral("%1 is locked; unlock it first").arg(name);
        else fx.errors << QStringLiteral("%1 is inside locked %2; unlock that first").arg(name, film.nameOf(l->id));
        return true;
    }
    QString newId(const char* prefix) {
        const QString p = QString::fromLatin1(prefix);
        for (;;) {
            const QString id = ids::random(p, 6);
            if (!film.node(id) && !film.entry(id)) return id;
        }
    }
    bool checkGivenId(const QString& id) {
        if (!ids::isSafe(id)) {
            fx.errors << QStringLiteral("\"%1\" is not a usable id (1-128 letters, digits, _ . -)").arg(id.left(80));
            return false;
        }
        if (film.node(id) || film.entry(id)) {
            fx.errors << QStringLiteral("there is already a %1 with the id \"%2\"")
                             .arg(film.node(id) ? QStringLiteral("node") : QStringLiteral("bible entry"), id);
            return false;
        }
        return true;
    }
    /// Index among `siblings` for "after": "" / start -> 0, end -> size, else after that sibling.
    std::optional<int> position(const After& after, const QStringList& siblings, const QString& parentName,
                                bool absentIsEnd, const QString& self = {}) {
        if (!after.given) return absentIsEnd ? int(siblings.size()) : 0;
        const QString r = after.ref.trimmed();
        static const QRegularExpression start(QStringLiteral("^(start|beginning|first|begin|top|0)$"),
                                              QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression end(QStringLiteral("^(end|last|bottom)$"), QRegularExpression::CaseInsensitiveOption);
        if (r.isEmpty() || start.match(r).hasMatch()) return 0;
        if (end.match(r).hasMatch()) return int(siblings.size());
        const Film::Found f = film.resolve(r);
        if (!f.node) {
            fx.errors << f.error;
            return std::nullopt;
        }
        if (!self.isEmpty() && f.node->id == self) {
            fx.errors << QStringLiteral("%1 cannot go after itself").arg(capitalised(film.nameOf(self)));
            return std::nullopt;
        }
        const int i = int(siblings.indexOf(f.node->id));
        if (i < 0) {
            fx.errors << QStringLiteral("%1 is not in %2: after must name one of its children")
                             .arg(capitalised(film.nameOf(f.node->id)), parentName);
            return std::nullopt;
        }
        return i + 1;
    }
    QString label(const QString& fallback) const { return note.trimmed().isEmpty() ? fallback : note.trimmed(); }

    void updateNode(const Op& o) {
        const QString ref = takeTarget();
        takeAfter();
        takeParent();
        const Node* n = findNode(ref, "update_node", o.kind);
        if (!n) return;
        const QString id = n->id;
        const QString name = film.nameOf(id);
        const Patch p = parseFields(nodeFields(n->kind), kindName(n->kind));
        if (!p.ok) return;
        if (!anyGiven(p)) {
            fx.errors << QStringLiteral("nothing to change in %1 (%2): give at least one of %3")
                             .arg(name, id, fieldList(nodeFields(n->kind)));
            return;
        }
        QJsonObject data = n->data;
        const QStringList changed = applyPatch(data, p);
        if (changed.isEmpty()) {
            fx.unchanged << QStringLiteral("%1 (%2) already has that").arg(capitalised(name), id);
            return;
        }
        if (refuseLocked(id)) return;
        film.nodeMut(id)->data = data;
        fx.touched << id;
        fx.applied << QStringLiteral("%1: %2").arg(name, label(changed.join(QStringLiteral(", ")) + QStringLiteral(" changed")));
    }

    void insertNode(const Op& o) {
        if (!o.kind) {
            fx.errors << QStringLiteral("insert_node needs a kind (sequence, scene, shot, beat, line or cue)");
            return;
        }
        const NodeKind kind = *o.kind;
        const QString givenId = takeString({"id", "new_id"});
        QString parentRef = takeParent();
        // "scene_id" when adding a shot names its scene (only kinds that may hold it count)
        for (NodeKind k : allKinds()) {
            if (!canContain(k, kind)) continue;
            const QString v = kindName(k);
            const QString got = takeString(QStringList{v + QStringLiteral("_id"), v});
            if (parentRef.isEmpty()) parentRef = got;
        }
        const After after = takeAfter();
        const Patch p = parseFields(nodeFields(kind), kindName(kind));
        if (!p.ok) return;
        const Node* parent = nullptr;
        if (!parentRef.trimmed().isEmpty()) {
            const Film::Found f = film.resolve(parentRef);
            if (!f.node) {
                fx.errors << f.error;
                return;
            }
            parent = f.node;
        } else if (after.given && !after.ref.trimmed().isEmpty()) {
            const Film::Found f = film.resolve(after.ref);
            if (f.node && !f.node->parentId.isEmpty()) parent = film.node(f.node->parentId);
            else if (!f.node && kind != NodeKind::Sequence) {
                fx.errors << f.error;
                return;
            }
        }
        if (!parent && kind != NodeKind::Sequence) {
            fx.errors << QStringLiteral("%1: give parent (an id, a position like 1.2 or a title)").arg(capitalised(containmentRule(kind)));
            return;
        }
        if (!canContain(parent ? std::optional<NodeKind>(parent->kind) : std::nullopt, kind)) {
            fx.errors << QStringLiteral("%1, not %2").arg(capitalised(containmentRule(kind)),
                                                          parent ? QStringLiteral("in ") + film.nameOf(parent->id)
                                                                 : QStringLiteral("at the top level"));
            return;
        }
        const QString parentId = parent ? parent->id : QString();
        const QString parentName = parent ? film.nameOf(parentId) : QStringLiteral("the film");
        if (parent && refuseLocked(parentId)) return;
        if (film.size() >= limits.maxNodes) {
            fx.errors << QStringLiteral("the film has %1 nodes, the most it may have").arg(film.size());
            return;
        }
        const QStringList siblings = film.children(parentId);
        if (siblings.size() >= limits.maxChildren) {
            fx.errors << QStringLiteral("%1 already holds %2 nodes, the most one may hold").arg(capitalised(parentName)).arg(siblings.size());
            return;
        }
        if (!givenId.isEmpty() && !checkGivenId(givenId)) return;
        const std::optional<int> at = position(after, siblings, parentName, true);
        if (!at) return;
        Node n;
        n.id = givenId.isEmpty() ? newId(kIdPrefix[size_t(kind)]) : givenId;
        n.parentId = parentId;
        n.kind = kind;
        applyPatch(n.data, p);
        if (findField(nodeFields(kind), u"title") && !n.data.contains(QStringLiteral("title")))
            n.data.insert(QStringLiteral("title"), QStringLiteral("New ") + kindName(kind));   // ops.ts newScene
        if (kind == NodeKind::Shot && !n.data.contains(QStringLiteral("duration")))
            n.data.insert(QStringLiteral("duration"), 4);
        n.stale = true;
        film.insertNode(n, *at);
        fx.touched << n.id;
        fx.applied << QStringLiteral("added %1 (%2)").arg(film.nameOf(n.id), n.id);
    }

    void deleteNode(const Op& o) {
        const Node* n = findNode(takeTarget(), "delete_node", o.kind);
        if (!n) return;
        const QString id = n->id;
        if (refuseLocked(id)) return;
        const QStringList sub = film.subtree(id);
        for (const QString& d : sub)
            if (d != id && film.node(d)->locked) {
                fx.errors << QStringLiteral("%1 holds locked %2; unlock it first").arg(capitalised(film.nameOf(id)), film.nameOf(d));
                return;
            }
        const QString name = film.nameOf(id);
        film.removeSubtree(id);
        fx.applied << (sub.size() > 1 ? QStringLiteral("removed %1 and the %2 node%3 in it")
                                              .arg(name).arg(sub.size() - 1).arg(sub.size() > 2 ? QStringLiteral("s") : QString())
                                      : QStringLiteral("removed %1").arg(name));
    }

    void moveNode(const Op& o) {
        const QString ref = takeTarget();
        const QString parentRef = takeParent();
        const After after = takeAfter();
        const QStringList extra = leftovers();
        const Node* n = findNode(ref, "move_node", o.kind);
        if (!n) return;
        if (!extra.isEmpty()) {
            fx.errors << QStringLiteral("move_node takes node, parent and after (not \"%1\")").arg(extra.join(QStringLiteral("\", \"")));
            return;
        }
        const QString id = n->id, oldParent = n->parentId;
        const QString name = film.nameOf(id);
        QString parentId = oldParent;
        if (!parentRef.trimmed().isEmpty()) {
            const Film::Found f = film.resolve(parentRef);
            if (!f.node) {
                fx.errors << f.error;
                return;
            }
            if (f.node->id == id || film.isAncestor(id, f.node->id)) {
                fx.errors << QStringLiteral("cannot move %1 into itself").arg(name);
                return;
            }
            parentId = f.node->id;
        }
        const Node* parent = parentId.isEmpty() ? nullptr : film.node(parentId);
        if (!canContain(parent ? std::optional<NodeKind>(parent->kind) : std::nullopt, n->kind)) {
            fx.errors << QStringLiteral("%1, not %2").arg(capitalised(containmentRule(n->kind)),
                                                          parent ? QStringLiteral("in ") + film.nameOf(parentId)
                                                                 : QStringLiteral("at the top level"));
            return;
        }
        const QString parentName = parent ? film.nameOf(parentId) : QStringLiteral("the film");
        if (parentId == oldParent) {
            // reordering the children of a locked node changes it; a locked node itself may move among its siblings
            if (parent && refuseLocked(parentId)) return;
        } else {
            if (refuseLocked(id)) return;
            if (!oldParent.isEmpty() && refuseLocked(oldParent)) return;
            if (parent && refuseLocked(parentId)) return;
            if (film.children(parentId).size() >= limits.maxChildren) {
                fx.errors << QStringLiteral("%1 already holds %2 nodes, the most one may hold").arg(capitalised(parentName)).arg(film.children(parentId).size());
                return;
            }
        }
        QStringList rest = film.children(parentId);
        rest.removeAll(id);
        // ops.ts: no position given = the start
        const std::optional<int> at = position(after, rest, parentName, false, id);
        if (!at) return;
        if (parentId == oldParent && *at == film.indexOf(id)) {
            fx.unchanged << QStringLiteral("%1 is already there").arg(capitalised(name));
            return;
        }
        film.moveNode(id, parentId, *at);
        fx.applied << QStringLiteral("moved %1 to %2").arg(name, film.nameOf(id));
    }

    void lock(const Op& o, bool on) {
        const Node* n = findNode(takeTarget(), on ? "lock" : "unlock", o.kind);
        if (!n) return;
        const QString id = n->id, name = film.nameOf(id);
        if (n->locked == on) {
            fx.unchanged << QStringLiteral("%1 is already %2").arg(capitalised(name), on ? QStringLiteral("locked") : QStringLiteral("unlocked"));
            return;
        }
        if (!on) {
            film.nodeMut(id)->locked = false;
            fx.applied << QStringLiteral("unlocked %1").arg(name);
            return;
        }
        film.nodeMut(id)->locked = true;
        fx.applied << QStringLiteral("locked %1").arg(name);
    }

    void regenerate(const Op& o) {
        const Node* n = findNode(takeTarget(), "regenerate", o.kind);
        if (!n) return;
        const QString id = n->id;
        // stricter than ops.ts: a locked node keeps what it has (automation never touches it)
        if (refuseLocked(id)) return;
        int marked = 0;
        for (const QString& d : film.subtree(id)) {
            if (film.lockedAt(d)) continue;
            film.nodeMut(d)->stale = true;
            fx.staled << d;
            ++marked;
        }
        fx.applied << (marked > 1 ? QStringLiteral("%1 and %2 node%3 in it queued for regeneration")
                                         .arg(film.nameOf(id)).arg(marked - 1).arg(marked > 2 ? QStringLiteral("s") : QString())
                                   : QStringLiteral("%1 queued for regeneration").arg(film.nameOf(id)));
    }

    // ---- the bible
    bool uniqueName(BibleKind k, const QString& name, const QString& except) {
        for (const BibleEntry* e : film.entries(k))
            if (e->id != except && e->name.compare(name, Qt::CaseInsensitive) == 0) {
                fx.errors << QStringLiteral("there is already a %1 called %2").arg(bibleKindName(k), quoted(e->name));
                return false;
            }
        return true;
    }
    bool takeRefs(QStringList* out, bool* given) {
        const QJsonValue v = takeValue({"refs", "references", "ref_artifacts", "images", "reference_images"});
        *given = !(v.isUndefined() || v.isNull());
        if (!*given) return true;
        QString err;
        if (!checkRefs(v, out, &err)) {
            fx.errors << err;
            return false;
        }
        return true;
    }
    bool styleComplete(const QJsonObject& data) {
        const QString preset = data.value(QStringLiteral("preset")).toString();
        if (preset.isEmpty()) {
            fx.errors << QStringLiteral("a style needs a preset (one of: %1, custom)").arg(stylePresets().join(QStringLiteral(", ")));
            return false;
        }
        if (preset == QLatin1String("custom") && data.value(QStringLiteral("custom")).toString().trimmed().isEmpty()) {
            fx.errors << QStringLiteral("a custom style needs a description (custom)");
            return false;
        }
        return true;
    }

    void addBible(const Op& o) {
        if (!o.bibleKind) {
            fx.errors << QStringLiteral("add_bible needs a kind (character, location, prop, style or voice)");
            return;
        }
        const BibleKind k = *o.bibleKind;
        const QString kname = bibleKindName(k);
        const QString givenId = takeString({"id", "new_id"});
        const QString rawName = takeString({"name", "title", "called"});
        bool refsGiven = false;
        QStringList refs;
        if (!takeRefs(&refs, &refsGiven)) return;
        const Patch p = parseFields(bibleFields(k), kname);
        if (!p.ok) return;
        const QString name = cleanText(rawName, kMaxNameLen);
        if (name.isEmpty()) {
            fx.errors << QStringLiteral("a %1 needs a name").arg(kname);
            return;
        }
        if (name.size() > kMaxNameLen) {
            fx.errors << QStringLiteral("the name of a %1 is too long (%2 characters; at most %3)").arg(kname).arg(name.size()).arg(kMaxNameLen);
            return;
        }
        if (k == BibleKind::Style && !film.entries(BibleKind::Style).isEmpty()) {
            fx.errors << QStringLiteral("the film already has a style (%1); change it with set_style")
                             .arg(quoted(film.entries(BibleKind::Style).first()->name));
            return;
        }
        if (!uniqueName(k, name, {})) return;
        if (film.bibleSize() >= limits.maxBible) {
            fx.errors << QStringLiteral("the bible has %1 entries, the most it may have").arg(film.bibleSize());
            return;
        }
        if (!givenId.isEmpty() && !checkGivenId(givenId)) return;
        BibleEntry e;
        e.id = givenId.isEmpty() ? newId(kBiblePrefix[size_t(k)]) : givenId;
        e.kind = k;
        e.name = name;
        applyPatch(e.data, p);
        e.refs = refs;
        if (k == BibleKind::Style && !styleComplete(e.data)) return;
        film.putEntry(e);
        fx.bible << e.id;
        fx.applied << QStringLiteral("added %1 (%2)").arg(film.entryName(e.id), e.id);
    }

    // "character": "Ann" names the entry of update_character; a key of another kind ("voice" of a character) is a
    // field, and so is the kind's own key once the entry is named otherwise ({id, voice} of update_voice).
    QStringList kindRefKeys(const Op& o) const {
        if (!o.bibleKind) return {};
        const QString v = bibleKindName(*o.bibleKind);
        return {v + QStringLiteral("_id"), v};
    }
    QString takeEntryRef(const Op& o, bool nameIsRef) {
        QString r = takeString({"entry", "entry_id", "bible_id", "id", "target"});
        for (const QString& k : kindRefKeys(o))
            if (r.isEmpty()) r = takeString(QStringList{k});
        if (r.isEmpty() && nameIsRef) r = takeString({"name"});
        return r;
    }

    void updateBible(const Op& o) {
        const bool explicitRef = has({"entry", "entry_id", "bible_id", "id", "target"}) || has(kindRefKeys(o));
        const QString ref = takeEntryRef(o, !explicitRef);
        const QList<BibleKind> kinds = o.bibleKind ? QList<BibleKind>{*o.bibleKind} : allBibleKinds();
        QString err;
        const QString id = film.resolveEntry(ref, kinds, &err);
        if (id.isEmpty()) {
            fx.errors << (ref.trimmed().isEmpty() ? QStringLiteral("update_bible needs the entry to change (entry: an id or a name)") : err);
            return;
        }
        BibleEntry e = *film.entry(id);
        const QString oldName = film.entryName(id);
        const QString rename = cleanText(takeString({"name", "new_name", "rename", "title"}), kMaxNameLen);
        bool refsGiven = false;
        QStringList refs;
        if (!takeRefs(&refs, &refsGiven)) return;
        const Patch p = parseFields(bibleFields(e.kind), bibleKindName(e.kind));
        if (!p.ok) return;
        if (!anyGiven(p) && rename.isEmpty() && !refsGiven) {
            fx.errors << QStringLiteral("nothing to change in %1: give at least one of name, refs, %2")
                             .arg(oldName, fieldList(bibleFields(e.kind)));
            return;
        }
        if (rename.size() > kMaxNameLen) {
            fx.errors << QStringLiteral("the name is too long (%1 characters; at most %2)").arg(rename.size()).arg(kMaxNameLen);
            return;
        }
        QStringList changed = applyPatch(e.data, p);
        if (!rename.isEmpty() && rename != e.name) {
            if (!uniqueName(e.kind, rename, id)) return;
            e.name = rename;
            changed << QStringLiteral("name");
        }
        if (refsGiven && refs != e.refs) {
            e.refs = refs;
            changed << QStringLiteral("refs");
        }
        if (changed.isEmpty()) {
            fx.unchanged << QStringLiteral("%1 already has that").arg(capitalised(oldName));
            return;
        }
        if (e.kind == BibleKind::Style && !styleComplete(e.data)) return;
        film.putEntry(e);
        fx.bible << id;
        fx.applied << QStringLiteral("%1: %2").arg(oldName, label(changed.join(QStringLiteral(", ")) + QStringLiteral(" changed")));
    }

    void removeBible(const Op& o) {
        const QString ref = takeEntryRef(o, true);
        const QList<BibleKind> kinds = o.bibleKind ? QList<BibleKind>{*o.bibleKind} : allBibleKinds();
        QString err;
        const QString id = film.resolveEntry(ref, kinds, &err);
        if (id.isEmpty()) {
            fx.errors << (ref.trimmed().isEmpty() ? QStringLiteral("remove_bible needs the entry to remove (entry: an id or a name)") : err);
            return;
        }
        const Users u = usersOf(film, id);
        if (!u.nodes.isEmpty() || !u.entries.isEmpty()) {
            QStringList names;
            for (const QString& n : u.nodes) names << film.nameOf(n);
            for (const QString& e : u.entries) names << film.entryName(e).mid(4);   // without "the "
            const int total = int(names.size());
            fx.errors << QStringLiteral("%1 is used by %2%3; remove it from %4 first")
                             .arg(capitalised(film.entryName(id)), names.mid(0, 3).join(QStringLiteral(", ")),
                                  total > 3 ? QStringLiteral(" and %1 more").arg(total - 3) : QString(),
                                  total > 1 ? QStringLiteral("them") : QStringLiteral("it"));
            return;
        }
        const QString name = film.entryName(id);
        film.removeEntry(id);
        fx.applied << QStringLiteral("removed %1").arg(name);
    }

    void setStyle() {
        const QString given = takeString({"style_id", "style", "preset", "value", "id", "name"}).trimmed();
        const QString custom = cleanText(takeString({"custom", "custom_style", "description"}), 2000);
        if (given.isEmpty()) {
            fx.errors << QStringLiteral("set_style needs a style (one of: %1, custom)").arg(stylePresets().join(QStringLiteral(", ")));
            return;
        }
        QString preset;
        for (const QString& s : stylePresets() + QStringList{QStringLiteral("custom")})
            if (s.compare(given, Qt::CaseInsensitive) == 0) preset = s;
        if (preset.isEmpty()) {
            fx.errors << QStringLiteral("unknown style \"%1\" (styles: %2, custom)").arg(given.left(60), stylePresets().join(QStringLiteral(", ")));
            return;
        }
        // ops.ts: a custom style takes its description from the note
        const QString desc = custom.isEmpty() && preset == QLatin1String("custom") ? cleanText(note, 2000) : custom;
        if (preset == QLatin1String("custom") && desc.isEmpty()) {
            fx.errors << QStringLiteral("a custom style needs a description (custom)");
            return;
        }
        const QList<const BibleEntry*> styles = film.entries(BibleKind::Style);
        BibleEntry e;
        if (!styles.isEmpty()) e = *styles.first();
        else {
            e.id = newId(kBiblePrefix[size_t(BibleKind::Style)]);
            e.kind = BibleKind::Style;
            e.name = QStringLiteral("Style");
        }
        QJsonObject data = e.data;
        data.insert(QStringLiteral("preset"), preset);
        if (!desc.isEmpty()) data.insert(QStringLiteral("custom"), desc);
        else if (preset != QLatin1String("custom")) data.remove(QStringLiteral("custom"));
        if (!styles.isEmpty() && data == e.data) {
            fx.unchanged << QStringLiteral("the style is already %1").arg(preset);
            return;
        }
        e.data = data;
        film.putEntry(e);
        fx.bible << e.id;
        fx.applied << QStringLiteral("style → %1").arg(preset);
    }

    void setAspect() {
        const QString v = takeString({"aspect", "aspect_ratio", "ratio", "value"}).trimmed();
        if (!aspects().contains(v)) {
            fx.errors << QStringLiteral("aspect must be one of: %1").arg(aspects().join(QStringLiteral(", ")));
            return;
        }
        if (film.settings().aspect == v) {
            fx.unchanged << QStringLiteral("the aspect is already %1").arg(v);
            return;
        }
        film.settingsMut().aspect = v;
        fx.applied << QStringLiteral("aspect → %1").arg(v);
    }

    void setLanguage() {
        const QString v = takeString({"language", "lang", "value"}).trimmed();
        static const QRegularExpression code(QStringLiteral("^[A-Za-z]{2,3}(-[A-Za-z0-9]{2,8}){0,3}$"));
        if (v.isEmpty()) {
            fx.errors << QStringLiteral("set_language needs a language code (en, es, pt-BR ...)");
            return;
        }
        if (!code.match(v).hasMatch()) {
            fx.errors << QStringLiteral("\"%1\" is not a language code (en, es, pt-BR ...)").arg(v.left(40));
            return;
        }
        if (film.settings().language == v) {
            fx.unchanged << QStringLiteral("the language is already %1").arg(v);
            return;
        }
        film.settingsMut().language = v;
        fx.applied << QStringLiteral("language → %1").arg(v);
    }

    void setLength() {
        const QJsonValue v = takeValue({"target_seconds", "seconds", "length", "duration", "value"});
        bool ok = false;
        double s = v.isDouble() ? v.toDouble() : v.toString().trimmed().toDouble(&ok);
        if (v.isDouble()) ok = std::isfinite(s);
        if (!ok || !(s >= 5)) {
            fx.errors << QStringLiteral("target length must be at least 5 s");
            return;
        }
        if (s > 4 * 3600) {
            fx.errors << QStringLiteral("target length must be at most 14400 s (4 h)");
            return;
        }
        s = std::round(s * 100) / 100;
        if (film.settings().targetSeconds == s) {
            fx.unchanged << QStringLiteral("the target length is already %1 s").arg(json::jsNumber(s));
            return;
        }
        film.settingsMut().targetSeconds = s;
        fx.applied << QStringLiteral("length → ~%1 s").arg(qRound64(s));
    }
};

QStringList uniqueList(const QStringList& in) {
    QStringList out;
    QSet<QString> seen;
    for (const QString& s : in)
        if (!seen.contains(s)) {
            seen.insert(s);
            out << s;
        }
    return out;
}

bool inScope(const Film& prev, const Film& next, const QString& id, const QStringList& scope) {
    for (const QString& s : scope)
        if (s == id || prev.isAncestor(s, id) || next.isAncestor(s, id)) return true;
    return false;
}

} // namespace

QJsonObject OpResult::toJson() const {
    return QJsonObject{
        {QStringLiteral("applied"), QJsonArray::fromStringList(applied)},
        {QStringLiteral("errors"), QJsonArray::fromStringList(errors)},
        {QStringLiteral("unchanged"), QJsonArray::fromStringList(unchanged)},
        {QStringLiteral("touched"), QJsonArray::fromStringList(touched)},
        {QStringLiteral("stale"), QJsonArray::fromStringList(stale)},
        {QStringLiteral("lockedDrift"), QJsonArray::fromStringList(lockedDrift)},
        {QStringLiteral("bibleTouched"), QJsonArray::fromStringList(bibleTouched)},
        {QStringLiteral("rolledBack"), rolledBack},
    };
}

OpResult applyOps(const Film& film, const QJsonArray& ops, const ApplyOptions& o) {
    OpResult r;
    r.film = film;
    if (ops.size() > o.limits.maxOps) {
        r.errors << QStringLiteral("too many operations (%1); send at most %2 per call").arg(ops.size()).arg(o.limits.maxOps);
        r.rolledBack = true;
        return r;
    }
    Effects total;
    const bool guarded = (o.locks && o.locks->size() > 0) || o.scope.has_value();
    for (int i = 0; i < ops.size(); ++i) {
        const Op op = normalizeOp(ops.at(i));
        if (!guarded) {
            Applier(r.film, o.limits, total).run(op, i);
            continue;
        }
        // session.ts applyAgentOps: while steps run (or for a side task) the ops go in one by one, and one that
        // touches something held by somebody else is refused with its own sentence
        Film cand = r.film;
        Effects e;
        Applier(cand, o.limits, e).run(op, i);
        total.errors << e.errors;
        total.unchanged << e.unchanged;
        if (e.applied.isEmpty()) continue;
        if (o.scope) {
            const FilmDiff d = diffFilm(r.film, cand);
            bool structural = !d.reorderedParents.isEmpty() || !d.fields.isEmpty();
            QStringList outside;
            for (const QString& id : d.nodes) {
                const Node* a = r.film.node(id);
                const Node* b = cand.node(id);
                if (!a || !b || a->parentId != b->parentId) structural = true;
                else if (!inScope(r.film, cand, id, *o.scope)) outside << id;
            }
            if (structural) {
                total.errors << QStringLiteral("A side task may only change its own nodes (it cannot add, remove or move "
                                               "nodes, or change the bible or the settings).");
                continue;
            }
            if (!outside.isEmpty()) {
                outside.sort();
                total.errors << QStringLiteral("%1 %2 not one of this side task's nodes (%3).")
                                    .arg(namesForSentence(r.film, outside), outside.size() == 1 ? QStringLiteral("is") : QStringLiteral("are"),
                                         o.scope->join(QStringLiteral(", ")));
                continue;
            }
        }
        if (o.locks) {
            if (const std::optional<Conflict> c = o.locks->checkEdit(film.projectId(), r.film, cand, o.owner)) {
                total.errors << c->message;
                continue;
            }
        }
        r.film = cand;
        total.applied << e.applied;
        total.touched << e.touched;
        total.bible << e.bible;
        total.staled << e.staled;
    }
    r.errors = total.errors;
    r.unchanged = total.unchanged;
    if (o.atomic && !r.errors.isEmpty()) {
        r.film = film;
        r.rolledBack = true;
        return r;
    }
    r.applied = total.applied;
    const StaleReport sr = propagate(r.film, changesBetween(film, r.film));
    QStringList st;
    for (const QString& id : uniqueList(total.staled + sr.changed))
        if (r.film.node(id)) st << id;
    r.stale = st;
    r.lockedDrift = sr.lockedDrift;
    for (const QString& id : uniqueList(total.touched))
        if (r.film.node(id)) r.touched << id;
    for (const QString& id : uniqueList(total.bible))
        if (r.film.entry(id)) r.bibleTouched << id;
    return r;
}

} // namespace studio::film
