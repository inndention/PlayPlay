#ifndef NDIENGINE_H
#define NDIENGINE_H

#include <QObject>

class NDIEngine : public QObject
{
    Q_OBJECT

public:
    explicit NDIEngine(QObject* parent = nullptr);
    
    // TODO: Implement NDI source discovery and capture
};

#endif // NDIENGINE_H

