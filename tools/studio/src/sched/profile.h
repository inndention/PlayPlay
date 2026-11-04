#pragma once
// The concurrency profile (ST-10, docs/plans/starmedia-studio.md §4.3): ONE set of limits for everything in the Studio
// that runs at the same time, a port of starmedia local/concurrency.mjs + src/concurrency.ts.
//
//   queue.heavy / queue.light     runs the RunQueue lets through at once, across all projects (runqueue.h)
//   gates.videos|images|audio|chat   provider POSTs of each kind on the wire at once (provider/gates.h)
//   gates.poll                    new here: the poll gate of gates.h (status polls, downloads); the TS has no such gate
//   steps                         steps of ONE run worked on at once (sched/graph.h setDefaults); per-kind caps follow
//                                 from the gates (kindSteps), so 12 narration lines never crowd out the image requests
//   workers                       the worker pool (TS: PHP workers of StarMedia Local), 1..32
//
// It never touches spend caps, approval gates or budgets: more parallelism only means more of the SAME approved work
// at a time. How a number is chosen, lowest to highest priority (the first VALID one wins, the rest are ignored with a
// warning): 1. the safe default (queue 3/8, gates 3/4/4/8, poll 8, steps 4, workers 6); 2. the hardware tier (mode
// "auto": lean / safe / balanced / fast from cores and free RAM, the smaller of the two decides; an unknown figure never
// raises the tier above safe); 3. `server` (a published profile adopted from another instance); 4. `settings` (the
// persisted setting, config.json "concurrency"); 5. `env` (STARMEDIA_CONCURRENCY=lean|safe|balanced|fast|auto,
// STARMEDIA_QUEUE_HEAVY, STARMEDIA_QUEUE_LIGHT, STARMEDIA_GATE_VIDEOS|IMAGES|AUDIO|CHAT|POLL, STARMEDIA_EXEC_STEPS,
// STARMEDIA_WORKERS: the TS names, so one operator setting drives both). Every number is a whole number clamped into
// its range; text that is not a number is ignored with a warning, never guessed at.
//
// Ranges: the TS FIELDS ranges are a browser's (Ranges::Browser: chat at most 32, images 12 ...). A server has the
// widened ranges of design §4.3 (Ranges::Server, the default here: videos / images / audio / poll 1-64, chat 1-256,
// queue 1-64 / 1-256, steps 1-64; the same maxima as provider::GateLimits). The defaults and the tiers are the TS
// numbers under both, so nothing widens until a profile raises it.
//
// resolveProfile() is pure: the same input gives the same output, and nothing is read from the machine unless the
// hardware is left out. applyProfile() hands the numbers to what enforces them (the graph defaults, the gates, the
// queue); until then everything keeps the safe defaults.
#include "provider/gates.h"
#include "sched/graph.h"

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

namespace studio::sched {

class RunQueue;

enum class Tier { Lean, Safe, Balanced, Fast };
QString tierName(Tier t);                  // "lean" | "safe" | "balanced" | "fast"
bool parseTier(QStringView text, Tier* out);   // case-insensitive
/// "auto" or a tier name, case-insensitive (TS MODES).
bool isMode(QStringView text);
const QStringList& modeNames();            // auto, lean, safe, balanced, fast

enum class Ranges { Browser, Server };

/// One field of the profile (TS FIELDS): its key, environment variable, range and words for a settings form.
struct Field {
    QString key;                           // "queue.heavy", "gates.chat", "steps" ...
    QString env;
    int min = 1, max = 1;
    QString label, hint;
};
const QList<Field>& fields(Ranges r = Ranges::Server);
const Field* field(QStringView key, Ranges r = Ranges::Server);
/// The safe defaults per key (TS BASE).
const QHash<QString, int>& baseValues();
/// Which gate each stage kind goes through (TS KIND_GATE): image → images, video → videos, tts / stt → audio, text /
/// json / vision / embed / rerank → chat. A kind not listed (local renders, packaging) has no gate.
const QHash<QString, QString>& kindGate();

struct TierSpec {
    double mult = 1;
    int workers = 6;
    int minCores = 0;
    qint64 minFreeRamMb = 0;
    QString summary;
};
const TierSpec& tierSpec(Tier t);

/// Cores and free memory of this machine; a figure that could not be read is left out (nullopt).
struct Hardware {
    std::optional<int> cores;
    std::optional<qint64> freeRamMb;
    std::optional<qint64> totalRamMb;
    QString source = QStringLiteral("unknown");   // "os" | "given" | "unknown" | what the publisher said
    QJsonObject toJson() const;            // {cores, freeRamMb, totalRamMb (null when unknown), source}
    static Hardware fromJson(const QJsonValue& v);
};
/// Reads the machine: QThread::idealThreadCount() and the OS memory figures (Windows GlobalMemoryStatusEx).
Hardware detectHardware();
/// The tier a machine gets: the best one whose core AND memory needs are both met. Nothing known = safe; one figure
/// unknown never raises the tier above safe (TS tierFor).
Tier tierFor(const Hardware& hw);
/// The numbers of a tier before any override (TS tierValues), keyed like fields().
QHash<QString, int> tierValues(Tier t, const Hardware& hw, Ranges r = Ranges::Server);

/// TS readInt: a whole-number reading of a JSON value (a finite number, floored, or a string holding one); nullopt
/// for anything else (booleans, null, objects, text that is not a number, Infinity).
std::optional<qint64> readInt(const QJsonValue& v);
std::optional<qint64> readInt(QStringView text);
inline std::optional<qint64> readInt(const QString& text) { return readInt(QStringView(text)); }
/// JS String(v) for a number: "7", "2.9", "1e+21" (money-free; used in warnings).
QString numberText(double d);

/// What cleanSettings / cleanEnv keep of a layer: the valid mode (lower case, empty = none), the valid numbers
/// (clamped), and a warning for each thing that was not.
struct Cleaned {
    QString mode;
    QHash<QString, int> values;
    QStringList warnings;
};
/// A partial { mode, queue: {heavy, light}, gates: {videos, images, audio, chat, poll}, steps, workers }; anything
/// that is not an object is ignored with a warning (null / undefined: nothing, quietly).
Cleaned cleanSettings(const QJsonValue& raw, const QString& where = QStringLiteral("settings"),
                      Ranges r = Ranges::Server);
/// The same for environment variables (name → value; an unset or empty variable is skipped). Warnings name the
/// variable.
Cleaned cleanEnv(const QHash<QString, QString>& env, Ranges r = Ranges::Server);
/// The STARMEDIA_* variables of this process (the names in fields() plus STARMEDIA_CONCURRENCY), for ProfileInput::env.
QHash<QString, QString> processEnv();

struct ProfileInput {
    std::optional<Hardware> hardware;      // nullopt: detectHardware()
    QString mode;                          // an explicit mode (lowest priority of the four mode sources)
    QJsonValue settings;                   // the persisted setting
    QJsonValue server;                     // what another instance published (adoptPublished)
    QHash<QString, QString> env;           // environment variables (processEnv(), or a test's)
    int heavyCap = 0;                      // the shared generation cap of the server behind the proxy; 0 = none
    Ranges ranges = Ranges::Server;
};

struct Profile {
    QString mode = QStringLiteral("auto"); // what was asked for
    Tier tier = Tier::Safe;                // what the numbers are based on
    Hardware hardware;
    struct { int heavy = 3, light = 8; } queue;
    struct { int videos = 3, images = 4, audio = 4, chat = 8, poll = 8; } gates;
    int steps = 4;
    QHash<QString, int> kindSteps;         // per stage kind: steps, and no more than the kind's gate (video: steps)
    int workers = 6;
    int heavyCap = 0;
    QHash<QString, QString> sources;       // key → "default" | "hardware" | "mode" | "server" | "setting" | "env"
    QStringList warnings;
    Ranges ranges = Ranges::Server;

    /// The value of one key ("gates.chat"); 0 for an unknown key.
    int value(QStringView key) const;
    /// The gate widths and the heavy cap for provider::Gates::configure(); perProvider is taken from `keep` (the
    /// profile does not decide sub-gates). A heavyCap of 0 keeps the one in `keep` (TS configureOpenRouter).
    provider::GateLimits gateLimits(const provider::GateLimits& keep = {}) const;
    /// steps + kindSteps as the graph's per-run limits (sched::setDefaults).
    GraphLimits graphLimits() const;
    /// {mode, tier, hardware, queue, gates, steps, kindSteps, workers, heavyCap, sources, warnings, line}
    QJsonObject toJson() const;
};

/// Works the profile out (TS resolveProfile). Pure when `hardware` is given.
Profile resolveProfile(const ProfileInput& input = {});
/// One line for logs and the board: "balanced (auto): queue 4 heavy / 12 light, gates 4 video / 6 image / 6 audio /
/// 12 text, 6 steps per run, 8 workers".
QString describeProfile(const Profile& p);
/// What an instance publishes about its profile for another to adopt (TS publishProfile): the machine it measured,
/// the mode, and ONLY the numbers somebody chose (env, setting, server), never derived ones, so a mode picked on the
/// adopting side still takes effect. {mode, tier, hardware, workers, heavyCap, overrides, line, warnings}
QJsonObject publishProfile(const Profile& p);
/// The other side: the hardware and the `server` layer (overrides + a non-auto mode) for resolveProfile().
ProfileInput adoptPublished(const QJsonValue& published);

/// The worker count (TS workersFor): `configured` is the config value, a number kept exactly (1..32) or "auto" (the
/// profile's number); STARMEDIA_WORKERS in the input's env wins over both; nothing configured = the safe 6.
struct WorkersResult {
    int workers = 6;
    QString source;                        // "env" | "config" | "default" | the profile's source for auto
    QStringList warnings;
    Profile profile;
};
WorkersResult workersFor(const QJsonValue& configured, const ProfileInput& input = {});

// ---------------- the profile in force ----------------

struct ApplyTargets {
    provider::Gates* gates = nullptr;
    RunQueue* queue = nullptr;
};
/// Puts a profile in force (TS applyProfile): sched::setDefaults(steps, kindSteps), gates->configure(), the queue's
/// pools, then the listeners. Runs already going keep what they started with. Call it on the gates' / queue's thread.
Profile applyProfile(const Profile& p, const ApplyTargets& t = {});
/// The profile in force right now (the safe defaults until applyProfile() ran).
Profile currentProfile();
/// Back to the safe defaults (tests).
Profile resetProfile(const ApplyTargets& t = {});
using ProfileListenerId = quint64;
/// Told after every applyProfile(); a listener that throws does not stop the others.
ProfileListenerId onProfileChange(std::function<void(const Profile&)> fn);
void removeProfileListener(ProfileListenerId id);

} // namespace studio::sched
