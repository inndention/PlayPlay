#ifndef DEKTECENGINE_H
#define DEKTECENGINE_H

#include <QObject>

class DekTecEngine : public QObject
{
    Q_OBJECT

public:
    explicit DekTecEngine(QObject* parent = nullptr);
    
    // TODO: Implement DekTec ASI/SDI output
};

#endif // DEKTECENGINE_H

