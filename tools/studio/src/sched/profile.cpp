// The concurrency profile (ST-10). See profile.h; the rules and the warning texts are those of starmedia
// local/concurrency.mjs, so an operator reading a Studio log sees what a StarMedia log would have said.
#include "sched/profile.h"

#include <QJsonArray>
#include <QProcessEnvironment>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <exception>
#include <map>
#include <mutex>

#include "core/json.h"
#include "core/log.h"
#include "sched/runqueue.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace studio::sched {

namespace {

constexpr QStringView kCat = u"profile";
constexpr QLatin1StringView kModeEnv("STARMEDIA_CONCURRENCY");

const QStringList& tierNames() {
    static const QStringList names{QStringLiteral("lean"), QStringLiteral("safe"), QStringLiteral("balanced"),
                                   QStringLiteral("fast")};
    return names;
}

QList<Field> makeFields(Ranges r) {
    const bool srv = r == Ranges::Server;
    // the TS FIELDS table (browser ranges) with the server ranges of design §4.3 beside it; the maxima equal
    // provider::GateLimits::maxWidth so the gates never clamp what the profile let through
    auto f = [&](const char* key, const char* env, int min, int browserMax, int serverMax, const char* label,
                 const char* hint) {
        return Field{QString::fromLatin1(key), QString::fromLatin1(env), min, srv ? serverMax : browserMax,
                     QString::fromLatin1(label), QString::fromLatin1(hint)};
    };
    return {
        f("queue.heavy", "STARMEDIA_QUEUE_HEAVY", 1, 12, 64, "Heavy runs at once",
          "Video, many images and renders, across all open projects."),
        f("queue.light", "STARMEDIA_QUEUE_LIGHT", 1, 32, 256, "Light runs at once",
          "Text phases and quiet agent decisions, across all open projects."),
        f("gates.videos", "STARMEDIA_GATE_VIDEOS", 1, 8, 64, "Video requests at once",
          "Video submissions on the wire to the provider."),
        f("gates.images", "STARMEDIA_GATE_IMAGES", 1, 12, 64, "Image requests at once",
          "Image requests on the wire to the provider."),
        f("gates.audio", "STARMEDIA_GATE_AUDIO", 1, 12, 64, "Audio requests at once",
          "Narration and transcription requests."),
        f("gates.chat", "STARMEDIA_GATE_CHAT", 1, 32, 256, "Text requests at once",
          "Model calls for writing, deciding and describing."),
        f("gates.poll", "STARMEDIA_GATE_POLL", 1, 32, 64, "Status polls at once",
          "Video status polls and downloads on the wire."),
        f("steps", "STARMEDIA_EXEC_STEPS", 1, 16, 64, "Steps of one run at once",
          "Scenes, narration lines and clips of one plan worked on together."),
        f("workers", "STARMEDIA_WORKERS", 1, 32, 32, "Workers", "The worker pool; takes effect at the next start."),
    };
}

int clampInt(qint64 n, int lo, int hi) { return int(std::min<qint64>(hi, std::max<qint64>(lo, n))); }

QString modeList() { return modeNames().join(QStringLiteral(", ")); }

/// JSON.stringify(v) for a warning (never a secret: these are numbers and short words).
QString stringify(const QJsonValue& v) { return QString::fromUtf8(json::compact(v)); }

std::optional<qint64> floorToInt64(double d) {
    if (!std::isfinite(d)) return std::nullopt;
    const double f = std::floor(d);
    if (f >= 9.2e18) return std::numeric_limits<qint64>::max();
    if (f <= -9.2e18) return std::numeric_limits<qint64>::min();
    return qint64(f);
}

struct ProfileGlobal {
    std::mutex mutex;
    bool init = false;
    Profile current;
    std::map<ProfileListenerId, std::function<void(const Profile&)>> listeners;
    ProfileListenerId next = 0;
};
ProfileGlobal& global() {
    static ProfileGlobal* g = new ProfileGlobal;   // leaked on purpose, like the graph's counters
    return *g;
}

Profile safeProfile() {
    ProfileInput in;
    in.hardware = Hardware{};              // nothing known → the safe tier, every number "default"
    return resolveProfile(in);
}

} // namespace

// ---------------- names and tables ----------------

QString tierName(Tier t) { return tierNames().at(int(t)); }

bool parseTier(QStringView text, Tier* out) {
    const QString s = text.trimmed().toString().toLower();
    const int i = int(tierNames().indexOf(s));
    if (i < 0) return false;
    if (out) *out = Tier(i);
    return true;
}

const QStringList& modeNames() {
    static const QStringList names = QStringList{QStringLiteral("auto")} + tierNames();
    return names;
}

bool isMode(QStringView text) { return modeNames().contains(text.trimmed().toString().toLower()); }

const QList<Field>& fields(Ranges r) {
    static const QList<Field> browser = makeFields(Ranges::Browser);
    static const QList<Field> server = makeFields(Ranges::Server);
    return r == Ranges::Server ? server : browser;
}

const Field* field(QStringView key, Ranges r) {
    for (const Field& f : fields(r))
        if (f.key == key) return &f;
    return nullptr;
}

const QHash<QString, int>& baseValues() {
    static const QHash<QString, int> base{
        {QStringLiteral("queue.heavy"), 3},  {QStringLiteral("queue.light"), 8},  {QStringLiteral("gates.videos"), 3},
        {QStringLiteral("gates.images"), 4}, {QStringLiteral("gates.audio"), 4},  {QStringLiteral("gates.chat"), 8},
        {QStringLiteral("gates.poll"), 8},   {QStringLiteral("steps"), 4},        {QStringLiteral("workers"), 6},
    };
    return base;
}

const QHash<QString, QString>& kindGate() {
    static const QHash<QString, QString> kg{
        {QStringLiteral("image"), QStringLiteral("images")}, {QStringLiteral("video"), QStringLiteral("videos")},
        {QStringLiteral("tts"), QStringLiteral("audio")},    {QStringLiteral("stt"), QStringLiteral("audio")},
        {QStringLiteral("text"), QStringLiteral("chat")},    {QStringLiteral("json"), QStringLiteral("chat")},
        {QStringLiteral("vision"), QStringLiteral("chat")},  {QStringLiteral("embed"), QStringLiteral("chat")},
        {QStringLiteral("rerank"), QStringLiteral("chat")},
    };
    return kg;
}

const TierSpec& tierSpec(Tier t) {
    static const TierSpec specs[] = {
        {0.5, 3, 0, 0, QStringLiteral("a small or busy machine: fewer things at once")},
        {1, 6, 4, 2560, QStringLiteral("the studio defaults")},
        {1.5, 8, 6, 6144, QStringLiteral("enough cores and free memory for about 1.5 times as much at once")},
        {2, 12, 12, 12288, QStringLiteral("plenty of cores and free memory: about twice as much at once")},
    };
    return specs[int(t)];
}

// ---------------- the machine ----------------

QJsonObject Hardware::toJson() const {
    QJsonObject o;
    o.insert(QStringLiteral("cores"), cores ? QJsonValue(*cores) : QJsonValue());
    o.insert(QStringLiteral("freeRamMb"), freeRamMb ? QJsonValue(double(*freeRamMb)) : QJsonValue());
    o.insert(QStringLiteral("totalRamMb"), totalRamMb ? QJsonValue(double(*totalRamMb)) : QJsonValue());
    o.insert(QStringLiteral("source"), source);
    return o;
}

Hardware Hardware::fromJson(const QJsonValue& v) {
    Hardware hw;
    hw.source = QStringLiteral("given");
    if (!v.isObject()) return hw;
    const QJsonObject o = v.toObject();
    auto num = [&](const char* key) -> std::optional<qint64> {
        const QJsonValue x = o.value(QLatin1StringView(key));
        if (!x.isDouble()) return std::nullopt;
        const auto n = floorToInt64(x.toDouble());
        if (!n || *n <= 0) return std::nullopt;
        return n;
    };
    if (const auto c = num("cores")) hw.cores = int(std::min<qint64>(*c, 1 << 20));
    hw.freeRamMb = num("freeRamMb");
    hw.totalRamMb = num("totalRamMb");
    if (o.value(QStringLiteral("source")).isString()) hw.source = o.value(QStringLiteral("source")).toString();
    return hw;
}

Hardware detectHardware() {
    Hardware hw;
    hw.source = QStringLiteral("os");
    const int cores = QThread::idealThreadCount();
    if (cores > 0) hw.cores = cores;
#ifdef Q_OS_WIN
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        hw.freeRamMb = qint64(ms.ullAvailPhys / 1048576ULL);
        hw.totalRamMb = qint64(ms.ullTotalPhys / 1048576ULL);
    }
#endif
    return hw;
}

Tier tierFor(const Hardware& hw) {
    const bool cores = hw.cores && *hw.cores > 0;
    const bool ram = hw.freeRamMb && *hw.freeRamMb > 0;
    if (!cores && !ram) return Tier::Safe;
    Tier best = Tier::Lean;
    for (Tier t : {Tier::Lean, Tier::Safe, Tier::Balanced, Tier::Fast}) {
        const TierSpec& s = tierSpec(t);
        // a figure we do not have is treated as meeting 'safe' but nothing above it, so an unknown never raises the limits
        const bool okCores = cores ? *hw.cores >= s.minCores : s.mult <= 1;
        const bool okRam = ram ? *hw.freeRamMb >= s.minFreeRamMb : s.mult <= 1;
        if (okCores && okRam) best = t;
    }
    return best;
}

QHash<QString, int> tierValues(Tier t, const Hardware& hw, Ranges r) {
    const TierSpec& s = tierSpec(t);
    QHash<QString, int> v;
    for (const Field& f : fields(r)) {
        if (f.key == u"workers") {
            // never more workers than cores (when known), never fewer than 3 (the screens must stay answerable)
            const int w = hw.cores && *hw.cores > 0 ? std::min(s.workers, std::max(3, *hw.cores)) : s.workers;
            v.insert(f.key, clampInt(w, f.min, f.max));
        } else {
            const qint64 n = std::max<qint64>(1, qint64(std::floor(baseValues().value(f.key) * s.mult)));
            v.insert(f.key, clampInt(n, f.min, f.max));
        }
    }
    return v;
}

// ---------------- reading numbers ----------------

std::optional<qint64> readInt(QStringView text) {
    const QString s = text.trimmed().toString();
    if (s.isEmpty()) return std::nullopt;
    bool ok = false;
    const double d = s.toDouble(&ok);      // "7", "2.9", "1e3", "3.7e0" like JS Number(); "0x10" is not read (JS would)
    if (!ok) return std::nullopt;
    return floorToInt64(d);
}

std::optional<qint64> readInt(const QJsonValue& v) {
    if (v.isDouble()) return floorToInt64(v.toDouble());
    if (v.isString()) return readInt(v.toString());
    return std::nullopt;
}

QString numberText(double d) { return json::jsNumber(d); }

// ---------------- the layers ----------------

Cleaned cleanSettings(const QJsonValue& raw, const QString& where, Ranges r) {
    Cleaned c;
    if (raw.isUndefined() || raw.isNull()) return c;
    if (!raw.isObject()) {
        c.warnings << QStringLiteral("%1: expected an object, ignored").arg(where);
        return c;
    }
    const QJsonObject o = raw.toObject();
    const QJsonValue mode = o.value(QStringLiteral("mode"));
    if (!mode.isUndefined() && !mode.isNull() && !(mode.isString() && mode.toString().isEmpty())) {
        if (mode.isString() && isMode(mode.toString())) c.mode = mode.toString().toLower();
        else c.warnings << QStringLiteral("%1.mode: %2 is not one of %3, ignored").arg(where, stringify(mode), modeList());
    }
    for (const Field& f : fields(r)) {
        const QJsonValue v = json::at(o, f.key);
        if (v.isUndefined() || v.isNull() || (v.isString() && v.toString().isEmpty())) continue;
        const auto n = readInt(v);
        if (!n) {
            c.warnings << QStringLiteral("%1.%2: %3 is not a number, ignored").arg(where, f.key, stringify(v));
            continue;
        }
        const int cl = clampInt(*n, f.min, f.max);
        if (cl != *n)
            c.warnings << QStringLiteral("%1.%2: %3 is outside %4..%5, using %6")
                              .arg(where, f.key, QString::number(*n), QString::number(f.min), QString::number(f.max),
                                   QString::number(cl));
        c.values.insert(f.key, cl);
    }
    return c;
}

Cleaned cleanEnv(const QHash<QString, QString>& env, Ranges r) {
    Cleaned c;
    const QString mode = env.value(kModeEnv).trimmed();
    if (!mode.isEmpty()) {
        if (isMode(mode)) c.mode = mode.toLower();
        else
            c.warnings << QStringLiteral("%1: %2 is not one of %3, ignored")
                              .arg(kModeEnv, QString::fromUtf8(json::jsString(mode)), modeList());
    }
    for (const Field& f : fields(r)) {
        const auto it = env.constFind(f.env);
        if (it == env.constEnd() || it->isEmpty()) continue;
        const auto n = readInt(*it);
        if (!n) {
            c.warnings << QStringLiteral("%1: %2 is not a number, ignored").arg(f.env, QString::fromUtf8(json::jsString(*it)));
            continue;
        }
        const int cl = clampInt(*n, f.min, f.max);
        if (cl != *n)
            c.warnings << QStringLiteral("%1: %2 is outside %3..%4, using %5")
                              .arg(f.env, QString::number(*n), QString::number(f.min), QString::number(f.max),
                                   QString::number(cl));
        c.values.insert(f.key, cl);
    }
    return c;
}

QHash<QString, QString> processEnv() {
    const QProcessEnvironment pe = QProcessEnvironment::systemEnvironment();
    QHash<QString, QString> env;
    QStringList names{QString(kModeEnv)};
    for (const Field& f : fields(Ranges::Server)) names << f.env;
    for (const QString& n : names)
        if (pe.contains(n)) env.insert(n, pe.value(n));
    return env;
}

// ---------------- the profile ----------------

int Profile::value(QStringView key) const {
    if (key == u"queue.heavy") return queue.heavy;
    if (key == u"queue.light") return queue.light;
    if (key == u"gates.videos") return gates.videos;
    if (key == u"gates.images") return gates.images;
    if (key == u"gates.audio") return gates.audio;
    if (key == u"gates.chat") return gates.chat;
    if (key == u"gates.poll") return gates.poll;
    if (key == u"steps") return steps;
    if (key == u"workers") return workers;
    return 0;
}

provider::GateLimits Profile::gateLimits(const provider::GateLimits& keep) const {
    provider::GateLimits l = keep;
    l[provider::GateKind::Videos] = gates.videos;
    l[provider::GateKind::Images] = gates.images;
    l[provider::GateKind::Audio] = gates.audio;
    l[provider::GateKind::Chat] = gates.chat;
    l[provider::GateKind::Poll] = gates.poll;
    if (heavyCap > 0) l.heavyCap = heavyCap;
    return l;
}

GraphLimits Profile::graphLimits() const {
    GraphLimits l;
    l.steps = steps;
    l.perKind = kindSteps;
    return l;
}

QJsonObject Profile::toJson() const {
    QJsonObject o;
    o.insert(QStringLiteral("mode"), mode);
    o.insert(QStringLiteral("tier"), tierName(tier));
    o.insert(QStringLiteral("hardware"), hardware.toJson());
    o.insert(QStringLiteral("queue"), QJsonObject{{QStringLiteral("heavy"), queue.heavy}, {QStringLiteral("light"), queue.light}});
    o.insert(QStringLiteral("gates"), QJsonObject{{QStringLiteral("videos"), gates.videos},
                                                  {QStringLiteral("images"), gates.images},
                                                  {QStringLiteral("audio"), gates.audio},
                                                  {QStringLiteral("chat"), gates.chat},
                                                  {QStringLiteral("poll"), gates.poll}});
    o.insert(QStringLiteral("steps"), steps);
    QJsonObject ks;
    for (auto it = kindSteps.cbegin(); it != kindSteps.cend(); ++it) ks.insert(it.key(), it.value());
    o.insert(QStringLiteral("kindSteps"), ks);
    o.insert(QStringLiteral("workers"), workers);
    o.insert(QStringLiteral("heavyCap"), heavyCap);
    QJsonObject src;
    for (auto it = sources.cbegin(); it != sources.cend(); ++it) src.insert(it.key(), it.value());
    o.insert(QStringLiteral("sources"), src);
    o.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(warnings));
    o.insert(QStringLiteral("ranges"), ranges == Ranges::Server ? QStringLiteral("server") : QStringLiteral("browser"));
    o.insert(QStringLiteral("line"), describeProfile(*this));
    return o;
}

Profile resolveProfile(const ProfileInput& input) {
    Profile p;
    p.ranges = input.ranges;
    if (input.hardware) {
        p.hardware = *input.hardware;
        if (p.hardware.source.isEmpty()) p.hardware.source = QStringLiteral("given");
    } else {
        p.hardware = detectHardware();
    }
    const Cleaned server = cleanSettings(input.server, QStringLiteral("server"), input.ranges);
    const Cleaned settings = cleanSettings(input.settings, QStringLiteral("settings"), input.ranges);
    const Cleaned env = cleanEnv(input.env, input.ranges);
    p.warnings << server.warnings << settings.warnings << env.warnings;

    // the mode: env > setting > what the server published > the explicit argument > auto
    QString asked = QStringLiteral("auto");
    for (const QString& m : {input.mode, server.mode, settings.mode, env.mode}) {
        if (m.isEmpty()) continue;
        if (isMode(m)) asked = m.trimmed().toLower();
        else p.warnings << QStringLiteral("mode %1 ignored (use one of %2)").arg(QString::fromUtf8(json::jsString(m)), modeList());
    }
    p.mode = asked;
    Tier tier = Tier::Safe;
    if (asked == u"auto") tier = tierFor(p.hardware);
    else parseTier(asked, &tier);
    p.tier = tier;
    const QHash<QString, int> base = tierValues(tier, p.hardware, input.ranges);
    const QString tierSource = asked == u"auto" ? QStringLiteral("hardware") : QStringLiteral("mode");
    QHash<QString, int> v;
    for (const Field& f : fields(input.ranges)) {
        int val = base.value(f.key);
        QString src = val == baseValues().value(f.key) ? QStringLiteral("default") : tierSource;
        const std::pair<const Cleaned*, const char*> layers[] = {{&server, "server"}, {&settings, "setting"}, {&env, "env"}};
        for (const auto& [layer, name] : layers) {
            const auto it = layer->values.constFind(f.key);
            if (it != layer->values.constEnd()) {
                val = *it;
                src = QString::fromLatin1(name);
            }
        }
        v.insert(f.key, val);
        p.sources.insert(f.key, src);
    }

    // coherence: a gate wider than the server's shared cap only queues behind the cap; say so, and trim what was not
    // set by hand
    p.heavyCap = input.heavyCap >= 1 ? input.heavyCap : 0;
    if (p.heavyCap) {
        for (const char* g : {"videos", "images", "audio"}) {
            const QString k = QStringLiteral("gates.") + QLatin1StringView(g);
            if (v.value(k) > p.heavyCap) {
                const QString& src = p.sources[k];
                if (src == u"default" || src == u"hardware" || src == u"mode") v[k] = p.heavyCap;
                else
                    p.warnings << QStringLiteral("%1 is %2 but the server runs at most %3 generation calls at once; the "
                                                 "extra ones would only wait")
                                      .arg(k, QString::number(v.value(k)), QString::number(p.heavyCap));
            }
        }
    }
    p.queue.heavy = v.value(QStringLiteral("queue.heavy"));
    p.queue.light = v.value(QStringLiteral("queue.light"));
    p.gates.videos = v.value(QStringLiteral("gates.videos"));
    p.gates.images = v.value(QStringLiteral("gates.images"));
    p.gates.audio = v.value(QStringLiteral("gates.audio"));
    p.gates.chat = v.value(QStringLiteral("gates.chat"));
    p.gates.poll = v.value(QStringLiteral("gates.poll"));
    p.steps = v.value(QStringLiteral("steps"));
    p.workers = v.value(QStringLiteral("workers"));
    // a step that is one request on the wire (a picture, a narration line, a model call) is no wider than its gate;
    // a VIDEO step spends nearly all its life polling for the finished clip, not on the wire (the gate only spaces
    // the submissions), so it is not narrowed to the gate's width. Kinds without a gate (local renders) have no cap:
    // their own lanes keep a render, or a local model, to one at a time.
    for (auto it = kindGate().cbegin(); it != kindGate().cend(); ++it) {
        const int gate = v.value(QStringLiteral("gates.") + it.value());
        p.kindSteps.insert(it.key(), it.key() == u"video" ? p.steps : std::max(1, std::min(p.steps, gate)));
    }
    return p;
}

QString describeProfile(const Profile& p) {
    return QStringLiteral("%1%2: queue %3 heavy / %4 light, gates %5 video / %6 image / %7 audio / %8 text, %9 steps per "
                          "run, %10 workers")
        .arg(tierName(p.tier), p.mode == u"auto" ? QStringLiteral(" (auto)") : QString(), QString::number(p.queue.heavy),
             QString::number(p.queue.light), QString::number(p.gates.videos), QString::number(p.gates.images),
             QString::number(p.gates.audio), QString::number(p.gates.chat), QString::number(p.steps))
        .arg(p.workers);
}

QJsonObject publishProfile(const Profile& p) {
    QJsonObject overrides;
    for (const Field& f : fields(p.ranges)) {
        if (f.key == u"workers") continue;
        const QString src = p.sources.value(f.key);
        if (src != u"env" && src != u"setting" && src != u"server") continue;
        json::setAt(overrides, f.key, p.value(f.key));
    }
    QJsonObject o;
    o.insert(QStringLiteral("mode"), p.mode);
    o.insert(QStringLiteral("tier"), tierName(p.tier));
    o.insert(QStringLiteral("hardware"), p.hardware.toJson());
    o.insert(QStringLiteral("workers"), p.workers);
    o.insert(QStringLiteral("heavyCap"), p.heavyCap);
    o.insert(QStringLiteral("overrides"), overrides);
    o.insert(QStringLiteral("line"), describeProfile(p));
    o.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(p.warnings));
    return o;
}

ProfileInput adoptPublished(const QJsonValue& published) {
    ProfileInput in;
    if (!published.isObject()) return in;
    const QJsonObject pub = published.toObject();
    if (pub.value(QStringLiteral("hardware")).isObject()) in.hardware = Hardware::fromJson(pub.value(QStringLiteral("hardware")));
    QJsonObject server;
    if (pub.value(QStringLiteral("overrides")).isObject()) server = pub.value(QStringLiteral("overrides")).toObject();
    const QJsonValue mode = pub.value(QStringLiteral("mode"));
    if (mode.isString() && !mode.toString().isEmpty() && mode.toString() != u"auto") server.insert(QStringLiteral("mode"), mode);
    in.server = server;
    return in;
}

WorkersResult workersFor(const QJsonValue& configured, const ProfileInput& input) {
    WorkersResult r;
    r.profile = resolveProfile(input);
    r.warnings = r.profile.warnings;
    const bool envW = r.profile.sources.value(QStringLiteral("workers")) == u"env";
    const bool isAuto = configured.isString() && configured.toString().trimmed().toLower() == u"auto";
    if (envW) {
        r.workers = r.profile.workers;
        r.source = QStringLiteral("env");
        return r;
    }
    if (isAuto) {
        r.workers = r.profile.workers;
        r.source = r.profile.sources.value(QStringLiteral("workers"));
        return r;
    }
    const int base = baseValues().value(QStringLiteral("workers"));
    if (configured.isUndefined() || configured.isNull()) {
        r.workers = base;
        r.source = QStringLiteral("default");
        return r;
    }
    const auto n = readInt(configured);
    if (!n || *n < 1 || *n > 32) {
        r.warnings << QStringLiteral("workers %1 is not 'auto' or a whole number 1..32; using %2").arg(stringify(configured)).arg(base);
        r.workers = base;
        r.source = QStringLiteral("default");
        return r;
    }
    r.workers = int(*n);
    r.source = QStringLiteral("config");
    return r;
}

// ---------------- the profile in force ----------------

Profile applyProfile(const Profile& p, const ApplyTargets& t) {
    std::map<ProfileListenerId, std::function<void(const Profile&)>> listeners;
    {
        ProfileGlobal& g = global();
        std::lock_guard lock(g.mutex);
        g.current = p;
        g.init = true;
        listeners = g.listeners;
    }
    setDefaults(p.graphLimits());
    if (t.gates) {
        const QStringList w = t.gates->configure(p.gateLimits(t.gates->limits()));
        for (const QString& s : w) log::warn(kCat, s);
    }
    if (t.queue) t.queue->setLimits(p.queue.heavy, p.queue.light);
    log::info(kCat, QStringLiteral("profile applied: ") + describeProfile(p));
    for (auto& [id, fn] : listeners) {
        try {
            fn(p);
        } catch (const std::exception& e) {   // a broken listener must not stop the others
            log::warn(kCat, QStringLiteral("profile listener threw: %1").arg(QString::fromUtf8(e.what())));
        } catch (...) {
            log::warn(kCat, QStringLiteral("profile listener threw"));
        }
    }
    return p;
}

Profile currentProfile() {
    ProfileGlobal& g = global();
    std::lock_guard lock(g.mutex);
    if (!g.init) {
        g.current = safeProfile();
        g.init = true;
    }
    return g.current;
}

Profile resetProfile(const ApplyTargets& t) { return applyProfile(safeProfile(), t); }

ProfileListenerId onProfileChange(std::function<void(const Profile&)> fn) {
    if (!fn) return 0;
    ProfileGlobal& g = global();
    std::lock_guard lock(g.mutex);
    const ProfileListenerId id = ++g.next;
    g.listeners.emplace(id, std::move(fn));
    return id;
}

void removeProfileListener(ProfileListenerId id) {
    ProfileGlobal& g = global();
    std::lock_guard lock(g.mutex);
    g.listeners.erase(id);
}

} // namespace studio::sched
