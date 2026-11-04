#pragma once
// Input hashes and stale propagation (ST-21, docs/plans/starmedia-studio.md §4.3: "a re-run after an edit regenerates
// only nodes whose input hash changed"; the studio's "edits mark only affected shots stale", README L148).
//
// A node's input hash covers exactly what its outputs are made from:
//   sequence  summary, video_model + the style + the aspect          (its scenes are written from it)
//   scene     summary, time_of_day, mood, the ids of its location and characters
//   shot      visual, camera, source, duration + the bible entries it shows (its characters, location and props; its
//             scene's when it has none of its own) + the style + the aspect + its sequence's video model
//   beat      summary
//   line      text, delivery + the voice it is spoken with (its own voice, else its speaker's) + the language
//   cue       cue_kind, description, duration + the style
// Titles, notes, transitions, on-screen text and positions are not inputs: changing them stales nothing. A bible entry
// counts by entryHash() (bible.h): editing a character's description stales the shots that show it and nothing else;
// pointing the character at another voice stales its lines and nothing else.
//
// propagate() recomputes every hash: a node whose hash differs from the stored one takes the new hash and becomes
// stale. A locked node (or one under a locked node) is never touched by automation (ops.ts: a style change stales only
// unlocked scenes): it keeps its hash and its flag and is reported as drift, so unlocking it later shows it stale.
// markFresh() is how a finished generation clears the flag: only when the inputs still hash to what it was made from.
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include "film/model.h"

namespace studio::film {

/// Entry hashes and the style hash of one film, computed once per pass.
class HashCache {
public:
    explicit HashCache(const Film& f) : m_film(f) {}
    QString entry(const QString& id);      // "" for an unknown id
    QString style();                       // all style entries, by id
private:
    const Film& m_film;
    QHash<QString, QString> m_entries;
    QString m_style;
    bool m_styleDone = false;
};

/// What a node's input hash covers, as a JSON object (for tests and diagnostics).
QJsonObject inputsOf(const Film& f, const Node& n, HashCache* cache = nullptr);
/// sha256 hex of json::stable(inputsOf(...)).
QString inputHash(const Film& f, const Node& n, HashCache* cache = nullptr);

/// The effective bible entries a node's inputs read (a shot without its own characters reads its scene's; a line reads
/// its voice, or its speaker's voice), by id.
QStringList entriesUsed(const Film& f, const Node& n);
/// What a node's generation reads that is not a node: "bible:<id>" for entriesUsed(), "style", "aspect", "language"
/// (the field keys of diffFilm() in locks.h).
QStringList keysUsed(const Film& f, const Node& n);

struct StaleReport {
    QStringList changed;                   // inputs changed: new hash stored, now stale
    QStringList lockedDrift;               // locked (or under a locked node), inputs changed: left as they were
};
/// Recomputes every node's input hash and marks the changed ones stale (see above). After loading a film, or when the
/// hash rules change.
StaleReport propagate(Film& f);

/// What an edit changed that input hashes can see. A node's hash depends only on itself, its ancestors (the scene a shot
/// inherits from, its sequence's video model), the bible and the settings; so without a bible or settings change only
/// the subtrees of the changed nodes can change, and a bible change reaches only the nodes that read that entry.
struct Changes {
    QStringList roots;                     // nodes added, or whose kind, data, parent or locked flag changed
    QSet<QString> entries;                 // bible entries added, removed, or changed as generation sees them
    bool style = false, aspect = false, language = false;
    bool empty() const { return roots.isEmpty() && entries.isEmpty() && !style && !aspect && !language; }
};
Changes changesBetween(const Film& before, const Film& after);
/// propagate() limited to the nodes `c` can reach (applyOps uses this: a one-shot edit of a feature film rehashes one
/// node, not ten thousand). The same result as the full pass whenever the stored hashes were current before the edit.
StaleReport propagate(Film& f, const Changes& c);

/// A generation finished for `id` from inputs that hashed to `usedHash`: the node is fresh again only when its inputs
/// still hash to that (an edit during the run keeps it stale) and it is not locked (automation never touches a locked
/// node). Returns whether it is fresh now.
bool markFresh(Film& f, const QString& id, const QString& usedHash);

struct Users {
    QStringList nodes;                     // nodes that name the entry in a field (not the ones inheriting it)
    QStringList entries;                   // bible entries that name it (characters pointing at a voice)
};
/// Who names a bible entry: removing it is refused while anything does.
Users usersOf(const Film& f, const QString& entryId);
/// Nodes whose input hash depends on the entry (direct, inherited from a scene, or through a speaker's voice).
QStringList affectedBy(const Film& f, const QString& entryId);

} // namespace studio::film
