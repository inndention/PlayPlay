#ifndef METERTICKER_H
#define METERTICKER_H

#include <QList>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

class MeterTicker;

/**
 * MeterHold — one meter's peak-hold values (one per audio channel, dBFS), decayed by the shared MeterTicker.
 *
 * Made by MeterTicker::createHold(owner) and owned by that QML item (deleted with it). feed(peaks) raises each hold to a
 * louder peak; the ticker lowers every value above the floor by decayDb per tick. `hold` notifies only when a value moved,
 * so a silent meter costs nothing. Non-numbers (null / NaN) count as the floor, never as 0 dBFS.
 */
class MeterHold : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList hold READ hold NOTIFY holdChanged)
    Q_PROPERTY(bool decaying READ decaying NOTIFY decayingChanged)

public:
    QVariantList hold() const;                         // cached until the next change
    bool decaying() const { return m_decaying; }
    const QVector<double>& values() const { return m_values; }
    Q_INVOKABLE void feed(const QVariantList& peaksDbfs);
    Q_INVOKABLE void clear();                          // every value back to the floor (the list keeps its length)

signals:
    void holdChanged();
    void decayingChanged();

private:
    friend class MeterTicker;
    MeterHold(MeterTicker* ticker, QObject* parent);
    double floorDb() const;
    bool decay(double stepDb, double floorDb);        // true while a value is still above the floor
    void setDecaying(bool on) { if (m_decaying != on) { m_decaying = on; emit decayingChanged(); } }
    void changed();

    QPointer<MeterTicker> m_ticker;
    QVector<double> m_values;
    mutable QVariantList m_cache;
    mutable bool m_cacheValid = false;
    bool m_decaying = false;
};

/**
 * MeterTicker — the one timer behind every VuMeter / AudioStrip peak hold (perf-app PA-5, W18).
 *
 * Each meter ran its own 150 ms QML Timer, always, that copied its hold array, lowered it by 1.5 dB and reassigned it:
 * N meters = N x 6.7 fires/s, plus a re-evaluation of every bar's hold binding per fire, also on a silent or hidden meter.
 * Here one QTimer (same 150 ms / 1.5 dB / -100 dBFS floor, so the markers move exactly as before) decays the holds in C++,
 * and runs only while at least one hold is above the floor: it starts on the first loud feed and stops when the last hold
 * reaches the floor. A feed never restarts a running timer (15 Hz feeds would otherwise postpone the decay for ever).
 *
 * C++ and a context property ("meterTicker", main.cpp) because qml/ui/ has no qmldir for a QML singleton.
 * GUI thread only. PLAYPLAY_UI_STATS_SEC=<n> logs the tick rate every n seconds (diagnostics; off by default).
 */
class MeterTicker : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(int intervalMs READ intervalMs CONSTANT)
    Q_PROPERTY(double decayDb READ decayDb CONSTANT)
    Q_PROPERTY(double floorDb READ floorDb CONSTANT)

public:
    struct Options { int intervalMs = 150; double decayDb = 1.5; double floorDb = -100.0; };
    struct Stats { quint64 ticks = 0, starts = 0, stops = 0, holdsCreated = 0, holdChanges = 0, feeds = 0; int holdsLive = 0; };

    explicit MeterTicker(QObject* parent = nullptr);
    explicit MeterTicker(const Options& options, QObject* parent = nullptr);

    // A new hold for one meter. With an owner (the meter item) it is parented to it and never collected by the JS GC.
    Q_INVOKABLE QObject* createHold(QObject* owner = nullptr);
    // {ticks, running, holdsLive, holdsCreated, decaying, starts, stops, holdChanges, feeds, intervalMs, decayDb, floorDb}
    Q_INVOKABLE QVariantMap stats() const;
    const Stats& counters() const { return m_stats; }
    int decayingCount() const;

    bool running() const { return m_timer.isActive(); }
    int intervalMs() const { return m_opt.intervalMs; }
    double decayDb() const { return m_opt.decayDb; }
    double floorDb() const { return m_opt.floorDb; }

public slots:
    void tick();                                       // the timer's slot; public so tests can step it

signals:
    void runningChanged();

private:
    friend class MeterHold;
    void wake(MeterHold* hold);                        // the hold has a value above the floor: decay it from the next tick
    void logStats();

    Options m_opt;
    QTimer m_timer;
    QTimer m_statsLog;
    QList<QPointer<MeterHold>> m_active;               // holds above the floor, in wake order
    Stats m_stats;
    Stats m_logged;
};

#endif // METERTICKER_H
