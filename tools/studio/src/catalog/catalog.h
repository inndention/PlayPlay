#pragma once
// The model catalog (ST-08; docs/plans/starmedia-studio.md §8): a port of starmedia src/catalog.ts and the ModelInfo /
// ImageSpec / VideoSpec types of src/types.ts. Everything OpenRouter offers is pulled (/models?output_modalities=all,
// /videos/models, /images/models + each image model's /endpoints, each speech model's /endpoints for voice cloning),
// normalised into ModelInfo, scored (quality.h) and cached between runs; extra sources (the local model service, the
// render pool) are merged into every loaded catalog.
//
// Two layers, so the parity tests need no network:
//   - buildCatalog() is pure: the three JSON answers + the image specs + the voice-cloning ids + "now" → models, byte for
//     byte what build() in the TS makes of them (test_studio_catalog checks it against a golden made by the TS code);
//   - CatalogLoader does the fetching through the ST-07 client (pool of 8 for image endpoints, 6 for speech endpoints,
//     as the TS), the cache (TTL 6 h, CACHE_VERSION 5), the stale fallback and the sources, on the event loop (nothing
//     blocks: every step is a callback on the loader's thread).
//
// Numbers stay doubles here, as in the TS: prices are what the catalog publishes and the estimators (pricing.h) must
// give the TS values for the TS inputs. Money is made of them only at the edge (CostEstimate::micro(), ST-11/12).
//
// Decided here (not in the TS): VideoSpec::skus is a QJsonObject, whose keys iterate sorted, so videoRate()'s last-
// resort scan ("any key with duration_seconds / second in it") takes the alphabetically first such key where the TS
// takes the first listed; every direct and token key is looked up by name and unaffected.
#include "core/clock.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace studio::provider { class OpenRouter; }

namespace studio::catalog {

enum class Kind { Text, Image, Video, Speech, Stt, Embed, Rerank };
QString kindName(Kind k);                               // "text", "image", "video", "speech", "stt", "embed", "rerank"
std::optional<Kind> kindFromName(QStringView name);

/// Who serves a model (TS ModelInfo.provider): absent = OpenRouter.
enum class Provider { Unset, OpenRouter, Local, Pool };

struct Price {
    double in = 0;                                      // USD per token (per char / second for tts / stt: see pricing.h)
    double out = 0;
    std::optional<double> webSearch;
    std::optional<double> reasoning;
    std::optional<double> cacheRead;
};

struct ImageSpec {
    std::optional<double> perImage;                     // cheapest USD for one image, when priced per image
    std::optional<double> perMegapixel;
    std::optional<double> perOutputToken;
    std::optional<double> perInputToken;
    QStringList aspects;
    QStringList qualities;
    int maxN = 1;
    int maxRefs = 0;
    bool streaming = false;
};

struct VideoSpec {
    QList<int> durations;
    QStringList resolutions;
    QStringList aspects;
    bool audio = false;
    QStringList frames;
    QJsonObject skus;                                   // raw pricing_skus, $ unless the key starts with cents_
    bool edit = false;                                  // video-to-video edit model (TS: true | undefined)
};

struct ReasoningSpec {
    QStringList efforts;
    bool mandatory = false;
    bool defaultEnabled = false;
};

struct Throughput {
    std::optional<double> tokensPerSec;
    std::optional<double> secondsPerImage;
    std::optional<double> secondsPerVideoSecond;
    std::optional<double> secondsPerSpokenSecond;
    std::optional<double> secondsPerAudioMinute;
};

struct LocalSpec {
    QString backend;                                    // "ollama", "comfyui", "speaches", "pool" ...
    Throughput throughput;
    std::optional<double> vramGb;
    std::optional<bool> verified;
    QString licence;
    QJsonObject extra;                                  // whatever else a source put there (kept for the JSON)
};

struct ModelInfo {
    QString id;
    QString name;
    Kind kind = Kind::Text;
    double created = 0;                                 // unix seconds
    double ctx = 0;
    std::optional<double> maxOut;
    QStringList inMods;
    QStringList outMods;
    Price price;
    bool free = false;
    QStringList params;
    std::optional<ReasoningSpec> reasoning;
    std::optional<QStringList> voices;
    bool voiceCloning = false;                          // TS: true | undefined
    std::optional<double> expires;                      // unix seconds
    std::optional<ImageSpec> image;
    std::optional<VideoSpec> video;
    double quality = 0;                                 // 0..1 (quality.h); not a benchmark
    Provider provider = Provider::Unset;
    std::optional<LocalSpec> local;

    bool hasParam(QStringView p) const { return params.contains(p); }
    bool hasInMod(QStringView m) const { return inMods.contains(m); }
    /// pricing.ts isLocalModel: served by this machine (provider local or a local/* id).
    bool isLocal() const { return provider == Provider::Local || id.startsWith(QLatin1String("local/")); }
    /// id.split('/')[0]
    QString providerPrefix() const { return id.section(u'/', 0, 0); }

    /// The TS object shape (the cache file, the goldens, the API). Absent optionals are left out, as JSON.stringify
    /// drops undefined; `voiceCloning` and `video.edit` appear only when true.
    QJsonObject toJson() const;
    static ModelInfo fromJson(const QJsonObject& o);
};

struct CacheEntry {
    int version = 0;
    qint64 fetchedAt = 0;                               // epoch ms
    QList<ModelInfo> models;
    QJsonObject toJson() const;
    static CacheEntry fromJson(const QJsonObject& o);
};

/// A loaded catalog (TS Catalog). `stale` is set when OpenRouter could not be reached and an expired cache is served.
class Catalog {
public:
    Catalog() = default;
    Catalog(qint64 fetchedAt, QList<ModelInfo> models);    // TS wrap()

    qint64 fetchedAt() const { return m_fetchedAt; }
    const QList<ModelInfo>& models() const { return m_models; }
    bool isEmpty() const { return m_models.isEmpty(); }
    /// The model with this id (the last one when an id repeats, as a JS Map keeps the last set), or null.
    const ModelInfo* byId(QStringView id) const;
    bool has(QStringView id) const { return byId(id) != nullptr; }
    QString stale() const { return m_stale; }
    void setStale(const QString& why) { m_stale = why; }

    /// selector.ts priceRank: within a kind, the paid hosted models sorted by 0.3·in + 0.7·out take 0 … 1 (a lone one
    /// 0); free and local models (and unknown ids) are 0. Computed once per catalog.
    double priceRank(const ModelInfo& m) const;

    /// TS summarize(): kind → {total, free}.
    QJsonObject summary() const;

private:
    qint64 m_fetchedAt = 0;
    QList<ModelInfo> m_models;
    QHash<QString, int> m_index;
    QString m_stale;
    mutable QHash<QString, double> m_priceRank;
    mutable bool m_priceRanked = false;
};

// ---------------- the pure build (TS build()) ----------------

/// TS n(): undefined / null / "" → 0, else Number(x) || 0.
double num(const QJsonValue& v);
/// TS kindOf(output_modalities); nullopt when the model is none of the kinds the harness uses.
std::optional<Kind> kindOf(const QStringList& outputModalities);
/// The ImageSpec of one /images/models/{id}/endpoints answer (TS fetchImageSpecs, the per-model part).
ImageSpec imageSpecFromEndpoints(const QJsonObject& answer);
/// Text models the TS skips as generic workhorses (TEXT_SKIP), batch endpoints, moving aliases.
bool textSkipped(QStringView id);
bool videoUsable(QStringView id);                       // !NOT_USABLE_VIDEO
bool videoEditModel(QStringView id);                    // EDIT_MODEL

struct BuildInput {
    QJsonArray models;                                  // /models?output_modalities=all → data
    QJsonArray videos;                                  // /videos/models → data (empty when that call failed)
    QHash<QString, ImageSpec> imageSpecs;               // per image model id (only these are kept)
    QSet<QString> voiceCloning;                         // speech ids whose /endpoints say supports_voice_cloning
    double nowSec = 0;                                  // for the quality score (TS Date.now() / 1000)
    QList<std::pair<QString, double>> bonus;            // configureCatalog({bonus}): id substring → quality bonus, in order
};
/// Every usable model, normalised and scored: what the TS build() returns for the same answers.
QList<ModelInfo> buildCatalog(const BuildInput& in);

/// TS withSources(): models from the sources replace same ids and are appended (in source order).
QList<ModelInfo> mergeSources(QList<ModelInfo> models, const QList<ModelInfo>& extra);

// ---------------- the loader ----------------

/// Where the built catalog is kept between runs (TS CatalogCache). load() returns nullopt when there is none; both may
/// throw nothing: a failure is a nullopt / a no-op (the cache is best effort).
class CatalogCache {
public:
    virtual ~CatalogCache() = default;
    virtual std::optional<CacheEntry> load() = 0;
    virtual void save(const CacheEntry& entry) = 0;
};

class MemoryCatalogCache final : public CatalogCache {
public:
    std::optional<CacheEntry> load() override { return m_entry; }
    void save(const CacheEntry& entry) override { m_entry = entry; }
    void clear() { m_entry.reset(); }
private:
    std::optional<CacheEntry> m_entry;
};

/// A JSON file (atomic write); an unreadable or malformed file is "no cache".
class FileCatalogCache final : public CatalogCache {
public:
    explicit FileCatalogCache(const QString& path) : m_path(path) {}
    std::optional<CacheEntry> load() override;
    void save(const CacheEntry& entry) override;
    QString path() const { return m_path; }
private:
    QString m_path;
};

constexpr qint64 kCatalogTtlMs = 6 * 3600 * 1000;
constexpr int kCacheVersion = 5;

/// An extra model source (TS CatalogSource): fully built ModelInfo entries (provider, price and quality set). Decided
/// here: synchronous (the local service and the pool keep their lists in memory); `ok` false = the source is down and
/// is skipped (the TS catches a throw), the catalog still loads.
using CatalogSource = std::function<QList<ModelInfo>(bool* ok)>;

class CatalogLoader : public QObject {
    Q_OBJECT
public:
    CatalogLoader(provider::OpenRouter& api, Clock& clock, QObject* parent = nullptr);
    ~CatalogLoader() override;

    /// Not owned; null = no cache. The cache must outlive the loader.
    void setCache(CatalogCache* cache) { m_cache = cache; }
    void setBonus(const QList<std::pair<QString, double>>& bonus) { m_bonus = bonus; }
    void setSource(const QString& name, CatalogSource source);     // an empty source removes the name
    QStringList sourceNames() const { return m_sources.keys(); }

    using Done = std::function<void(const Catalog&)>;
    using Failed = std::function<void(const QString& message)>;
    /// TS loadCatalog({refresh}): a fresh cache answers at once (through the event loop, so `done` never runs inside
    /// load()); otherwise OpenRouter is asked, and when that fails an expired cache is served with `stale` set, else
    /// `failed` gets the TS CatalogError text. Callbacks run on `context`'s thread and are skipped once it is gone (null
    /// = this loader). One load at a time per loader is the expected use; several in flight each get their answer.
    void load(bool refresh, QObject* context, Done done, Failed failed);
    /// Loads in flight.
    int pending() const { return m_pending; }

    struct Build;                                       // one fetch in progress (catalog.cpp)

private:
    void deliver(bool hadContext, const QPointer<QObject>& alive, std::function<void()> fn);
    void pumpImages(const std::shared_ptr<Build>& b);
    void listsDone(const std::shared_ptr<Build>& b);
    void pumpSpeech(const std::shared_ptr<Build>& b);
    void complete(const std::shared_ptr<Build>& b);
    void fail(const std::shared_ptr<Build>& b, QString why);
    BuildInput inputOf(const Build& b, qint64 nowMs) const;
    Catalog withSources(qint64 fetchedAt, QList<ModelInfo> models) const;

    provider::OpenRouter& m_api;
    Clock& m_clock;
    CatalogCache* m_cache = nullptr;
    QList<std::pair<QString, double>> m_bonus;
    QMap<QString, CatalogSource> m_sources;
    int m_pending = 0;
};

} // namespace studio::catalog
