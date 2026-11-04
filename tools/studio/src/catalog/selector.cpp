// Role-based model selection (ST-08): starmedia src/selector.ts. See selector.h.
#include "catalog/selector.h"
#include "provider/limits.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace studio::catalog {

namespace {

struct RoleSpec {
    Role role;
    const char* name;
    Kind kind;
    double maxPriceRank;
};

const QList<RoleSpec>& roleSpecs() {
    static const QList<RoleSpec> s = {
        {Role::Writer, "writer", Kind::Text, 1},        {Role::Editor, "editor", Kind::Text, 0.9},      {Role::Judge, "judge", Kind::Text, 1},
        {Role::Reasoner, "reasoner", Kind::Text, 1},    {Role::Research, "research", Kind::Text, 0.8},  {Role::Fast, "fast", Kind::Text, 0.55},
        {Role::Vision, "vision", Kind::Text, 0.8},      {Role::Image, "image", Kind::Image, 1},         {Role::Video, "video", Kind::Video, 1},
        {Role::ImageEdit, "imageEdit", Kind::Image, 1}, {Role::VideoI2V, "videoI2V", Kind::Video, 1},   {Role::VideoEdit, "videoEdit", Kind::Video, 1},
        {Role::Tts, "tts", Kind::Speech, 1},            {Role::TtsClone, "ttsClone", Kind::Speech, 1},  {Role::Stt, "stt", Kind::Stt, 1},
        {Role::Embed, "embed", Kind::Embed, 0.8},       {Role::Rerank, "rerank", Kind::Rerank, 1}};
    return s;
}

const RoleSpec& spec(Role r) {
    for (const RoleSpec& s : roleSpecs())
        if (s.role == r) return s;
    return roleSpecs().first();
}

bool structured(const ModelInfo& m) { return m.hasParam(u"structured_outputs") || m.hasParam(u"response_format"); }

double nowOf(const PickCtx& ctx) { return ctx.nowSec > 0 ? ctx.nowSec : double(Clock::system().nowMs()) / 1000; }

bool expiring(const ModelInfo& m, double now) { return m.expires && *m.expires - now < 7 * 86400; }

bool excluded(const RunOptions& opts, const ModelInfo& m) {
    for (const QString& e : opts.exclude)
        if (m.id.contains(e)) return true;
    return false;
}

// ---- Fast ↔ Detailed words ----
const QSet<QString>& fastWords() {
    static const QSet<QString> s = {QStringLiteral("turbo"), QStringLiteral("fast"),    QStringLiteral("flash"),     QStringLiteral("lite"), QStringLiteral("mini"),
                                    QStringLiteral("instant"), QStringLiteral("schnell"), QStringLiteral("lightning"), QStringLiteral("nano")};
    return s;
}
const QSet<QString>& nameFastWords() {                  // in display names "nano" is a brand ("Nano Banana"), not a size
    static const QSet<QString> s = [] { QSet<QString> w = fastWords(); w.remove(QStringLiteral("nano")); return w; }();
    return s;
}
const QSet<QString>& detailedWords() {
    static const QSet<QString> s = {QStringLiteral("pro"), QStringLiteral("hd"),      QStringLiteral("ultra"), QStringLiteral("max"),
                                    QStringLiteral("plus"), QStringLiteral("quality"), QStringLiteral("large")};
    return s;
}

/// nullopt = no hint word at all; otherwise -1 / 1, or 0 when both kinds are present (e.g. "large-v3-turbo").
std::optional<int> leanOfWords(const QString& text, const QSet<QString>& fast) {
    static const QRegularExpression sep(QStringLiteral("[^a-z]+"));
    bool f = false, d = false;
    for (const QString& w : text.toLower().split(sep)) {
        if (fast.contains(w)) f = true;
        if (detailedWords().contains(w)) d = true;
    }
    if (!f && !d) return std::nullopt;
    return f == d ? 0 : f ? -1 : 1;
}

struct Scored {
    const ModelInfo* m;
    CostEstimate est;
    double key = 0;
    double seconds() const { return est.seconds.value_or(0); }
};

} // namespace

// ---------------- names ----------------

QString roleName(Role r) { return QString::fromLatin1(spec(r).name); }

std::optional<Role> roleFromName(QStringView name) {
    for (const RoleSpec& s : roleSpecs())
        if (name == QLatin1String(s.name)) return s.role;
    return std::nullopt;
}

const QList<Role>& allRoles() {
    static const QList<Role> r = [] { QList<Role> l; for (const RoleSpec& s : roleSpecs()) l << s.role; return l; }();
    return r;
}

Kind roleKind(Role r) { return spec(r).kind; }
double roleMaxPriceRank(Role r) { return spec(r).maxPriceRank; }

QString mediumName(Medium m) {
    switch (m) {
    case Medium::Text: return QStringLiteral("text");
    case Medium::Image: return QStringLiteral("image");
    case Medium::Video: return QStringLiteral("video");
    case Medium::Audio: return QStringLiteral("audio");
    }
    return QStringLiteral("text");
}

std::optional<Medium> mediumFromName(QStringView name) {
    if (name == u"text") return Medium::Text;
    if (name == u"image") return Medium::Image;
    if (name == u"video") return Medium::Video;
    if (name == u"audio") return Medium::Audio;
    return std::nullopt;
}

Medium mediumOf(Kind k) {
    return k == Kind::Image ? Medium::Image : k == Kind::Video ? Medium::Video : (k == Kind::Speech || k == Kind::Stt) ? Medium::Audio : Medium::Text;
}

QString localModeName(LocalMode m) {
    switch (m) {
    case LocalMode::Off: return QStringLiteral("off");
    case LocalMode::Auto: return QStringLiteral("auto");
    case LocalMode::Prefer: return QStringLiteral("prefer");
    case LocalMode::Only: return QStringLiteral("only");
    }
    return QStringLiteral("auto");
}

std::optional<LocalMode> localModeFromName(QStringView name) {
    if (name == u"off") return LocalMode::Off;
    if (name == u"auto") return LocalMode::Auto;
    if (name == u"prefer") return LocalMode::Prefer;
    if (name == u"only") return LocalMode::Only;
    return std::nullopt;
}

// ---------------- RunOptions JSON ----------------

QJsonObject RunOptions::toJson() const {
    QJsonObject o{{QStringLiteral("sliders"), QJsonObject{{QStringLiteral("cost"), cost}, {QStringLiteral("complexity"), complexity}}}};
    if (allowFree) o.insert(QStringLiteral("allowFree"), true);
    if (denyDataCollection) o.insert(QStringLiteral("denyDataCollection"), true);
    if (!pins.isEmpty()) {
        QJsonObject p;
        for (auto it = pins.constBegin(); it != pins.constEnd(); ++it) p.insert(roleName(it.key()), it.value());
        o.insert(QStringLiteral("pins"), p);
    }
    if (!exclude.isEmpty()) o.insert(QStringLiteral("exclude"), QJsonArray::fromStringList(exclude));
    if (local != LocalMode::Auto) o.insert(QStringLiteral("local"), localModeName(local));
    const auto media = [](const QMap<Medium, double>& m) {
        QJsonObject t;
        for (auto it = m.cbegin(); it != m.cend(); ++it) t.insert(mediumName(it.key()), it.value());
        return t;
    };
    if (tiers) o.insert(QStringLiteral("tiers"), media(*tiers));
    if (speed) o.insert(QStringLiteral("speed"), media(*speed));
    return o;
}

RunOptions RunOptions::fromJson(const QJsonObject& o) {
    RunOptions r;
    const QJsonObject sl = o.value(QLatin1String("sliders")).toObject();
    r.cost = sl.value(QLatin1String("cost")).toDouble(50);
    r.complexity = sl.value(QLatin1String("complexity")).toDouble(50);
    r.allowFree = o.value(QLatin1String("allowFree")).toBool();
    r.denyDataCollection = o.value(QLatin1String("denyDataCollection")).toBool();
    const QJsonObject pins = o.value(QLatin1String("pins")).toObject();
    for (auto it = pins.constBegin(); it != pins.constEnd(); ++it)
        if (const auto role = roleFromName(it.key()); role && it.value().isString() && !it.value().toString().isEmpty()) r.pins.insert(*role, it.value().toString());
    for (const QJsonValue& e : o.value(QLatin1String("exclude")).toArray())
        if (e.isString()) r.exclude << e.toString();
    r.local = localModeFromName(o.value(QLatin1String("local")).toString()).value_or(LocalMode::Auto);
    const auto media = [](const QJsonValue& v) -> std::optional<QMap<Medium, double>> {
        if (!v.isObject()) return std::nullopt;
        QMap<Medium, double> m;
        const QJsonObject t = v.toObject();
        for (auto it = t.constBegin(); it != t.constEnd(); ++it)
            if (const auto med = mediumFromName(it.key()); med && it.value().isDouble()) m.insert(*med, it.value().toDouble());
        return m;
    };
    r.tiers = media(o.value(QLatin1String("tiers")));
    r.speed = media(o.value(QLatin1String("speed")));
    return r;
}

// ---------------- dials ----------------

LocalMode localModeOf(const RunOptions& opts) { return opts.local; }

bool isNarrationTts(const ModelInfo& m) { return !(m.price.in == 0 && m.price.out > 0 && !(m.voices && !m.voices->isEmpty())); }

double tierFor(const RunOptions& opts, Kind kind) {
    double v = opts.cost;
    if (opts.tiers) {
        const auto it = opts.tiers->constFind(mediumOf(kind));
        if (it != opts.tiers->cend()) v = it.value();
    }
    return std::max(0.0, std::min(100.0, v)) / 100;
}

double qualityWeight(const RunOptions& opts, Kind kind) { return opts.tiers ? 0.25 + 0.75 * tierFor(opts, kind) : 1; }

int variantLean(QStringView id, QStringView name) {
    const QString i = id.toString(), n = name.toString();
    static const QRegularExpression prefix(QStringLiteral("^[^:]*:\\s*"));
    if (const auto l = leanOfWords(i.mid(i.indexOf(u'/') + 1), fastWords())) return *l;
    if (const auto l = leanOfWords(QString(n).replace(prefix, QString()), nameFastWords())) return *l;
    return 0;
}

std::optional<double> speedFor(const RunOptions& opts, Kind kind) {
    if (!opts.speed) return std::nullopt;
    const auto it = opts.speed->constFind(mediumOf(kind));
    if (it == opts.speed->cend() || !std::isfinite(it.value())) return std::nullopt;
    return std::max(0.0, std::min(100.0, it.value())) / 100;
}

double speedBias(const RunOptions& opts, const ModelInfo& m) {
    const auto s = speedFor(opts, m.kind);
    if (!s) return 0;
    const int lean = variantLean(m.id, m.name);
    return lean ? kSpeedWeight * qualityWeight(opts, m.kind) * (2 * *s - 1) * lean : 0;
}

// ---------------- roles ----------------

bool roleNeeds(Role role, const ModelInfo& m) {
    static const QRegularExpression imageNot(QStringLiteral("(vector|layer|edit|upscale|inpaint|remove|relight|restyle)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression imageEditNot(QStringLiteral("(vector|upscale|remove|relight|design-layer)"), QRegularExpression::CaseInsensitiveOption);
    const int maxRefs = m.image ? m.image->maxRefs : 0;
    switch (role) {
    case Role::Writer: return m.ctx >= 32000;
    case Role::Editor: return m.ctx >= 32000;
    case Role::Judge: return m.ctx >= 64000 && m.reasoning.has_value();
    case Role::Reasoner: return m.reasoning.has_value() && m.hasParam(u"tools");
    case Role::Research: return m.hasParam(u"tools") && m.ctx >= 32000;
    case Role::Fast: return structured(m) && m.ctx >= 16000;
    case Role::Vision: return m.hasInMod(u"image") && structured(m);
    case Role::Image: return !imageNot.match(m.id).hasMatch() && maxRefs >= 0;
    case Role::Video: return !(m.video && m.video->edit);
    case Role::ImageEdit: return maxRefs >= 1 && !imageEditNot.match(m.id).hasMatch();
    case Role::VideoI2V: return !(m.video && m.video->edit) && m.video && m.video->frames.contains(QLatin1String("first_frame"));
    case Role::VideoEdit: return m.video && m.video->edit;
    // token-priced models with no voice catalog are general audio generators (music/SFX from a prompt): usable when
    // pinned, but never auto-picked for narration, where their price-based quality score would put them first
    case Role::Tts: return isNarrationTts(m);
    case Role::TtsClone: return m.voiceCloning && isNarrationTts(m);
    case Role::Stt: return true;
    case Role::Embed: return !m.id.contains(QLatin1String("code"));
    case Role::Rerank: return true;
    }
    return false;
}

QList<const ModelInfo*> eligible(Role role, const PickCtx& ctx, const EligibleOptions& o) {
    QList<const ModelInfo*> out;
    if (!ctx.catalog) return out;
    const RoleSpec& s = spec(role);
    const double now = nowOf(ctx);
    const bool allowFree = ctx.freeOnly || ctx.opts.allowFree;
    const LocalMode localMode = localModeOf(ctx.opts);
    for (const ModelInfo& m : ctx.catalog->models()) {
        if (m.kind != s.kind) continue;
        if (!roleNeeds(role, m)) continue;
        if (o.json && m.kind == Kind::Text && !structured(m)) continue;
        if (expiring(m, now)) continue;
        if (!provider::limits::retiredReason(m.id).isEmpty()) continue;   // listed, but shut down by its provider
        if (excluded(ctx.opts, m)) continue;
        if (m.isLocal()) {
            // local models: $0 and private, so the free-only, data-collection and price-rank filters do not apply; only the policy does
            if (localMode != LocalMode::Off) out << &m;
            continue;
        }
        if (localMode == LocalMode::Only) continue;
        if (ctx.freeOnly) { if (m.free) out << &m; continue; }
        if (m.free && !allowFree) continue;
        if (m.free && ctx.opts.denyDataCollection) continue;   // free endpoints typically retain prompts
        if (s.maxPriceRank < 1 && !m.free && ctx.catalog->priceRank(m) > s.maxPriceRank) continue;
        out << &m;
    }
    return out;
}

bool fitsTime(const ModelInfo& m, const CostEstimate& est, std::optional<double> secondsLeft) {
    return !m.isLocal() || !secondsLeft || est.seconds.value_or(0) <= *secondsLeft + 1e-9;
}

QString pinIneligible(Role role, const PickCtx& ctx, const ModelInfo* m, QStringView id) {
    if (!m) return QStringLiteral("%1 is not in the model catalog").arg(id.isEmpty() ? QStringLiteral("that model") : id.toString());
    const RoleSpec& s = spec(role);
    if (m->kind != s.kind) return QStringLiteral("%1 is a %2 model; this step needs a %3 model").arg(m->id, kindName(m->kind), kindName(s.kind));
    const QString retired = provider::limits::retiredReason(m->id);
    if (!retired.isEmpty()) return retired;
    if (expiring(*m, nowOf(ctx))) return QStringLiteral("%1 is about to be retired by its provider").arg(m->id);
    if (excluded(ctx.opts, *m)) return QStringLiteral("%1 is excluded in the settings").arg(m->id);
    if (ctx.freeOnly && !m->free) return QStringLiteral("the cost dial is at 0, so only free models can run (%1 is paid)").arg(m->id);
    if (m->free && ctx.opts.denyDataCollection)
        return QStringLiteral("%1 is a free endpoint and free endpoints keep prompts (data collection is denied)").arg(m->id);
    if (!roleNeeds(role, *m)) return QStringLiteral("%1 cannot do this step (an editing-only or unsuitable model)").arg(m->id);
    return {};
}

PinnedResult pinnedPick(Role role, const PickCtx& ctx, QStringView id, const Estimator& estimate, const QStringList& fallbackIds) {
    const ModelInfo* m = ctx.catalog ? ctx.catalog->byId(id) : nullptr;
    const QString why = pinIneligible(role, ctx, m, id);
    if (!why.isEmpty()) return {std::nullopt, why};
    const CostEstimate est = estimate(*m);
    if (!est.finite()) return {std::nullopt, QStringLiteral("no price is known for %1").arg(id)};
    QStringList fallbacks;
    for (const QString& f : fallbackIds) {
        if (f == id || fallbacks.contains(f)) continue;
        if (pinIneligible(role, ctx, ctx.catalog->byId(f), f).isEmpty()) fallbacks << f;
    }
    return {Pick{*m, est, fallbacks, true}, {}};
}

std::optional<Pick> pickModel(Role role, const PickCtx& ctx, double allowance, const Estimator& estimate, const PickOptions& o) {
    if (!ctx.catalog) return std::nullopt;
    const RoleSpec& s = spec(role);
    const auto pinIt = ctx.opts.pins.constFind(role);
    if (pinIt != ctx.opts.pins.cend() && !pinIt.value().isEmpty()) {
        if (const ModelInfo* pinned = ctx.catalog->byId(pinIt.value()); pinned && pinned->kind == s.kind)
            return Pick{*pinned, estimate(*pinned), {}, true};
    }
    const LocalMode localMode = localModeOf(ctx.opts);
    // Above the middle of the cost slider, lean toward the flagship end of each price range.
    const double bias = std::max(0.0, (ctx.opts.cost - 40) / 60) * 0.6;
    // With per-medium tiers (Storyboard Studio) the slider is a true cost↔quality dial: low prefers the cheapest model
    // that can do the job, high prefers the most capable one, and the middle balances the two.
    const double t = tierFor(ctx.opts, s.kind), qw = qualityWeight(ctx.opts, s.kind);
    const auto base = [&](const ModelInfo& m) {
        return ctx.opts.tiers ? m.quality * qw + (m.free ? 0 : ctx.catalog->priceRank(m)) * (t - 0.5) * 0.8
                              : m.quality + bias * ctx.catalog->priceRank(m);
    };
    const auto rankKey = [&](const ModelInfo& m) {
        double k = base(m);
        if (ctx.opts.speed) k += speedBias(ctx.opts, m);    // only re-ranks models already eligible and affordable
        if (localMode == LocalMode::Prefer && m.isLocal()) k += kPreferLocalBoost;
        return k;
    };
    QList<Scored> scored;
    for (const ModelInfo* m : eligible(role, ctx, {o.json})) {
        if (o.exclude.contains(m->id)) continue;
        CostEstimate est = estimate(*m);
        if (!est.finite() || est.usd > allowance + 1e-12 || !fitsTime(*m, est, o.secondsLeft)) continue;
        scored << Scored{m, std::move(est), rankKey(*m)};
    }
    std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        if (a.key != b.key) return a.key > b.key;
        if (a.est.usd != b.est.usd) return a.est.usd < b.est.usd;
        return a.seconds() < b.seconds();
    });
    if (scored.isEmpty()) return std::nullopt;
    const Scored* primary = &scored[0];
    if (o.distinctProviderFrom)
        for (const Scored& x : scored)
            if (!o.distinctProviderFrom->contains(x.m->providerPrefix())) { primary = &x; break; }
    QStringList fallbacks;
    for (const Scored& x : scored) {
        if (fallbacks.size() >= 2) break;
        if (x.m->id != primary->m->id && x.m->providerPrefix() != primary->m->providerPrefix()) fallbacks << x.m->id;
    }
    return Pick{*primary->m, primary->est, fallbacks, false};
}

std::optional<Pick> cheapestModel(Role role, const PickCtx& ctx, const Estimator& estimate, const CheapestOptions& o) {
    QList<Scored> all;
    for (const ModelInfo* m : eligible(role, ctx, {o.json})) {
        CostEstimate est = estimate(*m);
        if (!est.finite()) continue;
        all << Scored{m, std::move(est), 0};
    }
    std::stable_sort(all.begin(), all.end(), [](const Scored& a, const Scored& b) {
        if (a.est.usd != b.est.usd) return a.est.usd < b.est.usd;
        if (a.seconds() != b.seconds()) return a.seconds() < b.seconds();
        return a.m->quality > b.m->quality;
    });
    QList<Scored> inTime;
    for (const Scored& x : all)
        if (fitsTime(*x.m, x.est, o.secondsLeft)) inTime << x;
    const QList<Scored>& scored = inTime.isEmpty() ? all : inTime;
    if (scored.isEmpty()) return std::nullopt;
    QStringList fallbacks;
    for (int i = 1; i < scored.size() && i < 3; ++i) fallbacks << scored[i].m->id;
    return Pick{*scored[0].m, scored[0].est, fallbacks, false};
}

} // namespace studio::catalog
