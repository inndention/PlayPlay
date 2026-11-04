// test_studio_catalog (ST-08; docs/plans/starmedia-studio.md §8): the catalog, the quality proxy, the estimators and
// the selector against the values the REAL StarMedia TypeScript produced for the same inputs (fixtures/catalog-
// golden.json, made by tools/studio/scripts/golden-catalog.mjs from src/catalog.ts, src/pricing.ts and src/selector.ts
// run against the TS test mock with the clock pinned to reference_now):
//   - the fixture catalog built from tests/studio/fixtures/models.json equals the TS catalog model by model (kinds,
//     prices, specs, voice cloning, free flags, quality scores);
//   - every estimator case (textCost / imageCost / videoCost / videoRate / ttsCost / sttCost / embedCost / rerankCost,
//     hosted and local, measured and unmeasured) gives the TS usd, confidence, seconds and detail text;
//   - fmtUsd (toFixed semantics), fmtSeconds, approxTokens, chooseEffort, searchRequestUsd, variantLean, speedBias,
//     tierFor and qualityWeight equal the TS;
//   - eligible / pickModel / cheapestModel / pinnedPick / pinIneligible give the TS ids, estimates, fallbacks and
//     reasons on the fixture catalog (with and without local models) and the hand-made catalogs of test/dials.ts and
//     test/variety.ts: budget fit, pins, free-only, exclusions, data collection, expiry, retired models, the
//     Fast ↔ Detailed and Cost ↔ Quality dials, provider-distinct fallbacks, local off / auto / prefer / only and the
//     time budget;
//   - the loader against the mock OpenRouter: the fetch sequence (pools), the cache (fresh / expired / refresh), the
//     stale fallback and the TS error text, an empty list, failing side lists, sources, a dead context;
//   - the TS files still hash as the golden says and (with Node 22.7+ on PATH) a fresh golden equals the committed one.
// Everything runs on ephemeral loopback ports; no key, no network, no spend.
#include <QtTest>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cmath>
#include <functional>
#include <memory>

#include "catalog/catalog.h"
#include "catalog/pricing.h"
#include "catalog/quality.h"
#include "catalog/selector.h"
#include "core/clock.h"
#include "core/config.h"
#include "core/json.h"
#include "core/log.h"
#include "mockopenrouter.h"
#include "provider/gates.h"
#include "provider/modelhealth.h"
#include "provider/openrouter.h"
#include "provider/wire.h"

using namespace studio;
using namespace studio::catalog;
using studio::test::MockOpenRouter;
using studio::test::MockReply;

namespace {

QJsonObject readJson(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

const QJsonObject& golden() {
    static const QJsonObject g = readJson(QStringLiteral(STUDIO_TEST_DIR "/fixtures/catalog-golden.json"));
    return g;
}

const QJsonObject& fixture() {
    static const QJsonObject f = readJson(QStringLiteral(STUDIO_TEST_DIR "/fixtures/models.json"));
    return f;
}

double referenceNow() { return golden().value(u"reference_now").toDouble(); }

// JS numbers that went through JSON: equal within rounding noise of the last places.
bool near(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
    if (std::isinf(a) || std::isinf(b)) return a == b;
    return std::abs(a - b) <= 1e-9 * std::max(1.0, std::abs(b));
}

// Deep comparison of two JSON values with near() on numbers; the first difference goes into `why`.
bool jsonNear(const QJsonValue& a, const QJsonValue& b, const QString& path, QString* why) {
    const auto fail = [&](const QString& what) {
        *why = QStringLiteral("%1: %2 (got %3, want %4)").arg(path, what, QString::fromUtf8(json::compact(a)).left(200), QString::fromUtf8(json::compact(b)).left(200));
        return false;
    };
    if (a.type() != b.type()) return fail(QStringLiteral("type"));
    switch (a.type()) {
    case QJsonValue::Double: return near(a.toDouble(), b.toDouble()) ? true : fail(QStringLiteral("number"));
    case QJsonValue::String: return a.toString() == b.toString() ? true : fail(QStringLiteral("string"));
    case QJsonValue::Bool: return a.toBool() == b.toBool() ? true : fail(QStringLiteral("bool"));
    case QJsonValue::Array: {
        const QJsonArray x = a.toArray(), y = b.toArray();
        if (x.size() != y.size()) return fail(QStringLiteral("array size"));
        for (int i = 0; i < x.size(); ++i)
            if (!jsonNear(x[i], y[i], path + u'[' + QString::number(i) + u']', why)) return false;
        return true;
    }
    case QJsonValue::Object: {
        const QJsonObject x = a.toObject(), y = b.toObject();
        if (x.keys() != y.keys()) return fail(QStringLiteral("keys %1 vs %2").arg(x.keys().join(u','), y.keys().join(u',')));
        for (auto it = x.constBegin(); it != x.constEnd(); ++it)
            if (!jsonNear(it.value(), y.value(it.key()), path + u'.' + it.key(), why)) return false;
        return true;
    }
    default: return true;
    }
}

// The fixture catalog as buildCatalog() makes it from models.json, at reference_now (what the TS built from the mock).
BuildInput fixtureInput() {
    const QJsonObject f = fixture();
    BuildInput in;
    in.models = f.value(u"models").toArray();
    in.videos = f.value(u"video_models").toArray();
    const QJsonObject eps = f.value(u"image_endpoints").toObject();
    for (auto it = eps.constBegin(); it != eps.constEnd(); ++it) in.imageSpecs.insert(it.key(), imageSpecFromEndpoints(it.value().toObject()));
    for (const QJsonValue& v : f.value(u"voice_cloning").toArray()) in.voiceCloning.insert(v.toString());
    in.nowSec = referenceNow();
    return in;
}

QList<ModelInfo> modelsOf(const QJsonArray& a) {
    QList<ModelInfo> out;
    for (const QJsonValue& v : a) out << ModelInfo::fromJson(v.toObject());
    return out;
}

// The catalogs the golden's selector cases name: the fixture, the fixture plus local models, the hand-made lists.
class Catalogs {
public:
    Catalogs() {
        QList<ModelInfo> fx = buildCatalog(fixtureInput());
        m_cats.emplace(QStringLiteral("fixture"), std::make_unique<Catalog>(qint64(referenceNow() * 1000), fx));
        fx += modelsOf(golden().value(u"fixtureLocalExtra").toArray());
        m_cats.emplace(QStringLiteral("fixtureLocal"), std::make_unique<Catalog>(qint64(referenceNow() * 1000), fx));
        const QJsonObject hand = golden().value(u"catalogs").toObject();
        for (auto it = hand.constBegin(); it != hand.constEnd(); ++it)
            m_cats.emplace(it.key(), std::make_unique<Catalog>(qint64(referenceNow() * 1000), modelsOf(it.value().toArray())));
    }
    const Catalog* get(const QString& name) const { const auto it = m_cats.find(name); return it == m_cats.end() ? nullptr : it->second.get(); }
private:
    std::map<QString, std::unique_ptr<Catalog>> m_cats;
};

const Catalogs& catalogs() {
    static const Catalogs c;
    return c;
}

ModelInfo handModel(const QString& name) { return ModelInfo::fromJson(golden().value(u"hand").toObject().value(name).toObject()); }

// A golden model reference: {id} = the fixture catalog's model, {hand} = a hand-made one.
std::optional<ModelInfo> modelOf(const QJsonObject& ref) {
    if (ref.contains(u"id")) {
        const ModelInfo* m = catalogs().get(QStringLiteral("fixture"))->byId(ref.value(u"id").toString());
        return m ? std::optional<ModelInfo>(*m) : std::nullopt;
    }
    return handModel(ref.value(u"hand").toString());
}

TextOptions textOptions(const QJsonObject& o) {
    TextOptions t;
    t.effort = o.value(u"effort").toString();
    t.searches = o.value(u"searches").toInt();
    if (o.contains(u"searchResults")) t.searchResults = o.value(u"searchResults").toInt();
    t.reasoningIncluded = o.value(u"reasoningIncluded").toBool();
    return t;
}

// The estimator a golden case names (the same table as golden-catalog.mjs est()).
Estimator estimatorOf(const QJsonObject& spec) {
    const QString kind = spec.value(u"kind").toString();
    if (kind == u"flat") return [usd = spec.value(u"usd").toDouble()](const ModelInfo&) { CostEstimate e; e.usd = usd; return e; };
    if (kind == u"priced")
        return [map = spec.value(u"map").toObject(), other = spec.value(u"other")](const ModelInfo& m) {
            CostEstimate e;
            e.usd = map.contains(m.id) ? map.value(m.id).toDouble() : other.isDouble() ? other.toDouble() : 1;
            return e;
        };
    if (kind == u"text")
        return [in = spec.value(u"in").toInteger(), out = spec.value(u"out").toInteger(), o = textOptions(spec.value(u"opts").toObject())](const ModelInfo& m) {
            return textCost(m, in, out, o);
        };
    if (kind == u"image") return [n = spec.value(u"n").toInt(), q = spec.value(u"quality").toString()](const ModelInfo& m) { return imageCost(m, {n, q}); };
    if (kind == u"video")
        return [s = spec.value(u"seconds").toDouble(), r = spec.value(u"resolution").toString(), a = spec.value(u"audio").toBool()](const ModelInfo& m) {
            return videoCost(m, {s, r, a});
        };
    if (kind == u"tts") return [c = spec.value(u"chars").toInteger()](const ModelInfo& m) { return ttsCost(m, c); };
    if (kind == u"stt") return [s = spec.value(u"seconds").toDouble()](const ModelInfo& m) { return sttCost(m, s); };
    if (kind == u"embed") return [t = spec.value(u"tokens").toInteger()](const ModelInfo& m) { return embedCost(m, t); };
    if (kind == u"rerank") return [](const ModelInfo& m) { return rerankCost(m); };
    return {};
}

QJsonValue pickJson(const std::optional<Pick>& p) {
    if (!p) return QJsonValue();
    return QJsonObject{{QStringLiteral("model"), p->model.id}, {QStringLiteral("estimate"), p->estimate.toJson()},
                       {QStringLiteral("fallbacks"), QJsonArray::fromStringList(p->fallbacks)}, {QStringLiteral("pinned"), p->pinned}};
}

QString describeCase(const QJsonObject& c) { return QString::fromUtf8(json::compact(c)).left(600); }

// One provider stack on the real clock against the mock (as test_studio_openrouter's Client).
struct Client {
    explicit Client(Config& cfg, const QString& base, Clock& clock = Clock::system()) : health(clock), gates(clock), wire(cfg, gates, clock), api(wire) {
        provider::WireOptions o;
        o.baseUrl = base;
        o.backoffBaseMs = 20;
        o.backoffJitterMs = 0;
        o.busyWaitMs = 20;
        o.rand = []() { return 0.0; };
        wire.setOptions(o);
        wire.setHealth(&health);
        gates.setHealth(&health);
    }
    provider::ModelHealth health;
    provider::Gates gates;
    provider::Wire wire;
    provider::OpenRouter api;
};

QByteArray lfText(QByteArray b) { return b.replace("\r\n", "\n"); }
QByteArray fileBytes(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

class TestStudioCatalog : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void names();
    void fixtureCatalogEqualsTs();
    void modelInfoJsonRoundTrip();
    void qualityProxy();
    void pricingGoldens();
    void miscGoldens();
    void selectorGoldens();
    void selectorRules();
    void runOptionsJson();
    void loaderThroughMock();
    void loaderFailures();
    void goldenMatchesStarmedia();
    void cleanupTestCase() { log::stop(); }
};

void TestStudioCatalog::initTestCase() {
    QVERIFY2(!golden().isEmpty(), "fixtures/catalog-golden.json missing or unreadable");
    QVERIFY2(!fixture().isEmpty(), "fixtures/models.json missing or unreadable");
    QCOMPARE(fixture().value(u"reference_now").toDouble(), referenceNow());
}

void TestStudioCatalog::names() {
    for (Kind k : {Kind::Text, Kind::Image, Kind::Video, Kind::Speech, Kind::Stt, Kind::Embed, Kind::Rerank}) QCOMPARE(kindFromName(kindName(k)), std::optional<Kind>(k));
    QVERIFY(!kindFromName(u"audio"));
    for (Role r : allRoles()) QCOMPARE(roleFromName(roleName(r)), std::optional<Role>(r));
    QCOMPARE(allRoles().size(), 17);
    QCOMPARE(roleName(Role::VideoI2V), QStringLiteral("videoI2V"));
    QCOMPARE(roleName(Role::TtsClone), QStringLiteral("ttsClone"));
    QCOMPARE(kindOf({QStringLiteral("text"), QStringLiteral("image")}), std::optional<Kind>(Kind::Image));
    QCOMPARE(kindOf({QStringLiteral("text")}), std::optional<Kind>(Kind::Text));
    QCOMPARE(kindOf({QStringLiteral("video"), QStringLiteral("speech")}), std::optional<Kind>(Kind::Speech));
    QVERIFY(!kindOf({QStringLiteral("3d")}));
    QVERIFY(!kindOf({}));
    QVERIFY(textSkipped(u"openai/gpt-5-codex"));
    QVERIFY(textSkipped(u"qwen/qwen3-coder"));
    QVERIFY(textSkipped(u"acme/x:thinking"));
    QVERIFY(!textSkipped(u"acme/encoder"));                 // \bcoder\b: "encoder" is not the word
    QVERIFY(!videoUsable(u"acme/vid-upscale"));
    QVERIFY(videoEditModel(u"runway/aleph"));
    QCOMPARE(mediumOf(Kind::Stt), Medium::Audio);
    QCOMPARE(mediumOf(Kind::Embed), Medium::Text);
    QCOMPARE(num(QJsonValue(QStringLiteral(" 1e-7 "))), 1e-7);
    QCOMPARE(num(QJsonValue(QStringLiteral(""))), 0.0);
    QCOMPARE(num(QJsonValue(QStringLiteral("abc"))), 0.0);
    QCOMPARE(num(QJsonValue()), 0.0);
    QCOMPARE(num(QJsonValue(0.5)), 0.5);
}

void TestStudioCatalog::fixtureCatalogEqualsTs() {
    const QList<ModelInfo> models = buildCatalog(fixtureInput());
    const QJsonArray want = golden().value(u"catalog").toObject().value(u"models").toArray();
    QCOMPARE(models.size(), want.size());
    for (int i = 0; i < models.size(); ++i) {
        QString why;
        QVERIFY2(jsonNear(models[i].toJson(), want[i], models[i].id, &why), qPrintable(why));
    }
    const Catalog cat(qint64(referenceNow() * 1000), models);
    QString why;
    QVERIFY2(jsonNear(cat.summary(), golden().value(u"catalog").toObject().value(u"summary"), QStringLiteral("summary"), &why), qPrintable(why));
    QVERIFY(cat.has(u"acme/vid-1"));
    QVERIFY(!cat.has(u"nope/x"));
    QVERIFY(cat.byId(u"acme/tts-clone")->voiceCloning);
    QVERIFY(!cat.byId(u"acme/tts-1")->voiceCloning);
    QVERIFY(cat.byId(u"beta/img-free")->free);
    QVERIFY(!cat.byId(u"acme/img-1")->free);
    QVERIFY(cat.byId(u"delta/free-model:free")->free);
    QCOMPARE(cat.byId(u"acme/img-1")->image->perImage, std::optional<double>(0.04));
    QCOMPARE(cat.byId(u"acme/img-1")->image->maxRefs, 4);

    // The filters: batch endpoints, moving aliases, text models the harness skips, image models without an Image API
    // entry, video models without a /videos/models entry or without durations, and the unusable-video words.
    BuildInput in = fixtureInput();
    const auto text = [](const QString& id) {
        return QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("name"), id},
                           {QStringLiteral("architecture"), QJsonObject{{QStringLiteral("output_modalities"), QJsonArray{QStringLiteral("text")}}}},
                           {QStringLiteral("pricing"), QJsonObject{{QStringLiteral("prompt"), QStringLiteral("0.000001")}, {QStringLiteral("completion"), QStringLiteral("0.000002")}}}};
    };
    in.models.append(text(QStringLiteral("acme/x:batch")));
    in.models.append(text(QStringLiteral("~acme/alias")));
    in.models.append(text(QStringLiteral("acme/x-codex")));
    in.models.append(text(QStringLiteral("acme/kept")));
    QJsonObject img = text(QStringLiteral("acme/img-noapi"));
    img.insert(QStringLiteral("architecture"), QJsonObject{{QStringLiteral("output_modalities"), QJsonArray{QStringLiteral("image")}}});
    in.models.append(img);
    QJsonObject vid = text(QStringLiteral("acme/vid-nolist"));
    vid.insert(QStringLiteral("architecture"), QJsonObject{{QStringLiteral("output_modalities"), QJsonArray{QStringLiteral("video")}}});
    in.models.append(vid);
    QJsonObject vidUp = vid;
    vidUp.insert(QStringLiteral("id"), QStringLiteral("acme/vid-upscale"));
    in.models.append(vidUp);
    in.videos.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("acme/vid-upscale")}, {QStringLiteral("supported_durations"), QJsonArray{4}},
                                 {QStringLiteral("supported_resolutions"), QJsonArray{QStringLiteral("720p")}}});
    QJsonObject vidEdit = vid;
    vidEdit.insert(QStringLiteral("id"), QStringLiteral("acme/vid-edit"));
    in.models.append(vidEdit);
    in.videos.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("acme/vid-edit")}});   // no durations: fine for an edit model
    QJsonObject vidBare = vid;
    vidBare.insert(QStringLiteral("id"), QStringLiteral("acme/vid-bare"));
    in.models.append(vidBare);
    in.videos.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("acme/vid-bare")}});
    QJsonObject expiring = text(QStringLiteral("acme/expiring"));
    expiring.insert(QStringLiteral("expiration_date"), QStringLiteral("2026-11-01"));
    expiring.insert(QStringLiteral("pricing"), QJsonObject{{QStringLiteral("prompt"), QStringLiteral("0")}, {QStringLiteral("completion"), QStringLiteral("0")},
                                                           {QStringLiteral("web_search"), QStringLiteral("0.01")}, {QStringLiteral("input_cache_read"), QStringLiteral("0")}});
    in.models.append(expiring);
    const QList<ModelInfo> more = buildCatalog(in);
    QStringList ids;
    for (const ModelInfo& m : more) ids << m.id;
    QVERIFY(ids.contains(QStringLiteral("acme/kept")));
    QVERIFY(ids.contains(QStringLiteral("acme/vid-edit")));
    QVERIFY(ids.contains(QStringLiteral("acme/expiring")));
    for (const QString& gone : {QStringLiteral("acme/x:batch"), QStringLiteral("~acme/alias"), QStringLiteral("acme/x-codex"), QStringLiteral("acme/img-noapi"),
                                QStringLiteral("acme/vid-nolist"), QStringLiteral("acme/vid-upscale"), QStringLiteral("acme/vid-bare")})
        QVERIFY2(!ids.contains(gone), qPrintable(gone));
    const Catalog c2(0, more);
    QVERIFY(c2.byId(u"acme/vid-edit")->video->edit);
    QVERIFY(!c2.byId(u"acme/vid-edit")->free);
    const ModelInfo* ex = c2.byId(u"acme/expiring");
    QCOMPARE(ex->expires, std::optional<double>(1793491200.0));   // 2026-11-01T00:00:00Z
    QVERIFY(ex->free);                                            // $0 text
    QCOMPARE(ex->price.webSearch, std::optional<double>(0.01));
    QCOMPARE(ex->price.cacheRead, std::optional<double>(0.0));    // "0" is truthy in JS: present, 0
    QVERIFY(c2.byId(u"acme/kept")->quality > 0 && c2.byId(u"acme/kept")->quality <= 1);

    // mergeSources: same ids replaced, extras appended in order
    ModelInfo local = handModel(QStringLiteral("localText"));
    ModelInfo replace = *cat.byId(u"acme/writer-pro");
    replace.name = QStringLiteral("replaced");
    const QList<ModelInfo> merged = mergeSources(models, {replace, local});
    QCOMPARE(merged.size(), models.size() + 1);
    QCOMPARE(merged.last().id, QStringLiteral("local/qwen"));
    QCOMPARE(merged[merged.size() - 2].name, QStringLiteral("replaced"));
    QVERIFY(merged.first().id != QStringLiteral("acme/writer-pro"));
}

void TestStudioCatalog::modelInfoJsonRoundTrip() {
    const QJsonObject hand = golden().value(u"hand").toObject();
    QVERIFY(hand.size() > 30);
    for (auto it = hand.constBegin(); it != hand.constEnd(); ++it) {
        QString why;
        QVERIFY2(jsonNear(ModelInfo::fromJson(it.value().toObject()).toJson(), it.value(), it.key(), &why), qPrintable(why));
    }
    for (const QJsonValue& v : golden().value(u"catalog").toObject().value(u"models").toArray()) {
        QString why;
        QVERIFY2(jsonNear(ModelInfo::fromJson(v.toObject()).toJson(), v, v.toObject().value(u"id").toString(), &why), qPrintable(why));
    }
    QVERIFY(handModel(QStringLiteral("localText")).isLocal());
    QCOMPARE(handModel(QStringLiteral("localText")).provider, Provider::Local);
    QVERIFY(!handModel(QStringLiteral("nativeSearch")).isLocal());
    ModelInfo byPrefix;
    byPrefix.id = QStringLiteral("local/x");
    QVERIFY(byPrefix.isLocal());
    QCOMPARE(byPrefix.providerPrefix(), QStringLiteral("local"));
    // the cache entry round-trips too
    CacheEntry e{kCacheVersion, 1234, buildCatalog(fixtureInput())};
    const CacheEntry back = CacheEntry::fromJson(e.toJson());
    QCOMPARE(back.version, kCacheVersion);
    QCOMPARE(back.fetchedAt, qint64(1234));
    QCOMPARE(back.models.size(), e.models.size());
    for (int i = 0; i < e.models.size(); ++i) {
        QString why;
        QVERIFY2(jsonNear(back.models[i].toJson(), e.models[i].toJson(), e.models[i].id, &why), qPrintable(why));
    }
}

void TestStudioCatalog::qualityProxy() {
    auto rank = percentileRank({3, 1, 2});
    QCOMPARE(rank(1), 0.0);
    QCOMPARE(rank(2), 0.5);
    QCOMPARE(rank(3), 1.0);
    QCOMPARE(rank(2.5), 1.0);        // two below → 2 / 2
    QCOMPARE(rank(0), 0.0);
    QCOMPARE(percentileRank({7})(7), 0.5);
    QCOMPARE(percentileRank({})(1), 0.5);
    const Catalog* fx = catalogs().get(QStringLiteral("fixture"));
    QCOMPARE(representativePrice(*fx->byId(u"acme/writer-pro")), 0.3 * 3e-6 + 0.7 * 1.5e-5);
    QCOMPARE(representativePrice(*fx->byId(u"acme/img-1")), 0.04);
    QCOMPARE(representativePrice(*fx->byId(u"acme/vid-1")), 0.1);   // the 720p rate
    QCOMPARE(representativePrice(*fx->byId(u"acme/tts-1")), 0.000015);
    QCOMPARE(representativePrice(handModel(QStringLiteral("imgMp"))), 0.03);
    QCOMPARE(representativePrice(handModel(QStringLiteral("imgOutTok"))), 0.00004 * 1500);
    // a bonus adds to the ids that contain the pattern, clamped to 1
    BuildInput in = fixtureInput();
    in.bonus = {{QStringLiteral("writer"), 0.5}, {QStringLiteral("zeta"), -2}};
    const Catalog bonus(0, buildCatalog(in));
    QCOMPARE(bonus.byId(u"acme/writer-pro")->quality, 1.0);
    QCOMPARE(bonus.byId(u"zeta/lite")->quality, 0.0);
    QCOMPARE(bonus.byId(u"epsilon/deep")->quality, fx->byId(u"epsilon/deep")->quality);
    // scoreOne equals the batch score; the free model sits at price 0.05; older models score lower
    const QList<ModelInfo>& ms = fx->models();
    for (const ModelInfo& m : ms) QVERIFY(near(scoreOne(m, ms, referenceNow()), m.quality));
    ModelInfo old = *fx->byId(u"acme/writer-pro");
    old.created -= 365 * 86400;
    QVERIFY(scoreOne(old, ms, referenceNow()) < old.quality);
    // the price rank of the selector: paid hosted models of a kind spread over 0..1, free and local ones 0
    QCOMPARE(fx->priceRank(*fx->byId(u"zeta/lite")), 0.0);
    QCOMPARE(fx->priceRank(*fx->byId(u"epsilon/deep")), 1.0);
    QCOMPARE(fx->priceRank(*fx->byId(u"gamma/fast-mini")), 0.25);
    QCOMPARE(fx->priceRank(*fx->byId(u"delta/free-model:free")), 0.0);
    QCOMPARE(fx->priceRank(*fx->byId(u"acme/vid-1")), 0.0);         // alone in its kind
    QCOMPARE(catalogs().get(QStringLiteral("fixtureLocal"))->priceRank(handModel(QStringLiteral("localText"))), 0.0);
}

void TestStudioCatalog::pricingGoldens() {
    const QJsonArray cases = golden().value(u"pricing").toArray();
    QVERIFY(cases.size() > 200);
    int checked = 0;
    for (const QJsonValue& cv : cases) {
        const QJsonObject c = cv.toObject();
        const QString fn = c.value(u"fn").toString();
        const auto m = modelOf(c.value(u"model").toObject());
        QVERIFY2(m.has_value(), qPrintable(describeCase(c)));
        const QJsonArray args = c.value(u"args").toArray();
        QJsonValue got;
        if (fn == u"textCost") got = textCost(*m, args[0].toInteger(), args[1].toInteger(), textOptions(args[2].toObject())).toJson();
        else if (fn == u"imageCost") got = imageCost(*m, {args[0].toObject().value(u"n").toInt(), args[0].toObject().value(u"quality").toString()}).toJson();
        else if (fn == u"videoCost") {
            const QJsonObject o = args[0].toObject();
            got = videoCost(*m, {o.value(u"seconds").toDouble(), o.value(u"resolution").toString(), o.value(u"audio").toBool()}).toJson();
        } else if (fn == u"videoRate") {
            const auto r = videoRate(*m, args[0].toString(), args[1].toBool());
            if (r) {
                QJsonObject o{{QStringLiteral("perSec"), r->perSec}, {QStringLiteral("approx"), r->approx}, {QStringLiteral("key"), r->key}};
                if (r->minTotal) o.insert(QStringLiteral("minTotal"), *r->minTotal);
                got = o;
            }
        } else if (fn == u"ttsCost") got = ttsCost(*m, args[0].toInteger()).toJson();
        else if (fn == u"sttCost") got = sttCost(*m, args[0].toDouble()).toJson();
        else if (fn == u"embedCost") got = embedCost(*m, args[0].toInteger()).toJson();
        else if (fn == u"rerankCost") got = rerankCost(*m).toJson();
        else QFAIL(qPrintable(QStringLiteral("unknown fn ") + fn));
        QString why;
        QVERIFY2(jsonNear(got, c.value(u"result"), fn + u' ' + m->id, &why), qPrintable(why + u"\n  case " + describeCase(c)));
        ++checked;
    }
    QCOMPARE(checked, cases.size());
    // money at the edge: an estimate books as micro-USD, an unknown price never does
    QCOMPARE(textCost(*modelOf(QJsonObject{{QStringLiteral("id"), QStringLiteral("acme/writer-pro")}}), 1000, 500).micro(), std::optional<money::Micro>(10500));
    QVERIFY(!videoCost(handModel(QStringLiteral("vidNone")), {4, QStringLiteral("720p"), false}).micro());
    QVERIFY(!videoCost(handModel(QStringLiteral("vidNone")), {4, QStringLiteral("720p"), false}).finite());
    QCOMPARE(sumUsd({textCost(handModel(QStringLiteral("nativeUnlisted")), 1000, 0), videoCost(handModel(QStringLiteral("vidNone")), {4, QStringLiteral("720p"), false})}), 0.003);
    QCOMPARE(sumSeconds({ttsCost(handModel(QStringLiteral("localTts")), 150), zeroEstimate()}), 9.0);
    QCOMPARE(zeroEstimate().detail, QStringLiteral("local"));
}

void TestStudioCatalog::miscGoldens() {
    const QJsonObject misc = golden().value(u"misc").toObject();
    for (const QJsonValue& v : misc.value(u"fmtUsd").toArray())
        QVERIFY2(fmtUsd(v[0].toDouble()) == v[1].toString(), qPrintable(QStringLiteral("fmtUsd(%1): got %2 want %3").arg(json::jsNumber(v[0].toDouble()), fmtUsd(v[0].toDouble()), v[1].toString())));
    for (const QJsonValue& v : misc.value(u"fmtSeconds").toArray())
        QVERIFY2(fmtSeconds(v[0].toDouble()) == v[1].toString(), qPrintable(QStringLiteral("fmtSeconds(%1): got %2").arg(v[0].toDouble()).arg(fmtSeconds(v[0].toDouble()))));
    QCOMPARE(fmtSeconds(std::numeric_limits<double>::infinity()), misc.value(u"fmtSecondsSpecial").toObject().value(u"inf").toString());
    QCOMPARE(fmtSeconds(std::nan("")), misc.value(u"fmtSecondsSpecial").toObject().value(u"nan").toString());
    for (const QJsonValue& v : misc.value(u"approxTokens").toArray()) QCOMPARE(approxTokens(v[0].toInteger()), v[1].toInteger());
    // toFixed semantics on their own: exact ties round up, the binary value decides otherwise
    QCOMPARE(toFixedJs(0.125, 2), QStringLiteral("0.13"));
    QCOMPARE(toFixedJs(0.375, 2), QStringLiteral("0.38"));
    QCOMPARE(toFixedJs(1.005, 2), QStringLiteral("1.00"));
    QCOMPARE(toFixedJs(2.5, 0), QStringLiteral("3"));
    QCOMPARE(toFixedJs(9.995, 2), QStringLiteral("9.99"));
    QCOMPARE(toFixedJs(99.995, 2), QStringLiteral("100.00"));     // 99.995 is just above the tie
    QCOMPARE(toFixedJs(0, 4), QStringLiteral("0.0000"));
    QCOMPARE(toFixedJs(-1.5, 0), QStringLiteral("-2"));
    QCOMPARE(toFixedJs(-0.0001, 2), QStringLiteral("0.00"));      // JS: "-0.00"
    QCOMPARE(groupedInt(1234567), QStringLiteral("1,234,567"));
    QCOMPARE(groupedInt(999), QStringLiteral("999"));
    for (const QJsonValue& v : misc.value(u"chooseEffort").toArray()) {
        const auto m = modelOf(v[0].toObject());
        QVERIFY(m.has_value());
        const QString got = chooseEffort(*m, v[1].toString());
        const QString want = v[2].isNull() ? QString() : v[2].toString();
        QVERIFY2(got == want, qPrintable(QStringLiteral("chooseEffort(%1, %2): got %3 want %4").arg(m->id, v[1].toString(), got, want)));
    }
    QCOMPARE(reasoningFactor(u"xhigh"), std::optional<double>(4.5));
    QVERIFY(!reasoningFactor(u"weird"));
    for (const QJsonValue& v : misc.value(u"searchRequestUsd").toArray()) {
        const auto m = modelOf(v[0].toObject());
        QVERIFY(m.has_value());
        const double got = v[1].isNull() ? searchRequestUsd(*m) : searchRequestUsd(*m, v[1].toInt());
        QVERIFY2(near(got, v[2].toDouble()), qPrintable(QStringLiteral("searchRequestUsd(%1, %2): got %3 want %4").arg(m->id).arg(v[1].toInt()).arg(got).arg(v[2].toDouble())));
    }
    for (const QJsonValue& v : misc.value(u"variantLean").toArray())
        QVERIFY2(variantLean(v[0].toString(), v[1].toString()) == v[2].toInt(),
                 qPrintable(QStringLiteral("variantLean(%1, %2): got %3 want %4").arg(v[0].toString(), v[1].toString()).arg(variantLean(v[0].toString(), v[1].toString())).arg(v[2].toInt())));
    const auto kindArg = [](const QJsonValue& v) { return *kindFromName(v.toString()); };
    for (const QJsonValue& v : misc.value(u"speedBias").toArray()) {
        const RunOptions o = RunOptions::fromJson(v[0].toObject());
        // the dial models of the golden: look them up among the hand catalogs by id
        std::optional<ModelInfo> m;
        for (const QString& name : {QStringLiteral("dials"), QStringLiteral("writers"), QStringLiteral("tts")})
            if (const ModelInfo* x = catalogs().get(name)->byId(v[1].toString())) m = *x;
        if (!m) {
            ModelInfo synth;                                   // acme/vid-1 and acme/img-max: only id / name / kind matter
            synth.id = v[1].toString();
            synth.name = synth.id;
            synth.kind = synth.id.contains(QLatin1String("img")) ? Kind::Image : Kind::Video;
            m = synth;
        }
        QVERIFY2(near(speedBias(o, *m), v[2].toDouble()), qPrintable(QStringLiteral("speedBias(%1, %2): got %3 want %4").arg(QString::fromUtf8(json::compact(v[0])), m->id).arg(speedBias(o, *m)).arg(v[2].toDouble())));
    }
    for (const QJsonValue& v : misc.value(u"tierFor").toArray())
        QVERIFY2(near(tierFor(RunOptions::fromJson(v[0].toObject()), kindArg(v[1])), v[2].toDouble()), qPrintable(QString::fromUtf8(json::compact(v))));
    for (const QJsonValue& v : misc.value(u"qualityWeight").toArray())
        QVERIFY2(near(qualityWeight(RunOptions::fromJson(v[0].toObject()), kindArg(v[1])), v[2].toDouble()), qPrintable(QString::fromUtf8(json::compact(v))));
}

void TestStudioCatalog::selectorGoldens() {
    const QJsonArray cases = golden().value(u"selector").toArray();
    QVERIFY(cases.size() > 2000);
    QMap<QString, int> counts;
    for (const QJsonValue& cv : cases) {
        const QJsonObject c = cv.toObject();
        const QString fn = c.value(u"fn").toString();
        PickCtx ctx;
        ctx.catalog = catalogs().get(c.value(u"catalog").toString());
        QVERIFY2(ctx.catalog, qPrintable(describeCase(c)));
        ctx.opts = RunOptions::fromJson(c.value(u"opts").toObject());
        ctx.freeOnly = c.value(u"freeOnly").toBool();
        ctx.nowSec = referenceNow();
        const QJsonArray args = c.value(u"args").toArray();
        const auto role = roleFromName(args[0].toString());
        QVERIFY2(role.has_value(), qPrintable(describeCase(c)));
        QJsonValue got;
        if (fn == u"eligible") {
            QStringList ids;
            for (const ModelInfo* m : eligible(*role, ctx, {args[1].toObject().value(u"json").toBool()})) ids << m->id;
            got = QJsonArray::fromStringList(ids);
        } else if (fn == u"pickModel") {
            const QJsonObject po = args[3].toObject();
            PickOptions o;
            o.json = po.value(u"json").toBool();
            for (const QJsonValue& e : po.value(u"exclude").toArray()) o.exclude.insert(e.toString());
            if (po.contains(u"distinctProviderFrom")) {
                QSet<QString> d;
                for (const QJsonValue& e : po.value(u"distinctProviderFrom").toArray()) d.insert(e.toString());
                o.distinctProviderFrom = d;
            }
            if (po.contains(u"secondsLeft")) o.secondsLeft = po.value(u"secondsLeft").toDouble();
            got = pickJson(pickModel(*role, ctx, args[1].toDouble(), estimatorOf(args[2].toObject()), o));
        } else if (fn == u"cheapestModel") {
            const QJsonObject co = args[2].toObject();
            CheapestOptions o;
            o.json = co.value(u"json").toBool();
            if (co.contains(u"secondsLeft")) o.secondsLeft = co.value(u"secondsLeft").toDouble();
            got = pickJson(cheapestModel(*role, ctx, estimatorOf(args[1].toObject()), o));
        } else if (fn == u"pinnedPick") {
            QStringList fb;
            for (const QJsonValue& e : args[3].toArray()) fb << e.toString();
            const PinnedResult r = pinnedPick(*role, ctx, args[1].toString(), estimatorOf(args[2].toObject()), fb);
            got = QJsonObject{{QStringLiteral("pick"), pickJson(r.pick)}, {QStringLiteral("reason"), r.reason.isEmpty() ? QJsonValue() : QJsonValue(r.reason)}};
        } else if (fn == u"pinIneligible") {
            const QString why = pinIneligible(*role, ctx, ctx.catalog->byId(args[1].toString()), args[1].toString());
            got = why.isEmpty() ? QJsonValue() : QJsonValue(why);
        } else {
            QFAIL(qPrintable(QStringLiteral("unknown fn ") + fn));
        }
        QString why;
        QVERIFY2(jsonNear(got, c.value(u"result"), fn, &why), qPrintable(why + u"\n  case " + describeCase(c)));
        counts[fn]++;
    }
    QVERIFY(counts.value(QStringLiteral("eligible")) > 500);
    QVERIFY(counts.value(QStringLiteral("pickModel")) > 1000);
    QVERIFY(counts.value(QStringLiteral("cheapestModel")) > 500);
    QVERIFY(counts.value(QStringLiteral("pinnedPick")) > 200);
    QVERIFY(counts.value(QStringLiteral("pinIneligible")) > 200);
}

// The rules of the slice, spelled out (the goldens above hold the exact values; these say what they mean).
void TestStudioCatalog::selectorRules() {
    PickCtx ctx;
    ctx.catalog = catalogs().get(QStringLiteral("fixtureLocal"));
    ctx.nowSec = referenceNow();
    ctx.opts.cost = 60;
    const Estimator writerEst = [](const ModelInfo& m) { return textCost(m, 3000, 1500); };
    const auto ids = [](const QList<const ModelInfo*>& l) { QStringList out; for (const ModelInfo* m : l) out << m->id; return out; };

    // budget fit: the pick is the best model whose estimate fits the allowance; a smaller allowance moves down the
    // price range (and the quality proxy), never above it
    auto best = pickModel(Role::Writer, ctx, 10, writerEst);
    QVERIFY(best.has_value());
    QVERIFY(!best->pinned && !best->model.isLocal());
    QVERIFY(best->estimate.usd <= 10);
    QCOMPARE(best->fallbacks.size(), 2);
    for (const QString& f : best->fallbacks) QVERIFY(f.section(u'/', 0, 0) != best->model.providerPrefix());   // other providers only
    auto cheaper = pickModel(Role::Writer, ctx, 0.01, writerEst);
    QVERIFY(cheaper.has_value());
    QVERIFY(cheaper->estimate.usd <= 0.01);
    QVERIFY(cheaper->model.id != best->model.id && cheaper->model.quality < best->model.quality);
    // under the default local policy ("auto") a local model costs $0 and fits any allowance, so it is what is left
    // when no hosted model fits; with local off there is no pick at all
    auto tiny = pickModel(Role::Writer, ctx, 0.0001, writerEst);
    QVERIFY(tiny.has_value());
    QVERIFY(tiny->model.isLocal());
    QCOMPARE(tiny->estimate.usd, 0.0);
    QVERIFY(tiny->estimate.seconds.value_or(0) > 0);
    QCOMPARE(tiny->model.id, QStringLiteral("local/qwen-fast"));        // the same quality, the quicker one wins the tie
    ctx.opts.local = LocalMode::Off;
    QVERIFY(!pickModel(Role::Writer, ctx, 0.0001, writerEst));
    for (const QString& id : ids(eligible(Role::Writer, ctx))) QVERIFY(!id.startsWith(QLatin1String("local/")));
    // "prefer": a local model outranks every hosted one, with hosted fallbacks; the time budget decides whether it fits
    ctx.opts.local = LocalMode::Prefer;
    auto pref = pickModel(Role::Writer, ctx, 10, writerEst);
    QVERIFY(pref.has_value() && pref->model.isLocal());
    QCOMPARE(pref->fallbacks.size(), 2);
    for (const QString& f : pref->fallbacks) QVERIFY(!f.startsWith(QLatin1String("local/")));
    PickOptions tight;
    tight.secondsLeft = 1;
    auto hosted = pickModel(Role::Writer, ctx, 10, writerEst, tight);
    QVERIFY(hosted.has_value() && !hosted->model.isLocal());
    QCOMPARE(hosted->model.id, best->model.id);
    // "only": hosted models are never eligible
    ctx.opts.local = LocalMode::Only;
    for (const QString& id : ids(eligible(Role::Writer, ctx))) QVERIFY(id.startsWith(QLatin1String("local/")));
    QVERIFY(ids(eligible(Role::Video, ctx)).isEmpty());                // no local video model in this catalog
    auto only = pickModel(Role::Writer, ctx, 10, writerEst);
    QVERIFY(only.has_value() && only->model.isLocal() && only->fallbacks.isEmpty());
    // cheapestModel: the cheapest in time first; over the time budget, a local model is passed over for a hosted one
    ctx.opts.local = LocalMode::Auto;
    auto cheapest = cheapestModel(Role::Writer, ctx, writerEst);
    QVERIFY(cheapest.has_value() && cheapest->model.isLocal());        // $0 beats every hosted price
    CheapestOptions co;
    co.secondsLeft = 1;
    auto cheapestHosted = cheapestModel(Role::Writer, ctx, writerEst, co);
    QVERIFY(cheapestHosted.has_value() && !cheapestHosted->model.isLocal());
    QCOMPARE(cheapestHosted->model.id, QStringLiteral("zeta/lite"));
    ctx.opts.local = LocalMode::Only;
    co.secondsLeft = 0.001;
    QVERIFY(cheapestModel(Role::Writer, ctx, writerEst, co).has_value());   // nothing else can do it: still returned
    ctx.opts.local = LocalMode::Auto;

    // pins: the pinned model is taken as is (no allowance, no eligibility check); a pin of the wrong kind or of an
    // unknown id is ignored and the normal pick happens
    ctx.opts.pins.insert(Role::Writer, QStringLiteral("zeta/lite"));
    auto pinned = pickModel(Role::Writer, ctx, 0.0000001, writerEst);
    QVERIFY(pinned.has_value() && pinned->pinned);
    QCOMPARE(pinned->model.id, QStringLiteral("zeta/lite"));
    QVERIFY(pinned->fallbacks.isEmpty());
    ctx.opts.pins.insert(Role::Writer, QStringLiteral("acme/img-1"));
    QCOMPARE(pickModel(Role::Writer, ctx, 10, writerEst)->model.id, best->model.id);
    ctx.opts.pins.insert(Role::Writer, QStringLiteral("nope/x"));
    QCOMPARE(pickModel(Role::Writer, ctx, 10, writerEst)->model.id, best->model.id);
    ctx.opts.pins.clear();
    // a hand-picked model for one stage (pinnedPick) is checked: kind, retired, expiring, excluded, free-only, data
    // collection, suitability; it never gets a substitute
    QVERIFY(pinnedPick(Role::Writer, ctx, u"zeta/lite", writerEst).pick.has_value());
    QVERIFY(pinnedPick(Role::Writer, ctx, u"acme/img-1", writerEst).reason.contains(QLatin1String("is a image model; this step needs a text model")));
    QVERIFY(pinnedPick(Role::Writer, ctx, u"nope/x", writerEst).reason.contains(QLatin1String("not in the model catalog")));
    QVERIFY(pinnedPick(Role::TtsClone, ctx, u"acme/tts-1", [](const ModelInfo& m) { return ttsCost(m, 100); }).reason.contains(QLatin1String("cannot do this step")));
    {
        const Catalog unpriced(0, {handModel(QStringLiteral("vidNone"))});   // no pricing_skus at all
        PickCtx uctx = ctx;
        uctx.catalog = &unpriced;
        QVERIFY(pinnedPick(Role::Video, uctx, u"acme/vid-none", [](const ModelInfo& m) { return videoCost(m, {4, QStringLiteral("720p"), false}); }).reason
                    .contains(QLatin1String("no price is known for acme/vid-none")));
    }
    auto withFallbacks = pinnedPick(Role::Writer, ctx, u"zeta/lite", writerEst, {QStringLiteral("zeta/lite"), QStringLiteral("acme/img-1"), QStringLiteral("beta/writer-2"), QStringLiteral("beta/writer-2")});
    QCOMPARE(withFallbacks.pick->fallbacks, QStringList{QStringLiteral("beta/writer-2")});

    // free-only (the cost dial at 0): only free and local models are eligible; a paid hand-picked model is refused
    ctx.freeOnly = true;
    for (const ModelInfo* m : eligible(Role::Writer, ctx)) QVERIFY(m->free || m->isLocal());
    QVERIFY(ids(eligible(Role::Writer, ctx)).contains(QStringLiteral("delta/free-model:free")));
    QVERIFY(pinnedPick(Role::Writer, ctx, u"zeta/lite", writerEst).reason.contains(QLatin1String("the cost dial is at 0")));
    QVERIFY(pinnedPick(Role::Writer, ctx, u"delta/free-model:free", writerEst).pick.has_value());
    ctx.freeOnly = false;
    // free models need allowFree; denied data collection takes them away again; exclusions match substrings
    QVERIFY(!ids(eligible(Role::Writer, ctx)).contains(QStringLiteral("delta/free-model:free")));
    ctx.opts.allowFree = true;
    QVERIFY(ids(eligible(Role::Writer, ctx)).contains(QStringLiteral("delta/free-model:free")));
    ctx.opts.denyDataCollection = true;
    QVERIFY(!ids(eligible(Role::Writer, ctx)).contains(QStringLiteral("delta/free-model:free")));
    QVERIFY(pinnedPick(Role::Writer, ctx, u"delta/free-model:free", writerEst).reason.contains(QLatin1String("keep prompts")));
    ctx.opts.denyDataCollection = false;
    ctx.opts.exclude = {QStringLiteral("writer")};
    for (const QString& id : ids(eligible(Role::Writer, ctx))) QVERIFY(!id.contains(QLatin1String("writer")));
    QVERIFY(pinnedPick(Role::Writer, ctx, u"acme/writer-pro", writerEst).reason.contains(QLatin1String("excluded in the settings")));
    ctx.opts.exclude.clear();
    // per-role requirements and the price-rank cut: "fast" takes only the cheaper 55 % of the text range
    const QStringList fast = ids(eligible(Role::Fast, ctx));
    QVERIFY(!fast.contains(QStringLiteral("epsilon/deep")) && !fast.contains(QStringLiteral("acme/writer-pro")));
    QVERIFY(fast.contains(QStringLiteral("zeta/lite")) && fast.contains(QStringLiteral("gamma/fast-mini")));
    QVERIFY(ids(eligible(Role::Judge, ctx)).contains(QStringLiteral("epsilon/deep")));          // reasoning + 64k
    QVERIFY(!ids(eligible(Role::Judge, ctx)).contains(QStringLiteral("gamma/fast-mini")));      // no reasoning
    QVERIFY(!ids(eligible(Role::Tts, ctx)).contains(QStringLiteral("acme/audio-gen")));        // token-priced, no voices
    QCOMPARE(ids(eligible(Role::TtsClone, ctx)), QStringList{QStringLiteral("acme/tts-clone")});
    QCOMPARE(ids(eligible(Role::VideoI2V, ctx)), QStringList{QStringLiteral("acme/vid-1")});
    QVERIFY(ids(eligible(Role::VideoEdit, ctx)).isEmpty());
    // a second provider when asked: the best model of another provider than the ones named
    PickOptions distinct;
    distinct.distinctProviderFrom = QSet<QString>{best->model.providerPrefix()};
    auto other = pickModel(Role::Writer, ctx, 10, writerEst, distinct);
    QVERIFY(other.has_value());
    QVERIFY(other->model.providerPrefix() != best->model.providerPrefix());
    // a pick's estimate is the estimator's (money for hosted, seconds for local)
    QVERIFY(!best->estimate.seconds.has_value());
    QVERIFY(best->estimate.micro().has_value());
}

void TestStudioCatalog::runOptionsJson() {
    const QJsonObject o = QJsonDocument::fromJson(R"({"sliders":{"cost":70,"complexity":20},"allowFree":true,"denyDataCollection":true,
        "pins":{"writer":"acme/writer-pro","nope":"x","image":""},"exclude":["a","b"],"local":"prefer","tiers":{"video":10,"text":"x","odd":5},"speed":{}})").object();
    const RunOptions r = RunOptions::fromJson(o);
    QCOMPARE(r.cost, 70.0);
    QCOMPARE(r.complexity, 20.0);
    QVERIFY(r.allowFree && r.denyDataCollection);
    QCOMPARE(r.pins.size(), 1);
    QCOMPARE(r.pins.value(Role::Writer), QStringLiteral("acme/writer-pro"));
    QCOMPARE(r.exclude, (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
    QCOMPARE(r.local, LocalMode::Prefer);
    QVERIFY(r.tiers && r.tiers->size() == 1 && r.tiers->value(Medium::Video) == 10);
    QVERIFY(r.speed && r.speed->isEmpty());
    QCOMPARE(tierFor(r, Kind::Video), 0.1);
    QCOMPARE(tierFor(r, Kind::Text), 0.7);                     // no text tier: the cost slider
    QVERIFY(!speedFor(r, Kind::Video));
    const RunOptions back = RunOptions::fromJson(r.toJson());
    QCOMPARE(back.toJson(), r.toJson());
    const RunOptions d = RunOptions::fromJson({});
    QCOMPARE(d.cost, 50.0);
    QCOMPARE(d.local, LocalMode::Auto);
    QVERIFY(!d.tiers && !d.speed);
    QCOMPARE(d.toJson().keys(), QStringList{QStringLiteral("sliders")});
}

void TestStudioCatalog::loaderThroughMock() {
    MockOpenRouter mock;
    QVERIFY2(mock.start(), qPrintable(mock.errorString()));
    Config cfg;
    Client cl(cfg, mock.apiUrl());
    FakeClock clock(qint64(referenceNow() * 1000));
    CatalogLoader loader(cl.api, clock);
    MemoryCatalogCache cache;
    loader.setCache(&cache);

    std::optional<Catalog> got;
    QString failed;
    const auto load = [&](bool refresh) {
        got.reset();
        failed.clear();
        loader.load(refresh, &loader, [&got](const Catalog& c) { got = c; }, [&failed](const QString& m) { failed = m; });
        QVERIFY(QTest::qWaitFor([&]() { return got.has_value() || !failed.isEmpty(); }, 10000));
        QCOMPARE(loader.pending(), 0);
    };

    // 1. a fresh fetch: the TS fetch sequence, every model as the golden has it (quality and created are computed
    //    from the mock's own clock, so those are compared within a hair)
    load(true);
    QVERIFY2(failed.isEmpty(), qPrintable(failed));
    QCOMPARE(mock.count(QStringLiteral("/models")), 1);
    QCOMPARE(mock.last(QStringLiteral("/models"))->queryValue(QStringLiteral("output_modalities")), QStringLiteral("all"));
    QCOMPARE(mock.count(QStringLiteral("/videos/models")), 1);
    QCOMPARE(mock.count(QStringLiteral("/images/models")), 1);
    QCOMPARE(mock.count(QStringLiteral("/images/models/*")), 2);
    QCOMPARE(mock.count(QStringLiteral("/models/*")), 4);       // the four speech models
    const QJsonArray want = golden().value(u"catalog").toObject().value(u"models").toArray();
    QCOMPARE(got->models().size(), want.size());
    QVERIFY(got->stale().isEmpty());
    QCOMPARE(got->fetchedAt(), clock.nowMs());
    for (int i = 0; i < want.size(); ++i) {
        QJsonObject a = got->models()[i].toJson(), b = want[i].toObject();
        QVERIFY(std::abs(a.value(u"quality").toDouble() - b.value(u"quality").toDouble()) < 2e-3);
        QVERIFY(std::abs(a.value(u"created").toDouble() - b.value(u"created").toDouble()) < 2 * 86400);
        for (const QString& k : {QStringLiteral("quality"), QStringLiteral("created")}) { a.remove(k); b.remove(k); }
        QString why;
        QVERIFY2(jsonNear(a, b, a.value(u"id").toString(), &why), qPrintable(why));
    }
    QVERIFY(cache.load().has_value());
    QCOMPARE(cache.load()->version, kCacheVersion);

    // 2. within the TTL the cache answers and nothing is fetched; after it, or with refresh, OpenRouter is asked again
    mock.resetStats();
    clock.advance(kCatalogTtlMs - 1);
    load(false);
    QVERIFY(got.has_value());
    QCOMPARE(mock.count(), 0);
    QCOMPARE(got->models().size(), want.size());
    clock.advance(2);
    load(false);
    QCOMPARE(mock.count(QStringLiteral("/models")), 1);
    mock.resetStats();
    load(true);
    QCOMPARE(mock.count(QStringLiteral("/models")), 1);

    // 3. sources: merged into every answer (cached or fetched), same ids replaced, a source that is down skipped
    ModelInfo local = handModel(QStringLiteral("localText"));
    ModelInfo replace = *got->byId(u"acme/writer-pro");
    replace.name = QStringLiteral("pool copy");
    replace.provider = Provider::Pool;
    loader.setSource(QStringLiteral("local"), [local, replace](bool*) { return QList<ModelInfo>{local, replace}; });
    loader.setSource(QStringLiteral("down"), [](bool* ok) { *ok = false; return QList<ModelInfo>{}; });
    mock.resetStats();
    load(false);
    QCOMPARE(mock.count(), 0);
    QCOMPARE(got->models().size(), want.size() + 1);
    QCOMPARE(got->byId(u"acme/writer-pro")->name, QStringLiteral("pool copy"));
    QCOMPARE(got->byId(u"local/qwen")->quality, 0.38);
    QCOMPARE(cache.load()->models.size(), want.size());        // sources are never cached
    loader.setSource(QStringLiteral("local"), {});
    loader.setSource(QStringLiteral("down"), {});
    QVERIFY(loader.sourceNames().isEmpty());

    // 4. side lists failing: no video / image models, the catalog still loads
    mock.failPath(QStringLiteral("/videos/models"));
    mock.failPath(QStringLiteral("/images/models"));
    load(true);
    QVERIFY2(failed.isEmpty(), qPrintable(failed));
    QVERIFY(!got->has(u"acme/vid-1"));
    QVERIFY(!got->has(u"acme/img-1"));
    QVERIFY(got->has(u"acme/writer-pro"));
    mock.clearScripts();
    // an image model whose endpoints fail is skipped, the other kept
    mock.failPath(QStringLiteral("/images/models/acme/img-1/endpoints"));
    load(true);
    QVERIFY(!got->has(u"acme/img-1"));
    QVERIFY(got->has(u"beta/img-free"));
    mock.clearScripts();
    // a speech model whose endpoints fail is left unflagged
    mock.failPath(QStringLiteral("/models/acme/tts-clone/endpoints"));
    load(true);
    QVERIFY(!got->byId(u"acme/tts-clone")->voiceCloning);
    QVERIFY(got->byId(u"acme/audio-gen")->voiceCloning);
    mock.clearScripts();

    // 5. a context that dies before the answer: nothing is delivered, nothing crashes
    {
        auto ctx = std::make_unique<QObject>();
        bool called = false;
        loader.load(true, ctx.get(), [&called](const Catalog&) { called = true; }, [&called](const QString&) { called = true; });
        ctx.reset();
        QVERIFY(QTest::qWaitFor([&loader]() { return loader.pending() == 0; }, 10000));
        QTest::qWait(50);
        QVERIFY(!called);
    }
    mock.stop();
}

void TestStudioCatalog::loaderFailures() {
    MockOpenRouter mock;
    QVERIFY2(mock.start(), qPrintable(mock.errorString()));
    Config cfg;
    Client cl(cfg, mock.apiUrl());
    FakeClock clock(qint64(referenceNow() * 1000));
    CatalogLoader loader(cl.api, clock);
    MemoryCatalogCache cache;
    loader.setCache(&cache);
    std::optional<Catalog> got;
    QString failed;
    const auto load = [&](bool refresh) {
        got.reset();
        failed.clear();
        loader.load(refresh, &loader, [&got](const Catalog& c) { got = c; }, [&failed](const QString& m) { failed = m; });
        QVERIFY(QTest::qWaitFor([&]() { return got.has_value() || !failed.isEmpty(); }, 10000));
    };

    // no cache and OpenRouter down: the TS CatalogError text
    mock.failPath(QStringLiteral("/models"));
    load(true);
    QVERIFY(!got.has_value());
    QVERIFY2(failed.startsWith(QLatin1String("The model catalog could not be loaded from OpenRouter (")), qPrintable(failed));
    QVERIFY(failed.contains(QLatin1String("there is no cached copy yet")));
    QVERIFY(failed.contains(QLatin1String("local ranking) still work.")));
    QVERIFY(!cache.load().has_value());
    mock.clearScripts();

    // a good load fills the cache; then, expired and with OpenRouter down, the stale copy is served with the reason
    load(true);
    QVERIFY2(failed.isEmpty(), qPrintable(failed));
    const qint64 fetched = got->fetchedAt();
    clock.advance(2 * kCatalogTtlMs);
    mock.failPath(QStringLiteral("/models"));
    load(false);
    QVERIFY(got.has_value());
    QVERIFY2(got->stale().startsWith(QLatin1String("the model catalog could not be refreshed from OpenRouter (")), qPrintable(got->stale()));
    QVERIFY2(got->stale().contains(QLatin1String("so a cached copy from 2026-10-26 00:00 UTC is used")), qPrintable(got->stale()));
    QCOMPARE(got->fetchedAt(), fetched);
    QCOMPARE(got->models().size(), 16);
    mock.clearScripts();

    // an empty model list with a usable cache is treated as a failure (stale copy), without one it is an empty catalog
    mock.script(QStringLiteral("GET /models"), MockReply::json(QJsonObject{{QStringLiteral("data"), QJsonArray()}}), -1);
    load(true);
    QVERIFY(got.has_value());
    QVERIFY2(got->stale().contains(QLatin1String("(the model list came back empty)")), qPrintable(got->stale()));
    QCOMPARE(got->models().size(), 16);
    cache.clear();
    load(true);
    QVERIFY(got.has_value() && got->isEmpty() && got->stale().isEmpty());
    mock.clearScripts();

    // a cache of another version or without models is not usable
    CacheEntry old = CacheEntry{kCacheVersion - 1, clock.nowMs(), buildCatalog(fixtureInput())};
    cache.save(old);
    mock.resetStats();
    load(false);
    QCOMPARE(mock.count(QStringLiteral("/models")), 1);
    QCOMPARE(cache.load()->version, kCacheVersion);
    cache.save(CacheEntry{kCacheVersion, clock.nowMs(), {}});
    mock.resetStats();
    load(false);
    QCOMPARE(mock.count(QStringLiteral("/models")), 1);

    // the file cache: written atomically, read back, junk is "no cache"
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FileCatalogCache file(dir.filePath(QStringLiteral("catalog.json")));
    QVERIFY(!file.load().has_value());
    file.save(CacheEntry{kCacheVersion, 77, buildCatalog(fixtureInput())});
    QVERIFY(file.load().has_value());
    QCOMPARE(file.load()->fetchedAt, qint64(77));
    QCOMPARE(file.load()->models.size(), 16);
    {
        QFile f(file.path());
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("{not json");
    }
    QVERIFY(!file.load().has_value());
    mock.stop();
}

void TestStudioCatalog::goldenMatchesStarmedia() {
    const QString root = qEnvironmentVariable("STARMEDIA_ROOT");
    if (root.isEmpty() || !QFileInfo::exists(root + u"/src/selector.ts")) QSKIP("no starmedia checkout (STARMEDIA_ROOT): the golden was not compared");
    QStringList drift;
    for (const QJsonValue& f : golden().value(u"source").toObject().value(u"files").toArray()) {
        const QString rel = f[u"path"].toString();
        const QByteArray text = lfText(fileBytes(root + u'/' + rel));
        if (text.isEmpty() || json::sha256Hex(text) != f[u"sha256"].toString().toLatin1()) drift << rel;
    }
    QVERIFY2(drift.isEmpty(), qPrintable(QStringLiteral("starmedia changed since the golden was made (%1): re-run node --experimental-transform-types "
                                                        "tools/studio/scripts/golden-catalog.mjs and check the test").arg(drift.join(u", "))));
    const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
    QProcess version;
    if (!node.isEmpty()) {
        version.start(node, {QStringLiteral("--version")});
        version.waitForFinished(15000);
    }
    const QStringList v = QString::fromLatin1(version.readAllStandardOutput()).trimmed().mid(1).split(u'.');
    const int major = v.value(0).toInt(), minor = v.value(1).toInt();
    if (node.isEmpty() || major < 22 || (major == 22 && minor < 7)) {
        qWarning("no Node 22.7+ on PATH: the regeneration was not run (the source hashes matched)");
        return;
    }
    QTemporaryDir out;
    QVERIFY(out.isValid());
    QProcess p;
    auto reap = qScopeGuard([&p]() { if (p.state() != QProcess::NotRunning) { p.kill(); p.waitForFinished(5000); } });   // our own child only
    const QString fresh = out.filePath(QStringLiteral("catalog-golden.json"));
    p.start(node, {QStringLiteral("--experimental-transform-types"), QStringLiteral("--no-warnings"), QStringLiteral(STUDIO_SOURCE_DIR "/scripts/golden-catalog.mjs"), root, fresh});
    QVERIFY2(p.waitForStarted(15000), qPrintable(p.errorString()));
    QVERIFY2(p.waitForFinished(90000), "the generator did not finish within 90 s");
    QVERIFY2(p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0,
             qPrintable(QStringLiteral("the generator failed (exit %1):\n%2").arg(p.exitCode()).arg(QString::fromUtf8(p.readAllStandardError().right(1500)))));
    const QByteArray a = lfText(fileBytes(fresh)), b = lfText(fileBytes(QStringLiteral(STUDIO_TEST_DIR "/fixtures/catalog-golden.json")));
    QVERIFY(!a.isEmpty());
    QVERIFY2(a == b, "catalog-golden.json differs from a fresh generation (re-run the generator and commit it)");
}

QTEST_GUILESS_MAIN(TestStudioCatalog)
#include "test_studio_catalog.moc"
