#pragma once
// The film bible and the field schema of the film model (ST-21, docs/plans/starmedia-studio.md §4.6, §4.8).
//
// The bible holds what every shot of a film must agree on: characters (appearance, wardrobe, the voice they speak with),
// locations, props, the style preset (one per film; the ids of StarMedia src/studio/styles.ts) and voices (a TTS model
// and preset voice, or a cloned profile whose consent the safety module re-checks at send time). Each entry may carry
// up to 16 reference images (artifact sha256s; limits.ts: at most 16 refs per request).
//
// Film nodes (model.h) and bible entries share one field schema: every field has a type, limits and two flags:
//   hashed  a generation input: part of the input hash (stale.h), so changing it makes the media made from it stale;
//   output  written by generation only (an anchor picture, a recorded voice), never by an op.
// checkField() validates one value the way applyOps needs it: a reason for every refusal, never a silent clamp.
//
// Pure value types and functions; safe on any thread.
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <functional>
#include <optional>

namespace studio::film {

enum class BibleKind { Character, Location, Prop, Style, Voice };
QString bibleKindName(BibleKind k);                       // "character" ...
std::optional<BibleKind> parseBibleKind(QStringView s);  // any case, plurals too ("characters")
const QList<BibleKind>& allBibleKinds();

// ---------------------------------------------------------------- field schema

enum class FieldType {
    Text,      // a string, cleaned (controls removed); maxLen characters
    Number,    // a finite number in [min, max], rounded to 2 decimals
    Enum,      // one of `values` (any case in, canonical spelling out)
    Ref,       // one bible entry of `refKinds` (by id or name in, id out)
    Refs,      // a list of bible entries of `refKinds` (at most maxLen), duplicates dropped
    Asset,     // an artifact sha256 (64 lower-case hex); output fields only
};

struct FieldSpec {
    QString name;
    FieldType type = FieldType::Text;
    int maxLen = 0;                        // Text: characters; Refs: entries
    double min = 0, max = 0;               // Number
    QStringList values;                    // Enum
    QList<BibleKind> refKinds;             // Ref / Refs
    bool hashed = false;                   // part of the input hash as it is stored
    bool output = false;                   // written by generation only
    QStringList aliases;                   // other names applyOps accepts for it ("description" for a shot's visual)
};

/// Looks a field up by name or alias (keys as normaliseKey() leaves them).
const FieldSpec* findField(const QList<FieldSpec>& specs, QStringView key);
/// The input fields of a schema, comma separated, for messages ("visual, camera, duration ...").
QString fieldList(const QList<FieldSpec>& specs);
/// "afterSceneId" -> "after_scene_id", "On-Screen Text" -> "on_screen_text": how op keys are compared.
QString normaliseKey(QStringView key);
/// Text as it is stored: CR LF -> LF, other controls -> space, trimmed; single-line fields (maxLen <= 300) also fold
/// line breaks and runs of spaces.
QString cleanText(QStringView s, int maxLen);

/// Resolves a bible reference (an id or a name) among entries of `kinds`; sets `error` and returns "" when none fits.
using RefResolver = std::function<QString(const QString& ref, const QList<BibleKind>& kinds, QString* error)>;

struct FieldCheck {
    bool ok = true;
    bool given = true;                     // false: no value (null, "", whitespace): "leave it as it is"
    bool clear = false;                    // "-" (and [] for a list): remove the field
    QJsonValue value;                      // the canonical value when given and not clear
    QString error;                         // why not ok ("duration must be a number from 1 to 20")
};
/// Validates one value for `f`. `what` names the owner in messages ("shot", "character").
FieldCheck checkField(const FieldSpec& f, const QJsonValue& v, const RefResolver& refs, QStringView what);

/// The hashed fields of `data` (those `specs` marks hashed and that are present), as stored.
QJsonObject hashedFields(const QList<FieldSpec>& specs, const QJsonObject& data);
/// `data` without the output fields of `specs`.
QJsonObject withoutOutputs(const QList<FieldSpec>& specs, const QJsonObject& data);

// ---------------------------------------------------------------- bible entries

struct BibleEntry {
    QString id, projectId;
    BibleKind kind = BibleKind::Character;
    QString name;
    QJsonObject data;
    QStringList refs;                      // reference images: artifact sha256s, at most kMaxRefs
    qint64 updatedMs = 0;
};
/// Every column but updated_ms.
bool sameEntry(const BibleEntry& a, const BibleEntry& b);

constexpr int kMaxRefs = 16;               // limits.ts: reference images per request
constexpr int kMaxNameLen = 80;

const QList<FieldSpec>& bibleFields(BibleKind k);
/// The style presets of StarMedia src/studio/styles.ts (STYLES ids, in that order); "custom" is accepted besides them
/// and needs a description.
const QStringList& stylePresets();
/// True for 64 lower-case hex characters.
bool isSha256(QStringView s);
/// Validates and normalises a reference list (lower-cases, drops duplicates); false with a reason when an item is not
/// a sha256 or there are more than kMaxRefs.
bool checkRefs(const QJsonValue& v, QStringList* out, QString* error);

/// The hash of what a generation reads from an entry: kind, name, hashed fields and reference images (notes and a
/// character's voice pointer are not part of it: a line resolves the voice itself, a shot never hears it).
QString entryHash(const BibleEntry& e);

/// JSON of an entry for the API and the agents' film reads.
QJsonObject entryJson(const BibleEntry& e);

} // namespace studio::film
