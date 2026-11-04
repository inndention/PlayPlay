#ifndef SERVICESTORAGE_H
#define SERVICESTORAGE_H

#include <QObject>

class ServiceStorage : public QObject
{
    Q_OBJECT

public:
    explicit ServiceStorage(QObject* parent = nullptr);
    
    // TODO: Service-level storage utilities
};

#endif // SERVICESTORAGE_H

