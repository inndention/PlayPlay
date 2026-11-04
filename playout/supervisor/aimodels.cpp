#include "aimodels.h"
#include "suitehttp.h"
#include "suitelogic.h"
#include "../schedule/schedulestore.h"

#include <QDateTime>

using namespace sched;

static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
static constexpr qint64 kRefreshMs = 6LL * 3600 * 1000;
static constexpr int kMaxAgeDays = 365;
static const char* kEnvVar = "OPENROUTER_API_KEY";

AiModels::AiModels(ScheduleStore* store, QObject* parent) : QObject(parent), m_store(store), m_http(new SuiteHttp(this)) {
    const QJsonObject c = m_store->settingJson("ai.catalog");
    if (!c.value("models").toArray().isEmpty()) {
        for (const QJsonValue& v : c.value("models").toArray()) if (!suites::isLegacyModel(v.toObject().value("id").toString())) m_catalog.append(v);   // the rules may have tightened since it was saved
        m_fetchedMs = qint64(c.value("fetchedMs").toDouble()); m_source = "cache";
    }
}

QString AiModels::apiKey(const ScheduleStore* store, QString* source) {
    const QString saved = store->settingJson("ai.config").value("apiKey").toString().trimmed();
    if (!saved.isEmpty()) { if (source) *source = "settings"; return saved; }
    const QString env = qEnvironmentVariable(kEnvVar).trimmed();
    if (!env.isEmpty()) { if (source) *source = "env"; return env; }
    if (source) *source = "none";
    return {};
}

QString AiModels::baseUrl(const ScheduleStore* store) {
    QString u = store->settingJson("ai.config").value("baseUrl").toString("https://openrouter.ai/api/v1").trimmed();
    while (u.endsWith('/')) u.chop(1);
    return u.isEmpty() ? QString("https://openrouter.ai/api/v1") : u;
}

QString AiModels::model(const ScheduleStore* store, const QString& featureModel) {
    if (!featureModel.trimmed().isEmpty() && !suites::isLegacyModel(featureModel)) return featureModel.trimmed();
    return suites::resolveAiModel(store->settingJson("ai.config").value("model").toString());
}

QJsonObject AiModels::keyStatus(const ScheduleStore* store) {
    QString src; const bool has = !apiKey(store, &src).isEmpty();
    QJsonObject o; o["hasKey"] = has; o["keySource"] = src; o["envVar"] = kEnvVar; o["envKeyPresent"] = !qEnvironmentVariable(kEnvVar).trimmed().isEmpty();
    o["model"] = model(store); o["defaultModel"] = suites::defaultAiModel(); o["baseUrl"] = baseUrl(store);
    return o;
}

void AiModels::migrate(ScheduleStore* store) {
    QJsonObject ai = store->settingJson("ai.config"); bool aiChanged = false;
    if (suites::isLegacyModel(ai.value("model").toString())) { ai["model"] = suites::defaultAiModel(); aiChanged = true; }
    for (const char* key : {"agents.config", "spots.llm"}) {
        QJsonObject c = store->settingJson(key); bool changed = false;
        if (suites::isLegacyModel(c.value("model").toString())) { c["model"] = suites::defaultAiModel(); changed = true; }
        const QString k = c.value("apiKey").toString().trimmed();      // per-feature keys become the one shared override
        if (!k.isEmpty()) { if (ai.value("apiKey").toString().isEmpty()) { ai["apiKey"] = k; aiChanged = true; } c.remove("apiKey"); changed = true; }
        if (changed) store->setSettingJson(key, c);
    }
    if (aiChanged) store->setSettingJson("ai.config", ai);
}

QJsonObject AiModels::config() const {
    QJsonObject o = keyStatus(m_store);
    o["configuredModel"] = m_store->settingJson("ai.config").value("model").toString();
    return o;
}

QJsonObject AiModels::setConfig(const QJsonObject& patch) {
    QJsonObject c = m_store->settingJson("ai.config");
    if (patch.contains("model")) { const QString m = patch.value("model").toString().trimmed(); if (m.isEmpty()) c.remove("model"); else c["model"] = suites::resolveAiModel(m); }
    if (patch.contains("baseUrl")) c["baseUrl"] = patch.value("baseUrl").toString().trimmed();
    if (!patch.value("apiKey").toString().trimmed().isEmpty()) c["apiKey"] = patch.value("apiKey").toString().trimmed();
    if (patch.value("clearKey").toBool()) c.remove("apiKey");
    m_store->setSettingJson("ai.config", c);
    return config();
}

QJsonObject AiModels::answer(bool all) const {
    QJsonArray list;
    if (all) list = m_catalog;
    else { const qint64 cutoff = nowMs() / 1000 - qint64(kMaxAgeDays) * 86400; for (const QJsonValue& v : m_catalog) if (qint64(v.toObject().value("created").toDouble()) >= cutoff) list.append(v); }
    QJsonObject o; o["models"] = list; o["count"] = list.size(); o["total"] = m_catalog.size(); o["source"] = m_source; o["fetchedMs"] = double(m_fetchedMs);
    o["defaultModel"] = suites::defaultAiModel(); o["maxAgeDays"] = all ? 0 : kMaxAgeDays;
    const QJsonObject k = keyStatus(m_store); o["hasKey"] = k.value("hasKey"); o["keySource"] = k.value("keySource"); o["model"] = k.value("model");
    return o;
}

void AiModels::models(bool all, bool refresh, JsonCb cb) {
    if (!refresh && !m_catalog.isEmpty() && m_source != "builtin" && nowMs() - m_fetchedMs < kRefreshMs) { cb(answer(all)); return; }
    m_waiting.append([this, all, cb]() { cb(answer(all)); });
    if (m_fetching) return;
    m_fetching = true;
    m_http->json("GET", baseUrl(m_store) + "/models", {}, {}, 20000, [this](int http, const QJsonObject& j, const QByteArray&, const QString&) {
        m_fetching = false;
        const QJsonArray data = j.value("data").toArray();
        if (http == 200 && !data.isEmpty()) {
            m_catalog = suites::filterModelCatalog(data, 0, 0); m_fetchedMs = nowMs(); m_source = "live";
            QJsonObject c; c["models"] = m_catalog; c["fetchedMs"] = double(m_fetchedMs); m_store->setSettingJson("ai.catalog", c);
        } else if (m_catalog.isEmpty()) { m_catalog = suites::builtinModelCatalog(); m_source = "builtin"; }
        else m_source = "cache";
        const auto w = m_waiting; m_waiting.clear(); for (const auto& f : w) f();
    });
}
