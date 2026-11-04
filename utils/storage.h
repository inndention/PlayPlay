#ifndef STORAGE_H
#define STORAGE_H

#include <QObject>
#include <QSettings>
#include "core/types.h"
#include "core/cgelement.h"

class Storage : public QObject
{
    Q_OBJECT

public:
    explicit Storage(QObject* parent = nullptr);
    
    // Settings
    AutomationSettings loadSettings() const;
    void saveSettings(const AutomationSettings& settings);
    
    // CG Elements
    QList<CGElement> loadCGElements() const;
    void saveCGElements(const QList<CGElement>& elements);
    
    // Clients
    QList<AdClient> loadClients() const;
    void saveClients(const QList<AdClient>& clients);

private:
    QSettings* m_settings;
};

#endif // STORAGE_H

