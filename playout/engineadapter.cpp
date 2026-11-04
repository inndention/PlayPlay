#include "engineadapter.h"
#include "supervisorclient.h"
#include <QDebug>
#include <QJsonObject>
#include <QFileInfo>

EngineAdapter::EngineAdapter(const QString& engineId, QObject* parent)
    : QObject(parent)
    , m_engineId(engineId)
{
    m_state.engineId = engineId;
    m_state.status = EngineStatus::Idle;
    m_state.isReady = false;
}

void EngineAdapter::updateState(const EngineState& state)
{
    if (m_state.status != state.status || m_state.currentAssetId != state.currentAssetId) {
        m_state = state;
        emit stateChanged(m_state);
    } else {
        m_state = state;
    }
}

/**
 * SupervisorEngineAdapter — presents one Supervisor channel to the app's
 * ControlService through the EngineAdapter interface. Transport calls become
 * MSG_UI_COMMANDs; observe() is fed from the channel's telemetry.
 */
class SupervisorEngineAdapter : public EngineAdapter {
public:
    SupervisorEngineAdapter(const QString& engineId, const QString& channelId, SupervisorClient* client)
        : EngineAdapter(engineId), m_channelId(channelId), m_client(client)
    {
        QObject::connect(m_client, &SupervisorClient::telemetryChanged, this, [this](const QString& ch) {
            if (ch == m_channelId) updateState(observe());
        });
        QObject::connect(m_client, &SupervisorClient::connectedChanged, this, [this]() { updateState(observe()); });
    }

    bool initialize() override { return true; }
    void shutdown() override {}

    bool loadAsset(const QString& assetId, const QString& assetPath) override {
        // "Take" semantics: play this file now on the channel; the schedule resumes after it.
        m_pendingTake = assetPath.isEmpty() ? assetId : assetPath;
        m_pendingTitle = assetId;
        return !m_pendingTake.isEmpty();
    }
    bool play() override {
        if (!m_pendingTake.isEmpty()) {
            m_client->take(m_pendingTake, QFileInfo(m_pendingTake).completeBaseName(), 0.0, m_channelId);
            m_pendingTake.clear();
        } else {
            m_client->play(m_channelId);
        }
        return true;
    }
    bool pause() override { m_client->pause(m_channelId); return true; }
    bool stop() override { m_client->stop(m_channelId); return true; }
    bool seek(double timeSeconds) override { m_client->seek(timeSeconds, m_channelId); return true; }
    void skip() { m_client->skip(m_channelId); }

    EngineState observe() override {
        EngineState s;
        s.engineId = m_engineId;
        const QJsonObject t = m_client->telemetry(m_channelId);
        s.isReady = m_client->isConnected() && !t.isEmpty();
        const QString st = t.value("state").toString();
        if (!s.isReady) s.status = EngineStatus::Error;
        else if (st == "ON_AIR") s.status = EngineStatus::Playing;
        else if (st == "PAUSED") s.status = EngineStatus::Paused;
        else if (st == "PREROLL") s.status = EngineStatus::Loading;
        else if (st == "ERROR") s.status = EngineStatus::Error;
        else s.status = EngineStatus::Idle;
        s.currentAssetId = t.value("assetId").toString();
        s.position = t.value("position").toDouble();
        s.duration = t.value("duration").toDouble();
        s.metadata = t;
        if (!s.isReady) s.errorMessage = "supervisor offline";
        return s;
    }

private:
    QString m_channelId;
    SupervisorClient* m_client;
    QString m_pendingTake;
    QString m_pendingTitle;
};

static SupervisorClient* g_supervisorClient = nullptr;
void setSupervisorClientForAdapters(SupervisorClient* c) { g_supervisorClient = c; }

EngineAdapter* createEngineAdapter(const QString& engineId, const QString& engineType, const QJsonObject& config)
{
    if (engineType == "supervisor") {
        if (!g_supervisorClient) { qWarning() << "[EngineAdapter] no SupervisorClient set"; return nullptr; }
        return new SupervisorEngineAdapter(engineId, config.value("channelId").toString("main"), g_supervisorClient);
    }
    qWarning() << "Unknown engine type:" << engineType << "(only 'supervisor' is supported; CasparCG/OBS/mpv were removed)";
    return nullptr;
}
