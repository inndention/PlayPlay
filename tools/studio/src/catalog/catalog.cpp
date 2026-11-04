// The model catalog (ST-08): starmedia src/catalog.ts. See catalog.h.
#include "catalog/catalog.h"
#include "catalog/quality.h"
#include "core/log.h"
#include "provider/openrouter.h"
#include "provider/wire.h"

#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimeZone>
#include <algorithm>
#include <cmath>

namespace studio::catalog {

namespace {

constexpr QStringView kLog = u"catalog";

// Models that are not usable as generic text-in/text-out workhorses in a publishing pipeline (TS TEXT_SKIP).
const QRegularExpression& textSkip() {
    static const QRegularExpression re(QStringLiteral(
        R"((:batch|:extended|:nitro|:floor|:online|:thinking|-audio|-tts|-transcribe|-realtime|-search-preview|deep-research|-codex|-code\b|\bcoder\b))"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}
const QRegularExpression& editModel() {
    static const QRegularExpression re(QStringLiteral("(edit|aleph)"), QRegularExpression::CaseInsensitiveOption);
    return re;
}
const QRegularExpression& notUsableVideo() {
    static const QRegularExpression re(QStringLiteral("(upscale|avatar|continuation|lipsync)"), QRegularExpression::CaseInsensitiveOption);
    return re;
}

QStringList strings(const QJsonValue& v) {
    QStringList out;
    for (const QJsonValue& x : v.toArray()) out << x.toString();
    return out;
}
QJsonArray array(const QStringList& l) {
    QJsonArray a;
    for (const QString& s : l) a.append(s);
    return a;
}
QList<int> ints(const QJsonValue& v) {
    QList<int> out;
    for (const QJsonValue& x : v.toArray()) out << int(x.toDouble());
    return out;
}
QJsonArray array(const QList<int>& l) {
    QJsonArray a;
    for (int i : l) a.append(i);
    return a;
}
std::optional<double> optNum(const QJsonValue& v) {
    return v.isDouble() ? std::optional<double>(v.toDouble()) : std::nullopt;
}
void put(QJsonObject& o, const QString& k, const std::optional<double>& v) {
    if (v) o.insert(k, *v);
}

// Date.parse(iso) / 1000: a date-only text is UTC midnight, a date-time without a zone is local time, as in JS.
std::optional<double> parseDateSec(const QString& text) {
    if (text.isEmpty()) return std::nullopt;
    if (text.size() == 10) {
        const QDate d = QDate::fromString(text, Qt::ISODate);
        if (!d.isValid()) return std::nullopt;
        return double(QDateTime(d, QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch()) / 1000;
    }
    const QDateTime dt = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!dt.isValid()) return std::nullopt;
    return double(dt.toMSecsSinceEpoch()) / 1000;
}

} // namespace

// ---------------- names ----------------

QString kindName(Kind k) {
    switch (k) {
    case Kind::Text: return QStringLiteral("text");
    case Kind::Image: return QStringLiteral("image");
    case Kind::Video: return QStringLiteral("video");
    case Kind::Speech: return QStringLiteral("speech");
    case Kind::Stt: return QStringLiteral("stt");
    case Kind::Embed: return QStringLiteral("embed");
    case Kind::Rerank: return QStringLiteral("rerank");
    }
    return QStringLiteral("text");
}

std::optional<Kind> kindFromName(QStringView name) {
    static const QHash<QString, Kind> map = {
        {QStringLiteral("text"), Kind::Text},     {QStringLiteral("image"), Kind::Image}, {QStringLiteral("video"), Kind::Video},
        {QStringLiteral("speech"), Kind::Speech}, {QStringLiteral("stt"), Kind::Stt},     {QStringLiteral("embed"), Kind::Embed},
        {QStringLiteral("rerank"), Kind::Rerank}};
    const auto it = map.find(name.toString());
    return it == map.end() ? std::nullopt : std::optional<Kind>(it.value());
}

// ---------------- JSON ----------------

QJsonObject ModelInfo::toJson() const {
    QJsonObject o;
    o.insert(QStringLiteral("id"), id);
    o.insert(QStringLiteral("name"), name);
    o.insert(QStringLiteral("kind"), kindName(kind));
    o.insert(QStringLiteral("created"), created);
    o.insert(QStringLiteral("ctx"), ctx);
    put(o, QStringLiteral("maxOut"), maxOut);
    o.insert(QStringLiteral("inMods"), array(inMods));
    o.insert(QStringLiteral("outMods"), array(outMods));
    QJsonObject p{{QStringLiteral("in"), price.in}, {QStringLiteral("out"), price.out}};
    put(p, QStringLiteral("webSearch"), price.webSearch);
    put(p, QStringLiteral("reasoning"), price.reasoning);
    put(p, QStringLiteral("cacheRead"), price.cacheRead);
    o.insert(QStringLiteral("price"), p);
    o.insert(QStringLiteral("free"), free);
    o.insert(QStringLiteral("params"), array(params));
    if (voices) o.insert(QStringLiteral("voices"), array(*voices));
    put(o, QStringLiteral("expires"), expires);
    if (reasoning)
        o.insert(QStringLiteral("reasoning"), QJsonObject{{QStringLiteral("efforts"), array(reasoning->efforts)},
                                                          {QStringLiteral("mandatory"), reasoning->mandatory},
                                                          {QStringLiteral("defaultEnabled"), reasoning->defaultEnabled}});
    o.insert(QStringLiteral("quality"), quality);
    if (image) {
        QJsonObject s;
        put(s, QStringLiteral("perImage"), image->perImage);
        put(s, QStringLiteral("perMegapixel"), image->perMegapixel);
        put(s, QStringLiteral("perOutputToken"), image->perOutputToken);
        put(s, QStringLiteral("perInputToken"), image->perInputToken);
        s.insert(QStringLiteral("aspects"), array(image->aspects));
        s.insert(QStringLiteral("qualities"), array(image->qualities));
        s.insert(QStringLiteral("maxN"), image->maxN);
        s.insert(QStringLiteral("maxRefs"), image->maxRefs);
        s.insert(QStringLiteral("streaming"), image->streaming);
        o.insert(QStringLiteral("image"), s);
    }
    if (video) {
        QJsonObject v{{QStringLiteral("durations"), array(video->durations)},  {QStringLiteral("resolutions"), array(video->resolutions)},
                      {QStringLiteral("aspects"), array(video->aspects)},      {QStringLiteral("audio"), video->audio},
                      {QStringLiteral("frames"), array(video->frames)},        {QStringLiteral("skus"), video->skus}};
        if (video->edit) v.insert(QStringLiteral("edit"), true);
        o.insert(QStringLiteral("video"), v);
    }
    if (voiceCloning) o.insert(QStringLiteral("voiceCloning"), true);
    switch (provider) {
    case Provider::Unset: break;
    case Provider::OpenRouter: o.insert(QStringLiteral("provider"), QStringLiteral("openrouter")); break;
    case Provider::Local: o.insert(QStringLiteral("provider"), QStringLiteral("local")); break;
    case Provider::Pool: o.insert(QStringLiteral("provider"), QStringLiteral("pool")); break;
    }
    if (local) {
        QJsonObject l = local->extra;
        l.insert(QStringLiteral("backend"), local->backend);
        QJsonObject t;
        put(t, QStringLiteral("tokensPerSec"), local->throughput.tokensPerSec);
        put(t, QStringLiteral("secondsPerImage"), local->throughput.secondsPerImage);
        put(t, QStringLiteral("secondsPerVideoSecond"), local->throughput.secondsPerVideoSecond);
        put(t, QStringLiteral("secondsPerSpokenSecond"), local->throughput.secondsPerSpokenSecond);
        put(t, QStringLiteral("secondsPerAudioMinute"), local->throughput.secondsPerAudioMinute);
        if (!t.isEmpty()) l.insert(QStringLiteral("throughput"), t);
        put(l, QStringLiteral("vramGb"), local->vramGb);
        if (local->verified) l.insert(QStringLiteral("verified"), *local->verified);
        if (!local->licence.isEmpty()) l.insert(QStringLiteral("licence"), local->licence);
        o.insert(QStringLiteral("local"), l);
    }
    return o;
}

ModelInfo ModelInfo::fromJson(const QJsonObject& o) {
    ModelInfo m;
    m.id = o.value(QLatin1String("id")).toString();
    m.name = o.value(QLatin1String("name")).toString();
    m.kind = kindFromName(o.value(QLatin1String("kind")).toString()).value_or(Kind::Text);
    m.created = o.value(QLatin1String("created")).toDouble();
    m.ctx = o.value(QLatin1String("ctx")).toDouble();
    m.maxOut = optNum(o.value(QLatin1String("maxOut")));
    m.inMods = strings(o.value(QLatin1String("inMods")));
    m.outMods = strings(o.value(QLatin1String("outMods")));
    const QJsonObject p = o.value(QLatin1String("price")).toObject();
    m.price.in = p.value(QLatin1String("in")).toDouble();
    m.price.out = p.value(QLatin1String("out")).toDouble();
    m.price.webSearch = optNum(p.value(QLatin1String("webSearch")));
    m.price.reasoning = optNum(p.value(QLatin1String("reasoning")));
    m.price.cacheRead = optNum(p.value(QLatin1String("cacheRead")));
    m.free = o.value(QLatin1String("free")).toBool();
    m.params = strings(o.value(QLatin1String("params")));
    if (o.value(QLatin1String("voices")).isArray()) m.voices = strings(o.value(QLatin1String("voices")));
    m.voiceCloning = o.value(QLatin1String("voiceCloning")).toBool();
    m.expires = optNum(o.value(QLatin1String("expires")));
    if (o.value(QLatin1String("reasoning")).isObject()) {
        const QJsonObject r = o.value(QLatin1String("reasoning")).toObject();
        m.reasoning = ReasoningSpec{strings(r.value(QLatin1String("efforts"))), r.value(QLatin1String("mandatory")).toBool(),
                                    r.value(QLatin1String("defaultEnabled")).toBool()};
    }
    m.quality = o.value(QLatin1String("quality")).toDouble();
    if (o.value(QLatin1String("image")).isObject()) {
        const QJsonObject s = o.value(QLatin1String("image")).toObject();
        ImageSpec spec;
        spec.perImage = optNum(s.value(QLatin1String("perImage")));
        spec.perMegapixel = optNum(s.value(QLatin1String("perMegapixel")));
        spec.perOutputToken = optNum(s.value(QLatin1String("perOutputToken")));
        spec.perInputToken = optNum(s.value(QLatin1String("perInputToken")));
        spec.aspects = strings(s.value(QLatin1String("aspects")));
        spec.qualities = strings(s.value(QLatin1String("qualities")));
        spec.maxN = s.value(QLatin1String("maxN")).toInt(1);
        spec.maxRefs = s.value(QLatin1String("maxRefs")).toInt(0);
        spec.streaming = s.value(QLatin1String("streaming")).toBool();
        m.image = spec;
    }
    if (o.value(QLatin1String("video")).isObject()) {
        const QJsonObject v = o.value(QLatin1String("video")).toObject();
        VideoSpec spec;
        spec.durations = ints(v.value(QLatin1String("durations")));
        spec.resolutions = strings(v.value(QLatin1String("resolutions")));
        spec.aspects = strings(v.value(QLatin1String("aspects")));
        spec.audio = v.value(QLatin1String("audio")).toBool();
        spec.frames = strings(v.value(QLatin1String("frames")));
        spec.skus = v.value(QLatin1String("skus")).toObject();
        spec.edit = v.value(QLatin1String("edit")).toBool();
        m.video = spec;
    }
    const QString prov = o.value(QLatin1String("provider")).toString();
    m.provider = prov == QLatin1String("local") ? Provider::Local : prov == QLatin1String("pool") ? Provider::Pool
               : prov == QLatin1String("openrouter") ? Provider::OpenRouter : Provider::Unset;
    if (o.value(QLatin1String("local")).isObject()) {
        QJsonObject l = o.value(QLatin1String("local")).toObject();
        LocalSpec spec;
        spec.backend = l.take(QStringLiteral("backend")).toString();
        const QJsonObject t = l.take(QStringLiteral("throughput")).toObject();
        spec.throughput.tokensPerSec = optNum(t.value(QLatin1String("tokensPerSec")));
        spec.throughput.secondsPerImage = optNum(t.value(QLatin1String("secondsPerImage")));
        spec.throughput.secondsPerVideoSecond = optNum(t.value(QLatin1String("secondsPerVideoSecond")));
        spec.throughput.secondsPerSpokenSecond = optNum(t.value(QLatin1String("secondsPerSpokenSecond")));
        spec.throughput.secondsPerAudioMinute = optNum(t.value(QLatin1String("secondsPerAudioMinute")));
        spec.vramGb = optNum(l.take(QStringLiteral("vramGb")));
        const QJsonValue ver = l.take(QStringLiteral("verified"));
        if (ver.isBool()) spec.verified = ver.toBool();
        spec.licence = l.take(QStringLiteral("licence")).toString();
        spec.extra = l;
        m.local = spec;
    }
    return m;
}

QJsonObject CacheEntry::toJson() const {
    QJsonArray a;
    for (const ModelInfo& m : models) a.append(m.toJson());
    return QJsonObject{{QStringLiteral("version"), version}, {QStringLiteral("fetchedAt"), double(fetchedAt)}, {QStringLiteral("models"), a}};
}

CacheEntry CacheEntry::fromJson(const QJsonObject& o) {
    CacheEntry e;
    e.version = o.value(QLatin1String("version")).toInt();
    e.fetchedAt = qint64(o.value(QLatin1String("fetchedAt")).toDouble());
    for (const QJsonValue& v : o.value(QLatin1String("models")).toArray())
        if (v.isObject()) e.models << ModelInfo::fromJson(v.toObject());
    return e;
}

// ---------------- Catalog ----------------

Catalog::Catalog(qint64 fetchedAt, QList<ModelInfo> models) : m_fetchedAt(fetchedAt), m_models(std::move(models)) {
    for (int i = 0; i < m_models.size(); ++i) m_index.insert(m_models[i].id, i);   // the last wins, as a JS Map
}

const ModelInfo* Catalog::byId(QStringView id) const {
    const auto it = m_index.constFind(id.toString());
    return it == m_index.cend() ? nullptr : &m_models[it.value()];
}

double Catalog::priceRank(const ModelInfo& m) const {
    if (!m_priceRanked) {
        m_priceRanked = true;
        QHash<int, QList<const ModelInfo*>> groups;
        for (const ModelInfo& x : m_models)
            // local models cost $0 and would otherwise sit at the bottom of every price range, squeezing the hosted percentiles
            if (!x.free && !x.isLocal()) groups[int(x.kind)] << &x;
        const auto key = [](const ModelInfo* x) { return x->price.in * 0.3 + x->price.out * 0.7; };
        for (auto& group : groups) {
            std::stable_sort(group.begin(), group.end(), [&key](const ModelInfo* a, const ModelInfo* b) { return key(a) - key(b) < 0; });
            for (int i = 0; i < group.size(); ++i)
                m_priceRank.insert(group[i]->id, group.size() > 1 ? double(i) / double(group.size() - 1) : 0.0);
        }
    }
    return m_priceRank.value(m.id, 0.0);
}

QJsonObject Catalog::summary() const {
    QMap<QString, QPair<int, int>> by;
    for (const ModelInfo& m : m_models) {
        auto& e = by[kindName(m.kind)];
        e.first++;
        if (m.free) e.second++;
    }
    QJsonObject o;
    for (auto it = by.cbegin(); it != by.cend(); ++it)
        o.insert(it.key(), QJsonObject{{QStringLiteral("total"), it.value().first}, {QStringLiteral("free"), it.value().second}});
    return o;
}

// ---------------- the pure build ----------------

double num(const QJsonValue& v) {
    if (v.isDouble()) return v.toDouble();
    if (v.isBool()) return v.toBool() ? 1 : 0;          // Number(true) = 1
    if (!v.isString()) return 0;                        // undefined, null, objects (NaN || 0)
    const QString s = v.toString().trimmed();
    if (s.isEmpty()) return 0;
    bool ok = false;
    const double d = s.toDouble(&ok);
    return ok ? d : 0;
}

std::optional<Kind> kindOf(const QStringList& out) {
    if (out.contains(QLatin1String("speech"))) return Kind::Speech;
    if (out.contains(QLatin1String("transcription"))) return Kind::Stt;
    if (out.contains(QLatin1String("embeddings"))) return Kind::Embed;
    if (out.contains(QLatin1String("rerank"))) return Kind::Rerank;
    if (out.contains(QLatin1String("video"))) return Kind::Video;
    if (out.size() == 1 && out[0] == QLatin1String("text")) return Kind::Text;
    if (out.contains(QLatin1String("image"))) return Kind::Image;
    return std::nullopt;
}

ImageSpec imageSpecFromEndpoints(const QJsonObject& d) {
    ImageSpec spec;
    const double inf = std::numeric_limits<double>::infinity();
    double perImage = inf, perMp = inf, perOutTok = inf, perInTok = inf;
    const auto addUnique = [](QStringList& into, const QJsonValue& values) {
        for (const QJsonValue& v : values.toArray()) {
            const QString s = v.toString();
            if (!into.contains(s)) into << s;
        }
    };
    for (const QJsonValue& epv : d.value(QLatin1String("endpoints")).toArray()) {
        const QJsonObject ep = epv.toObject();
        const QJsonObject sp = ep.value(QLatin1String("supported_parameters")).toObject();
        if (sp.value(QLatin1String("aspect_ratio")).toObject().contains(QLatin1String("values")))
            addUnique(spec.aspects, sp.value(QLatin1String("aspect_ratio")).toObject().value(QLatin1String("values")));
        if (sp.value(QLatin1String("quality")).toObject().contains(QLatin1String("values")))
            addUnique(spec.qualities, sp.value(QLatin1String("quality")).toObject().value(QLatin1String("values")));
        const double maxN = sp.value(QLatin1String("n")).toObject().value(QLatin1String("max")).toDouble();
        if (maxN) spec.maxN = std::max(spec.maxN, int(maxN));
        const double maxRefs = sp.value(QLatin1String("input_references")).toObject().value(QLatin1String("max")).toDouble();
        if (maxRefs) spec.maxRefs = std::max(spec.maxRefs, int(maxRefs));
        if (ep.value(QLatin1String("supports_streaming")).toBool()) spec.streaming = true;
        for (const QJsonValue& pv : ep.value(QLatin1String("pricing")).toArray()) {
            const QJsonObject p = pv.toObject();
            const double c = num(p.value(QLatin1String("cost_usd")));
            const QString billable = p.value(QLatin1String("billable")).toString(), unit = p.value(QLatin1String("unit")).toString();
            if (billable == QLatin1String("output_image")) {
                if (unit == QLatin1String("image")) perImage = std::min(perImage, c);
                else if (unit == QLatin1String("megapixel")) perMp = std::min(perMp, c);
                else if (unit == QLatin1String("token")) perOutTok = std::min(perOutTok, c);
            } else if (billable == QLatin1String("input_text") && unit == QLatin1String("token")) {
                perInTok = std::min(perInTok, c);
            }
        }
    }
    if (perImage < inf) spec.perImage = perImage;
    else if (perMp < inf) spec.perMegapixel = perMp;
    else if (perOutTok < inf) {
        spec.perOutputToken = perOutTok;
        if (perInTok < inf) spec.perInputToken = perInTok;
    }
    return spec;
}

bool textSkipped(QStringView id) { return textSkip().matchView(id).hasMatch(); }
bool videoUsable(QStringView id) { return !notUsableVideo().matchView(id).hasMatch(); }
bool videoEditModel(QStringView id) { return editModel().matchView(id).hasMatch(); }

QList<ModelInfo> buildCatalog(const BuildInput& in) {
    QHash<QString, QJsonObject> videoById;
    for (const QJsonValue& v : in.videos) videoById.insert(v.toObject().value(QLatin1String("id")).toString(), v.toObject());
    QList<ModelInfo> models;
    for (const QJsonValue& mv : in.models) {
        const QJsonObject m = mv.toObject();
        const QJsonObject arch = m.value(QLatin1String("architecture")).toObject();
        const QStringList out = strings(arch.value(QLatin1String("output_modalities")));
        const auto kind = kindOf(out);
        if (!kind) continue;
        const QJsonObject pr = m.value(QLatin1String("pricing")).toObject();
        ModelInfo info;
        info.id = m.value(QLatin1String("id")).toString();
        info.name = m.value(QLatin1String("name")).toString();
        info.kind = *kind;
        info.created = m.value(QLatin1String("created")).toDouble(0);
        info.ctx = m.value(QLatin1String("context_length")).toDouble(0);
        info.maxOut = optNum(m.value(QLatin1String("top_provider")).toObject().value(QLatin1String("max_completion_tokens")));
        info.inMods = strings(arch.value(QLatin1String("input_modalities")));
        info.outMods = out;
        info.price.in = num(pr.value(QLatin1String("prompt")));
        info.price.out = num(pr.value(QLatin1String("completion")));
        // `pr.web_search ? n(...) : undefined`: a JS-truthy value ("0" is truthy, 0 and "" are not)
        const auto truthy = [](const QJsonValue& v) {
            return v.isString() ? !v.toString().isEmpty() : v.isDouble() ? v.toDouble() != 0 : v.isBool() ? v.toBool() : v.isObject() || v.isArray();
        };
        if (truthy(pr.value(QLatin1String("web_search")))) info.price.webSearch = num(pr.value(QLatin1String("web_search")));
        if (truthy(pr.value(QLatin1String("input_cache_read")))) info.price.cacheRead = num(pr.value(QLatin1String("input_cache_read")));
        info.free = info.id.endsWith(QLatin1String(":free")) || (info.price.in == 0 && info.price.out == 0 && info.kind == Kind::Text);
        info.params = strings(m.value(QLatin1String("supported_parameters")));
        if (m.value(QLatin1String("supported_voices")).isArray()) info.voices = strings(m.value(QLatin1String("supported_voices")));
        const QString expiration = m.value(QLatin1String("expiration_date")).toString();
        if (!expiration.isEmpty()) info.expires = parseDateSec(expiration);
        if (truthy(m.value(QLatin1String("reasoning")))) {
            const QJsonObject r = m.value(QLatin1String("reasoning")).toObject();
            info.reasoning = ReasoningSpec{strings(r.value(QLatin1String("supported_efforts"))), r.value(QLatin1String("mandatory")).toBool(),
                                           r.value(QLatin1String("default_enabled")).toBool()};
        }
        if (info.id.contains(QLatin1String(":batch"))) continue;       // async batch endpoints are not interactive
        if (info.kind == Kind::Text && textSkipped(info.id)) continue;
        if (info.id.startsWith(u'~')) continue;                        // moving aliases duplicate a concrete id
        if (info.kind == Kind::Image) {
            const auto spec = in.imageSpecs.constFind(info.id);
            if (spec == in.imageSpecs.cend()) continue;                // only models reachable through the Image API
            info.image = spec.value();
            // free only on a published $0 price (or a :free id): pricing this client cannot parse is unknown, never free
            info.free = info.id.endsWith(QLatin1String(":free")) || info.image->perImage == 0.0
                        || (!info.image->perImage && info.image->perMegapixel == 0.0);
        }
        if (info.kind == Kind::Video) {
            const auto v = videoById.constFind(info.id);
            if (v == videoById.cend() || !videoUsable(info.id)) continue;
            const bool isEdit = videoEditModel(info.id);
            VideoSpec vs;
            vs.durations = ints(v->value(QLatin1String("supported_durations")));
            vs.resolutions = strings(v->value(QLatin1String("supported_resolutions")));
            vs.aspects = strings(v->value(QLatin1String("supported_aspect_ratios")));
            vs.audio = truthy(v->value(QLatin1String("generate_audio")));
            vs.frames = strings(v->value(QLatin1String("supported_frame_images")));
            vs.skus = v->value(QLatin1String("pricing_skus")).toObject();
            vs.edit = isEdit;
            if (!isEdit && (vs.durations.isEmpty() || vs.resolutions.isEmpty())) continue;
            info.video = vs;
            info.free = false;
        }
        if (info.kind == Kind::Speech && in.voiceCloning.contains(info.id)) info.voiceCloning = true;
        models << info;
    }
    score(models, in.nowSec, in.bonus);
    return models;
}

QList<ModelInfo> mergeSources(QList<ModelInfo> models, const QList<ModelInfo>& extra) {
    if (extra.isEmpty()) return models;
    QSet<QString> replaced;
    for (const ModelInfo& m : extra) replaced.insert(m.id);
    models.erase(std::remove_if(models.begin(), models.end(), [&replaced](const ModelInfo& m) { return replaced.contains(m.id); }), models.end());
    models += extra;
    return models;
}

// ---------------- caches ----------------

std::optional<CacheEntry> FileCatalogCache::load() {
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return std::nullopt;
    return CacheEntry::fromJson(doc.object());
}

void FileCatalogCache::save(const CacheEntry& entry) {
    QSaveFile f(m_path);
    if (!f.open(QIODevice::WriteOnly)) {
        log::warn(kLog, QStringLiteral("catalog cache %1 not writable: %2").arg(m_path, f.errorString()));
        return;
    }
    f.write(QJsonDocument(entry.toJson()).toJson(QJsonDocument::Compact));
    if (!f.commit()) log::warn(kLog, QStringLiteral("catalog cache %1 not written: %2").arg(m_path, f.errorString()));
}

// ---------------- the loader ----------------

struct CatalogLoader::Build {
    QPointer<QObject> context;
    bool hasContext = false;
    Done done;
    Failed failed;
    std::optional<CacheEntry> cached;
    bool usable = false;
    QJsonArray models;                                  // the three top-level answers
    QJsonArray videos;
    QHash<QString, ImageSpec> specs;
    QSet<QString> cloning;
    QList<QString> imageQueue;                          // image model ids whose /endpoints are still to fetch
    int imageInFlight = 0;
    QList<QString> speechQueue;                         // speech model ids whose /endpoints are still to fetch
    int speechInFlight = 0;
    int listsOutstanding = 3;                           // top-level calls + image endpoint calls not answered yet
    bool modelsFailed = false;
    QString why;
    bool finished = false;
};

CatalogLoader::CatalogLoader(provider::OpenRouter& api, Clock& clock, QObject* parent) : QObject(parent), m_api(api), m_clock(clock) {}
CatalogLoader::~CatalogLoader() = default;

void CatalogLoader::setSource(const QString& name, CatalogSource source) {
    if (source) m_sources.insert(name, std::move(source));
    else m_sources.remove(name);
}

Catalog CatalogLoader::withSources(qint64 fetchedAt, QList<ModelInfo> models) const {
    QList<ModelInfo> extra;
    for (auto it = m_sources.cbegin(); it != m_sources.cend(); ++it) {
        bool ok = true;
        QList<ModelInfo> got = it.value()(&ok);
        if (!ok) continue;                              // a source that is down must not break the catalog
        extra += got;
    }
    return Catalog(fetchedAt, mergeSources(std::move(models), extra));
}

void CatalogLoader::deliver(bool hadContext, const QPointer<QObject>& alive, std::function<void()> fn) {
    // queued to the context (or this loader), never run inside load(); a context that died meanwhile gets nothing
    QObject* target = alive ? alive.data() : this;
    QMetaObject::invokeMethod(target, [alive, hadContext, fn = std::move(fn)]() {
        if (hadContext && alive.isNull()) return;
        fn();
    }, Qt::QueuedConnection);
}

void CatalogLoader::load(bool refresh, QObject* context, Done done, Failed failed) {
    auto b = std::make_shared<Build>();
    b->context = context;
    b->hasContext = context != nullptr;
    b->done = std::move(done);
    b->failed = std::move(failed);
    if (m_cache) b->cached = m_cache->load();
    b->usable = b->cached && b->cached->version == kCacheVersion && !b->cached->models.isEmpty();
    const qint64 now = m_clock.nowMs();
    if (!refresh && b->usable && now - b->cached->fetchedAt < kCatalogTtlMs) {
        Catalog cat = withSources(b->cached->fetchedAt, b->cached->models);
        deliver(b->hasContext, b->context, [cat = std::move(cat), done = b->done]() { if (done) done(cat); });
        return;
    }
    ++m_pending;
    const auto settled = [this, b]() { if (--b->listsOutstanding == 0) listsDone(b); };
    // Promise.all of the three lists: /videos/models and /images/models may fail on their own, /models may not.
    m_api.getJson(QStringLiteral("/models?output_modalities=all"), this,
                  [b, settled](const QJsonValue& data, const provider::Response&) {
                      b->models = data.toObject().value(QLatin1String("data")).toArray();
                      settled();
                  },
                  [b, settled](const provider::WireError& e) {
                      b->modelsFailed = true;
                      b->why = e.message;
                      settled();
                  });
    m_api.getJson(QStringLiteral("/videos/models"), this,
                  [b, settled](const QJsonValue& data, const provider::Response&) {
                      b->videos = data.toObject().value(QLatin1String("data")).toArray();
                      settled();
                  },
                  [settled](const provider::WireError&) { settled(); });
    m_api.getJson(QStringLiteral("/images/models"), this,
                  [this, b, settled](const QJsonValue& data, const provider::Response&) {
                      for (const QJsonValue& v : data.toObject().value(QLatin1String("data")).toArray())
                          b->imageQueue << v.toObject().value(QLatin1String("id")).toString();
                      b->listsOutstanding += int(b->imageQueue.size());
                      pumpImages(b);
                      settled();
                  },
                  [settled](const provider::WireError&) { settled(); });
}

// A pool of 8 over the image endpoints (TS fetchImageSpecs); a model whose endpoints fail is skipped.
void CatalogLoader::pumpImages(const std::shared_ptr<Build>& b) {
    while (b->imageInFlight < 8 && !b->imageQueue.isEmpty()) {
        const QString id = b->imageQueue.takeFirst();
        ++b->imageInFlight;
        const auto settled = [this, b]() {
            --b->imageInFlight;
            pumpImages(b);
            if (--b->listsOutstanding == 0) listsDone(b);
        };
        m_api.getJson(QStringLiteral("/images/models/%1/endpoints").arg(id), this,
                      [b, id, settled](const QJsonValue& d, const provider::Response&) {
                          b->specs.insert(id, imageSpecFromEndpoints(d.toObject()));
                          settled();
                      },
                      [settled](const provider::WireError&) { settled(); });
    }
}

BuildInput CatalogLoader::inputOf(const Build& b, qint64 nowMs) const {
    BuildInput in;
    in.models = b.models;
    in.videos = b.videos;
    in.imageSpecs = b.specs;
    in.voiceCloning = b.cloning;
    in.nowSec = double(nowMs) / 1000;
    in.bonus = m_bonus;
    return in;
}

// Every list is in: the speech models' /endpoints decide voice cloning (pool of 6, TS markVoiceCloning), then the
// catalog is built, scored and cached.
void CatalogLoader::listsDone(const std::shared_ptr<Build>& b) {
    if (b->modelsFailed) { fail(b, b->why); return; }
    for (const ModelInfo& m : buildCatalog(inputOf(*b, m_clock.nowMs())))
        if (m.kind == Kind::Speech) b->speechQueue << m.id;
    if (b->speechQueue.isEmpty()) { complete(b); return; }
    pumpSpeech(b);
}

void CatalogLoader::pumpSpeech(const std::shared_ptr<Build>& b) {
    while (b->speechInFlight < 6 && !b->speechQueue.isEmpty()) {
        const QString id = b->speechQueue.takeFirst();
        ++b->speechInFlight;
        const auto settled = [this, b]() {
            --b->speechInFlight;
            if (b->speechQueue.isEmpty() && b->speechInFlight == 0) complete(b);
            else pumpSpeech(b);
        };
        m_api.getJson(QStringLiteral("/models/%1/endpoints").arg(id), this,
                      [b, id, settled](const QJsonValue& d, const provider::Response&) {
                          // /models/{id}/endpoints reports supports_voice_cloning per endpoint; a model counts if any does
                          const QJsonObject o = d.toObject();
                          QJsonArray eps = o.value(QLatin1String("data")).toObject().value(QLatin1String("endpoints")).toArray();
                          if (eps.isEmpty()) eps = o.value(QLatin1String("endpoints")).toArray();
                          for (const QJsonValue& e : eps)
                              if (e.toObject().value(QLatin1String("supports_voice_cloning")).toBool()) { b->cloning.insert(id); break; }
                          settled();
                      },
                      [settled](const provider::WireError&) { settled(); });   // leave unflagged
    }
}

void CatalogLoader::complete(const std::shared_ptr<Build>& b) {
    if (b->finished) return;
    const qint64 now = m_clock.nowMs();
    QList<ModelInfo> models = buildCatalog(inputOf(*b, now));
    if (models.isEmpty() && b->usable) { fail(b, QStringLiteral("the model list came back empty")); return; }
    b->finished = true;
    --m_pending;
    if (m_cache) m_cache->save(CacheEntry{kCacheVersion, now, models});
    Catalog cat = withSources(now, std::move(models));
    deliver(b->hasContext, b->context, [cat = std::move(cat), done = b->done]() { if (done) done(cat); });
}

void CatalogLoader::fail(const std::shared_ptr<Build>& b, QString why) {
    if (b->finished) return;
    b->finished = true;
    --m_pending;
    why = why.simplified().left(200);
    // OpenRouter is down or unreachable: an expired cache beats no catalog at all (prices / new models may be out of date)
    if (b->usable) {
        Catalog cat = withSources(b->cached->fetchedAt, b->cached->models);
        const QString when = QDateTime::fromMSecsSinceEpoch(b->cached->fetchedAt, QTimeZone::UTC).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        cat.setStale(QStringLiteral("the model catalog could not be refreshed from OpenRouter (%1), so a cached copy from %2 UTC is used; "
                                    "new models and price changes since then are missing").arg(why, when));
        log::warn(kLog, cat.stale());
        deliver(b->hasContext, b->context, [cat = std::move(cat), done = b->done]() { if (done) done(cat); });
        return;
    }
    const QString msg = QStringLiteral("The model catalog could not be loaded from OpenRouter (%1) and there is no cached copy yet. This is not a "
                                       "problem with any one model: try again in a minute. Tools that need no model list (memory, budget, jobs, "
                                       "local ranking) still work.").arg(why);
    log::warn(kLog, msg);
    deliver(b->hasContext, b->context, [msg, failed = b->failed]() { if (failed) failed(msg); });
}

} // namespace studio::catalog
