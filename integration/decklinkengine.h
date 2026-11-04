#ifndef DECKLINKENGINE_H
#define DECKLINKENGINE_H

#include <QObject>

class DeckLinkEngine : public QObject
{
    Q_OBJECT

public:
    explicit DeckLinkEngine(QObject* parent = nullptr);
    
    // TODO: Implement DeckLink video I/O
};

#endif // DECKLINKENGINE_H

