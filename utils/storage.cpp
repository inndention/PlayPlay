#include "storage.h"
#include <QJsonDocument>
#include <QJsonArray>

Storage::Storage(QObject* parent)
    : QObject(parent)
    , m_settings(new QSettings(this))
{
}

AutomationSettings Storage::loadSettings() const
{
    QByteArray data = m_settings->value("automationSettings").toByteArray();
    if (data.isEmpty()) {
        return AutomationSettings();
    }
    
    QJsonDocument doc = QJsonDocument::fromJson(data);
    return AutomationSettings::fromJson(doc.object());
}

void Storage::saveSettings(const AutomationSettings& settings)
{
    QJsonDocument doc(settings.toJson());
    m_settings->setValue("automationSettings", doc.toJson());
}

QList<CGElement> Storage::loadCGElements() const
{
    QByteArray data = m_settings->value("cgElements").toByteArray();
    if (data.isEmpty()) {
        return QList<CGElement>();
    }
    
    QJsonDocument doc = QJsonDocument::fromJson(data);
    QJsonArray array = doc.array();
    
    QList<CGElement> elements;
    for (const auto& val : array) {
        elements.append(CGElement::fromJson(val.toObject()));
    }
    
    return elements;
}

void Storage::saveCGElements(const QList<CGElement>& elements)
{
    QJsonArray array;
    for (const auto& element : elements) {
        array.append(element.toJson());
    }
    
    QJsonDocument doc(array);
    m_settings->setValue("cgElements", doc.toJson());
}

QList<AdClient> Storage::loadClients() const
{
    QByteArray data = m_settings->value("clients").toByteArray();
    if (data.isEmpty()) {
        return QList<AdClient>();
    }
    
    QJsonDocument doc = QJsonDocument::fromJson(data);
    QJsonArray array = doc.array();
    
    QList<AdClient> clients;
    for (const auto& val : array) {
        clients.append(AdClient::fromJson(val.toObject()));
    }
    
    return clients;
}

void Storage::saveClients(const QList<AdClient>& clients)
{
    QJsonArray array;
    for (const auto& client : clients) {
        array.append(client.toJson());
    }
    
    QJsonDocument doc(array);
    m_settings->setValue("clients", doc.toJson());
}

