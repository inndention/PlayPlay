#pragma once
// Run locks on a film (ST-21): port of StarMedia src/studio/runlocks.ts to the film tree.
//
// What a running step may hold, so people and agents can keep editing everything else. A step declares when it starts
// what it will write:
//   film    the whole film (a step that rewrites everything): nothing in the film may change until it ends
//   nodes   the nodes it writes, each WITH ITS SUBTREE (a step writing scene 3's shots holds scene 3; a keyframe step
//           holds the shots it draws)
//   writes  film-level things it writes ("anchor:<sequence id>": the first keyframe of a sequence becomes its anchor;
//           every shot step may set it, first writer wins: runs share it, edits do not)
//   reads   film-level things it reads and needs unchanged ("style", "aspect", "language", "bible:<entry id>")
// and an edit goes through when it changes none of that. An edit that touches a held node is REFUSED (never queued or
// merged: the step's "stale = false" would otherwise overwrite the person's change), with a sentence that names the
// node and what is busy with it. A lock is a `run` (a step executing) or a `reserve` (a side task owning its nodes for
// its whole life); a side task's reservation and the steps it starts never block each other.
//
// The tree adds one rule to runlocks.ts: a held node's INPUTS must not change either. checkEdit() compares the input
// hash (stale.h) of every held node before and after the edit, so editing the character a held shot shows, the scene
// location it inherits or its sequence's video model is refused even when the step did not list it in `reads`.
//
// Not ported: landInPlace / overlayState (runlocks.ts L304-339). They keep the browser's live storyboard object and its
// undo history consistent while a step writes into it; here a step writes through the film model and the DB writer.
//
// Pure data and functions with an injectable clock; use one RunLocks on one thread (the service's event loop).
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>
#include <map>
#include <optional>

#include "film/model.h"

namespace studio {
class Clock;
}

namespace studio::film {

enum class LockKind { Run, Reserve };

struct LockScope {
    bool film = false;                     // writes the whole film
    QStringList nodes;                     // node ids it writes (each with its subtree)
    QStringList writes;                    // film-level keys it writes
    QStringList reads;                     // film-level keys it reads and needs unchanged
};

struct LockSpec {
    QString projectId;
    QString owner;                         // "user" (a step started by hand), "main" (the chat agent), a side task id
    LockKind kind = LockKind::Run;
    QString phase;                         // "assets", "writing" ... or "branch" for a side task's reservation
    QString label;                         // reads in "Shot 1.2.3 is busy (<label>)": "generating shots"
    LockScope scope;
};

/// A held lock as everything else sees it (a snapshot).
struct LockInfo {
    QString id, projectId, owner;
    LockKind kind = LockKind::Run;
    QString phase, label;
    qint64 since = 0;
    bool film = false;
    QStringList nodes, writes, reads;
    QJsonObject toJson() const;
};

struct Conflict {
    enum class Kind { Film, Node, Field };
    Kind kind = Kind::Node;
    LockInfo lock;
    QStringList nodeIds;
    QStringList fields;
    QString message;                       // a sentence for the person and the agent: what, held by what, what to do
    QJsonObject toJson() const;
};

struct Acquired {
    bool ok = false;
    QString id;                            // when ok
    Conflict conflict;                     // when not
};

// ---------------------------------------------------------------- comparing films

/// Film-level keys every shot step may write (first writer wins): "anchor:<sequence id>".
bool isSharedWrite(QStringView key);
/// JSON text with sorted keys (runlocks.ts canon; json::stable).
QString canon(const QJsonValue& v);
/// A node without what generation writes (its output fields, chosen take, stale flag, hash, order key): what a step
/// READ (runlocks.ts sceneInputs).
QJsonObject nodeInputs(const Node& n);
bool sameInputs(const Node& a, const Node& b);

struct FilmDiff {
    QSet<QString> nodes;                   // nodes whose content changed, nodes added, nodes removed
    QSet<QString> reorderedParents;        // parents whose children now come in another order (not a content change)
    QSet<QString> fields;                  // "aspect" "language" "target_seconds" "style" "bible:<id>" "anchor:<seq>"
    bool empty() const { return nodes.isEmpty() && reorderedParents.isEmpty() && fields.isEmpty(); }
};
/// What changed between two films. A node's content is its kind, parent, data, locked and stale flags and chosen take
/// (runlocks.ts compares whole scenes, outputs included); its position among its siblings, its hash and stamps are not
/// content. Compare films BEFORE propagate() (applyOps does): staleness derived from an edit elsewhere is not an edit of
/// the node, while an explicit "regenerate" of a held node is. A bible entry is "bible:<id>" (and "style" for a style
/// entry) when it is added, removed or changed in a way generation sees (entryHash, or a character's voice; notes are
/// not); a sequence whose anchor_asset changed is also "anchor:<id>".
FilmDiff diffFilm(const Film& prev, const Film& next);

/// The lock scope of a generation step on `nodes` (a shot step): those nodes, writes the anchor of their sequences,
/// reads everything their inputs read (keysUsed() of each node in their subtrees). The film's GENERATION_INPUT_FIELDS.
LockScope generationScope(const Film& f, const QStringList& nodes);

// ---------------------------------------------------------------- the registry

class RunLocks {
public:
    explicit RunLocks(const Clock* clock = nullptr);   // nullptr = Clock::system()

    /// Called whenever a lock is taken, narrowed or released. Returns an id for removeListener.
    int onChange(std::function<void()> fn);
    void removeListener(int id);

    QList<LockInfo> list(const QString& projectId = {}) const;
    std::optional<LockInfo> get(const QString& id) const;
    int size() const { return int(m_held.size()); }
    /// Steps running under a lock (reservations are not steps).
    int runCount(const QString& projectId = {}) const;
    /// A step writes into this film right now.
    bool writerActive(const QString& projectId) const;
    /// Node id -> the locks naming it (for the badges on the cards). With `forOwner`, leaves out what that owner's own
    /// reservations hold (its edits may change those nodes).
    QHash<QString, QList<LockInfo>> heldNodes(const QString& projectId, const QString& forOwner = {}) const;
    /// The whole-film lock on this project, if any.
    std::optional<LockInfo> filmLock(const QString& projectId) const;

    /// Takes a lock, or says what is in the way. Two holders conflict when both write and their scopes overlap: a
    /// whole-film writer with any writer, the same node or one inside the other's subtree (needs `film`; without it
    /// only equal ids overlap), the same key (except shared writes), or a key one reads and the other writes. A step
    /// that only reads (an empty scope) never conflicts. A side task's reservation and the steps that task starts do
    /// not block each other.
    Acquired acquire(const LockSpec& spec, const Film* film = nullptr);
    /// What stands in the way of this lock, without taking it.
    std::optional<Conflict> canAcquire(const LockSpec& spec, const Film* film = nullptr) const;
    bool release(const QString& id);
    /// A step finished with some of its nodes: they are free now, the rest stay held.
    bool releaseNodes(const QString& id, const QStringList& nodeIds);
    /// A step keeps working on fewer nodes.
    void narrowNodes(const QString& id, const QStringList& keep);

    /// Would changing `prev` into `next` touch anything a lock held by somebody else guards? `owner` is the one
    /// editing (its own reservations never block it). The first conflict: a whole-film lock, then held nodes (changed,
    /// added under, or removed), then declared keys, then a held node whose inputs the edit changes. Reordering nodes
    /// among their siblings is fine, held ones included (their content is untouched).
    std::optional<Conflict> checkEdit(const QString& projectId, const Film& prev, const Film& next,
                                      const QString& owner) const;
    /// Is any of these nodes (or a node above them) held by somebody else?
    std::optional<Conflict> checkNodes(const QString& projectId, const QStringList& nodeIds, const QString& owner,
                                       const Film* film = nullptr) const;

private:
    struct Held {
        QString id, projectId, owner;
        LockKind kind = LockKind::Run;
        QString phase, label;
        qint64 since = 0;
        bool film = false;
        QStringList nodes, writes, reads;
    };
    static LockInfo info(const Held& h);
    static bool writes(const Held& h) { return h.film || !h.nodes.isEmpty() || !h.writes.isEmpty(); }
    static bool ownReserve(const Held& h, const QString& owner) { return h.kind == LockKind::Reserve && h.owner == owner; }
    std::optional<Conflict> conflictBetween(const Held& mine, const LockSpec& spec, const Held& h, const Film* film) const;
    void changed();

    const Clock* m_clock;
    std::map<int, Held> m_heldByN;         // lock number -> lock (acquire order, so the first conflict is stable)
    QHash<QString, int> m_held;            // id -> lock number
    int m_n = 0;
    std::map<int, std::function<void()>> m_listeners;
    int m_nextListener = 0;
};

/// "Shot 1.2.3 “Title”" / "Shots 1.1.1 “a”, 1.1.3 “c”" / "Scene 1.2 “b”, shot 1.1.1 “a”" for a sentence (at most
/// three names, then "and N more").
QString namesForSentence(const Film& f, const QStringList& ids);
/// The sentence for an edit that a whole-film step refuses (runlocks.ts wholeBoardMessage).
QString wholeFilmMessage(const LockInfo& lock);

} // namespace studio::film
