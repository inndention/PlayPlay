#pragma once
// The film model (ST-21, docs/plans/starmedia-studio.md §4.6, §4.8): the editable film tree, its bible and settings,
// validated operations, and how it is stored in studio.db (film_nodes, bible, projects.brief_json).
//
// The tree: sequence > scene > shot / beat / line / cue.
//   sequence  top level                            title, summary, video_model (one video model per sequence)
//   scene     in a sequence                        title, summary, location, characters, time_of_day, mood
//   shot      in a scene                           title, visual, camera, duration, transition, source, characters,
//                                                  location, props, on_screen
//   beat      in a scene                           title, summary
//   line      in a scene or a shot (dialogue)      speaker, text, delivery, voice
//   cue       in a sequence, a scene or a shot     cue_kind (music | sfx | ambience), description, at, duration
// A shot without characters / location of its own has its scene's (inherited: stale.h follows them). Node fields are
// the shared schema of bible.h; output fields (a sequence's anchor picture, a line's recorded voice) are written by
// generation through setOutputs(), never by an op.
//
// Operations (applyOps) port StarMedia src/studio/ops.ts applyOps / normalizeOp to the tree: the editor, the agents
// and the automation edit a film the same way, and an operation that cannot be applied is reported with a sentence
// saying why (the model can correct it instead of resending it). Rules kept from ops.ts:
//   - the input is never mutated; ops apply in order, each on its own (a refused op does not stop the others);
//   - loosely named ops and fields are mapped when the mapping is unambiguous ("edit_scene", "add_shot",
//     "description" for a shot's visual, nested {changes:{...}});
//   - a node is named by its id, its position ("1.2.3", "shot 1.2.3", "#1.2") or its title;
//   - "" leaves a field as it is, "-" clears it; nothing to change is an error, a value already there is "unchanged";
//   - a locked node refuses updates, deletion and graphics (here: everything but reordering among its siblings); a
//     locked node also freezes everything under it;
//   - insert after "" / "start" = first, after "end" (or no after) = last; move without after = first;
//   - a style change makes every unlocked shot stale (here through the input hashes, stale.h); an unknown style is
//     refused; the target length is at least 5 s.
// Stricter than ops.ts (a film is edited by many agents at once): unknown fields, out-of-range numbers, unknown enum
// values, ambiguous titles, ids from another project and regenerating a locked node are refused with a reason
// instead of being ignored, clamped or picked first. With RunLocks (locks.h) every op is checked against the steps
// running on the film (session.ts applyAgentOps): an op that touches something a step holds is refused with the
// lock's sentence and the others still apply.
//
// Film is a value type (implicitly shared Qt containers: copies are cheap until written). Use one Film per project on
// one thread (the service's event loop), apply ops to it and persist the difference with saveWrite(prev, next).
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <optional>

#include "film/bible.h"
#include "store/dbwriter.h"

namespace studio::film {

class RunLocks;

// ---------------------------------------------------------------- kinds

enum class NodeKind { Sequence, Scene, Shot, Beat, Line, Cue };
QString kindName(NodeKind k);                             // "sequence" ...
std::optional<NodeKind> parseKind(QStringView s);         // any case, plurals too
const QList<NodeKind>& allKinds();
/// May `child` go in `parent` (nullopt = the top level)?
bool canContain(std::optional<NodeKind> parent, NodeKind child);
/// "a shot goes in a scene" (for refusals).
QString containmentRule(NodeKind child);
const QList<FieldSpec>& nodeFields(NodeKind k);

/// The transitions of the composer (compose/timeline.h Transition names) and the shot sources of ops.ts SOURCES.
const QStringList& transitions();
const QStringList& sources();
/// The output aspects of the composer: "9:16", "16:9", "1:1", "4:5".
const QStringList& aspects();

// ---------------------------------------------------------------- nodes and the film

struct Node {
    QString id, projectId, parentId;       // parentId "" = top level
    NodeKind kind = NodeKind::Shot;
    QString orderKey;                      // sibling order in the DB (fixed-width digits; see Film)
    QJsonObject data;                      // the fields of nodeFields(kind)
    QString inputHash;                     // the inputs its outputs were last checked against (stale.h)
    bool locked = false;
    bool stale = true;                     // its outputs (media, or the children written from it) are out of date
    QString chosenTakeId;
    qint64 updatedMs = 0;

    QString title() const { return data.value(QStringLiteral("title")).toString(); }
};
/// Every column but updated_ms.
bool sameRow(const Node& a, const Node& b);

struct Settings {
    QString aspect = QStringLiteral("16:9");
    QString language = QStringLiteral("en");
    double targetSeconds = 0;              // 0 = not set
    QJsonObject toJson() const;
};

struct Limits {
    int maxNodes = 50000;                  // a 100 min film is ~1,500 shots + ~1,200 lines (design §3)
    int maxChildren = 1000;
    int maxBible = 500;
    int maxOps = 200;                      // per applyOps call
};

class Film {
public:
    Film() = default;
    explicit Film(QString projectId) : m_project(std::move(projectId)) {}

    const QString& projectId() const { return m_project; }
    int size() const { return int(m_nodes.size()); }
    bool isEmpty() const { return m_nodes.isEmpty() && m_bible.isEmpty(); }

    // ---- the tree
    const Node* node(const QString& id) const;
    /// Ordered child ids of `parentId` ("" = the sequences).
    QStringList children(const QString& parentId = {}) const { return m_children.value(parentId); }
    const Node* parentOf(const Node& n) const { return n.parentId.isEmpty() ? nullptr : node(n.parentId); }
    /// The nearest ancestor (or `id` itself) of kind `k`.
    const Node* ancestorOfKind(const QString& id, NodeKind k) const;
    /// Is `a` a proper ancestor of `b`?
    bool isAncestor(const QString& a, const QString& b) const;
    /// `id` and everything under it, pre-order.
    QStringList subtree(const QString& id) const;
    /// Every node, pre-order (sequence 1, its scenes and their children, sequence 2 ...).
    QStringList allIds() const;
    int indexOf(const QString& id) const;  // among its siblings, 0-based; -1 when unknown
    /// "1.2.3": 1-based positions from the top (what the editor shows).
    QString pathOf(const QString& id) const;
    /// "shot 1.2.3 “Title”" (titles over 28 characters are cut, as runlocks.ts nameOf does).
    QString nameOf(const QString& id) const;
    /// The node itself or the nearest ancestor that is locked (a locked node freezes its subtree); nullptr if none.
    const Node* lockedAt(const QString& id) const;

    // ---- the bible and settings
    const BibleEntry* entry(const QString& id) const;
    QList<const BibleEntry*> entries(std::optional<BibleKind> kind = std::nullopt) const;   // by kind, then name
    int bibleSize() const { return int(m_bible.size()); }
    /// "the character “Ann”".
    QString entryName(const QString& id) const;
    const Settings& settings() const { return m_settings; }

    // ---- references as people and models write them
    struct Found {
        const Node* node = nullptr;
        QString error;                     // set when node is null: why ("no node", "several nodes are called")
    };
    /// An id (exact, then any case), a position ("1.2.3", "shot 1.2.3", "#1.2", "scene 2" = scene 2 of the only
    /// sequence) or a title (any case; refused when several nodes have it).
    Found resolve(QStringView ref) const;
    /// A bible entry of `kinds` by id or name (any case); "" with `error` when none fits.
    QString resolveEntry(const QString& ref, const QList<BibleKind>& kinds, QString* error) const;
    RefResolver refResolver() const;

    // ---- mutation (no validation: applyOps validates; load(), the tests and the film phases use these directly)
    /// Inserts `n` (its projectId is set to the film's) at `index` among its siblings (-1 or past the end = last) and
    /// gives it an order key.
    void insertNode(Node n, int index = -1);
    /// Removes `id` and its subtree.
    void removeSubtree(const QString& id);
    /// Moves `id` under `parentId` at `index` (computed without the node itself).
    void moveNode(const QString& id, const QString& parentId, int index);
    /// Direct access for the film module (stale.cpp, ops); nullptr when unknown.
    Node* nodeMut(const QString& id);
    void putEntry(BibleEntry e);
    void removeEntry(const QString& id);
    Settings& settingsMut() { return m_settings; }

    /// Writes generation outputs (output fields of the node's kind: an asset sha256, a duration) into its data; they
    /// are not inputs, so nothing becomes stale. False with a reason for an unknown node, a field that is not an output
    /// or a bad value. An empty value removes the field.
    bool setOutputs(const QString& id, const QJsonObject& outputs, QString* error = nullptr);
    bool setChosenTake(const QString& id, const QString& takeId);

    /// What is wrong with the tree: orphans, cycles, children of the wrong kind, dangling bible references, nodes of
    /// another project. Empty for a film applyOps produced.
    QStringList problems() const;

    // ---- JSON for the API and the agents' film reads
    QJsonObject nodeJson(const QString& id, int depth = 0) const;   // depth > 0: with children that deep
    QJsonObject toJson(int depth = 4) const;                        // settings, bible, the tree

    /// Loads one project's film (film_nodes, bible, projects.brief_json for the settings) on the caller's connection.
    /// Fails closed (nullopt and `error`) on a read error, an unknown kind, a data_json that is not an object, or a
    /// damaged tree (problems()).
    static std::optional<Film> load(const QSqlDatabase& db, const QString& projectId, QString* error = nullptr);

private:
    void placeKey(const QString& parentId, int index);   // order key of the child at `index` (renumbers if needed)
    void collect(const QString& id, QStringList& out) const;

    QString m_project;
    QHash<QString, Node> m_nodes;
    QHash<QString, QStringList> m_children;              // parent id ("" = top) -> ordered child ids
    QHash<QString, BibleEntry> m_bible;
    Settings m_settings;
};

// ---------------------------------------------------------------- order keys

namespace order {
constexpr int kWidth = 12;                 // "000001048576": text order = number order
constexpr qint64 kStep = qint64(1) << 20;  // gap between renumbered siblings (~a million appends before a renumber)
QString key(qint64 v);
/// The number of a key; -1 when it is not kWidth digits.
qint64 value(const QString& key);
} // namespace order

// ---------------------------------------------------------------- operations

/// One operation as applyOps understands it (normalizeOp: aliases resolved, keys normalised).
struct Op {
    QString name;                          // canonical op; "" when the op is unknown (raw holds what was sent)
    QString raw;
    std::optional<NodeKind> kind;          // from the op name ("add_shot") or "kind" / "type"
    std::optional<BibleKind> bibleKind;    // from the op name ("add_character") or "kind" / "type"
    QJsonObject args;                      // every other key, normalised (nested changes/fields/patch merged)
};
/// The canonical ops.
const QStringList& opNames();
Op normalizeOp(const QJsonValue& raw);

struct OpResult {
    Film film;                             // the film after the applied ops (the input when nothing applied)
    QStringList applied;                   // one sentence per applied op
    QStringList errors;                    // one sentence per refused op: why
    QStringList unchanged;                 // ops whose change was already there
    QStringList touched;                   // nodes an applied op changed or added (still present)
    QStringList stale;                     // nodes stale because of this call (inputs changed, new, regenerate)
    QStringList lockedDrift;               // locked nodes whose inputs changed (left as they were; stale on unlock)
    QStringList bibleTouched;              // entries an applied op added or changed (still present)
    bool rolledBack = false;               // atomic and something was refused: nothing applied
    QJsonObject toJson() const;
};

struct ApplyOptions {
    /// The steps running on films. When it holds any lock, every op is checked against it (checkEdit) and an op that
    /// touches what another holder holds is refused with the lock's sentence.
    const RunLocks* locks = nullptr;
    QString owner = QStringLiteral("user");   // who edits: "user", "main" (the chat agent), a side task's id
    /// A side task: the only nodes (with their subtrees) its ops may change; it may not add, remove or move nodes,
    /// nor change the bible or the settings.
    std::optional<QStringList> scope;
    bool atomic = false;                   // any refusal: apply nothing
    Limits limits;
};

/// Applies `ops` (an array of objects) to a copy of `film`, then recomputes the input hashes the ops can have changed
/// and marks those nodes stale (stale.h propagate(Changes)). Assumes the stored hashes of `film` are current (a loaded
/// film gets one full propagate() first).
OpResult applyOps(const Film& film, const QJsonArray& ops, const ApplyOptions& o = {});

// ---------------------------------------------------------------- persistence

/// The rows that turn `prev` into `next` (the same project), as one Batched write: changed and new nodes and bible
/// entries upserted, removed ones deleted (with the takes of removed nodes), changed settings merged into
/// projects.brief_json with json_set (other brief keys are kept; the project row is created when missing).
/// updated_ms is the writer's clock. The write fails as a whole (nothing of it lands) when a node or entry id already
/// belongs to another project or brief_json is not valid JSON. Rows are captured now: the write may run later.
/// On a failed save the in-memory film is ahead of the DB and would fail the same way again: drop it and load() again
/// (the DB is the truth). Ids given by callers (restart-safe expansion) should carry the project id so they cannot
/// collide with another project's.
store::Write saveWrite(const Film& prev, const Film& next);

} // namespace studio::film
