#include "meterticker.h"

#include <QDebug>
#include <QJSEngine>
#include <algorithm>
#include <cmath>

// ---- MeterHold ----

MeterHold::MeterHold(MeterTicker* ticker, QObject* parent) : QObject(parent), m_ticker(ticker) {}

double MeterHold::floorDb() const { return m_ticker ? m_ticker->floorDb() : MeterTicker::Options().floorDb; }

QVariantList MeterHold::hold() const {
    if (!m_cacheValid) {
        m_cache.clear();
        m_cache.reserve(m_values.size());
        for (double v : m_values) m_cache.append(v);
        m_cacheValid = true;
    }
    return m_cache;
}

void MeterHold::changed() {
    m_cacheValid = false;
    if (m_ticker) ++m_ticker->m_stats.holdChanges;
    emit holdChanged();
}

void MeterHold::feed(const QVariantList& peaks) {
    const double floor = floorDb();
    if (m_ticker) ++m_ticker->m_stats.feeds;
    bool moved = false, above = false;
    if (m_values.size() < peaks.size()) { m_values.resize(peaks.size(), floor); moved = true; }   // grows, never shrinks (as before)
    for (int i = 0; i < peaks.size(); ++i) {
        bool ok = false;
        double p = peaks[i].toDouble(&ok);
        if (!ok || !std::isfinite(p) || p < floor) p = floor;   // null used to read as 0 dBFS in the JS version
        if (p > m_values[i]) { m_values[i] = p; moved = true; }
    }
    for (double v : m_values) if (v > floor) { above = true; break; }
    if (moved) changed();
    if (above && m_ticker) m_ticker->wake(this);
}

void MeterHold::clear() {
    const double floor = floorDb();
    bool moved = false;
    for (double& v : m_values) if (v != floor) { v = floor; moved = true; }
    if (moved) changed();
}

bool MeterHold::decay(double stepDb, double floor) {
    bool moved = false, above = false;
    for (double& v : m_values) {
        if (v > floor) { v = std::max(floor, v - stepDb); moved = true; }
        if (v > floor) above = true;
    }
    if (moved) changed();
    return above;
}

// ---- MeterTicker ----

MeterTicker::MeterTicker(QObject* parent) : MeterTicker(Options(), parent) {}

MeterTicker::MeterTicker(const Options& options, QObject* parent) : QObject(parent), m_opt(options) {
    m_opt.intervalMs = std::max(1, m_opt.intervalMs);
    m_timer.setInterval(m_opt.intervalMs);   // coarse (the default): the old QML Timer was only as exact as the animation clock
    connect(&m_timer, &QTimer::timeout, this, &MeterTicker::tick);
    if (const int sec = qEnvironmentVariableIntValue("PLAYPLAY_UI_STATS_SEC"); sec > 0) {   // diagnostics only; off by default
        m_statsLog.setInterval(sec * 1000);
        connect(&m_statsLog, &QTimer::timeout, this, &MeterTicker::logStats);
        m_statsLog.start();
    }
}

QObject* MeterTicker::createHold(QObject* owner) {
    auto* h = new MeterHold(this, owner);
    // An object returned to QML from an invokable would get JavaScript ownership; the owner's child list owns it instead,
    // so the GC can never delete a hold its meter still shows. Without an owner the GC may collect it.
    QJSEngine::setObjectOwnership(h, owner ? QJSEngine::CppOwnership : QJSEngine::JavaScriptOwnership);
    ++m_stats.holdsCreated;
    ++m_stats.holdsLive;
    connect(h, &QObject::destroyed, this, [this]() { --m_stats.holdsLive; });   // pruned from m_active by its QPointer
    return h;
}

void MeterTicker::wake(MeterHold* hold) {
    if (hold->m_decaying) return;
    hold->setDecaying(true);
    m_active.append(hold);
    if (!m_timer.isActive()) {   // never restart a running timer: the next tick stays where it was
        m_timer.start();
        ++m_stats.starts;
        emit runningChanged();
    }
}

void MeterTicker::tick() {
    ++m_stats.ticks;
    // Index loop: a holdChanged handler may feed (append to m_active) or, in theory, delete a hold.
    for (int i = 0; i < m_active.size();) {
        QPointer<MeterHold> h = m_active[i];
        if (!h) { m_active.removeAt(i); continue; }
        const bool above = h->decay(m_opt.decayDb, m_opt.floorDb);
        if (!h) { m_active.removeAt(i); continue; }
        if (!above) { h->setDecaying(false); m_active.removeAt(i); continue; }
        ++i;
    }
    if (m_active.isEmpty() && m_timer.isActive()) {   // every hold is on the floor: idle until the next loud feed
        m_timer.stop();
        ++m_stats.stops;
        emit runningChanged();
    }
}

int MeterTicker::decayingCount() const {
    int n = 0;
    for (const auto& h : m_active) if (h) ++n;
    return n;
}

QVariantMap MeterTicker::stats() const {
    return {
        {"ticks", double(m_stats.ticks)}, {"running", running()}, {"holdsLive", m_stats.holdsLive},
        {"holdsCreated", double(m_stats.holdsCreated)}, {"decaying", decayingCount()}, {"starts", double(m_stats.starts)},
        {"stops", double(m_stats.stops)}, {"holdChanges", double(m_stats.holdChanges)}, {"feeds", double(m_stats.feeds)},
        {"intervalMs", m_opt.intervalMs}, {"decayDb", m_opt.decayDb}, {"floorDb", m_opt.floorDb},
    };
}

void MeterTicker::logStats() {
    const double sec = m_statsLog.interval() / 1000.0;
    auto rate = [sec](quint64 now, quint64 before) { return QString::number((now - before) / sec, 'f', 1); };
    qInfo().noquote() << QStringLiteral("[Meters] ticker %1 ticks/s (%2), holds %3 (%4 decaying), hold changes %5/s, feeds %6/s, starts %7, stops %8")
                             .arg(rate(m_stats.ticks, m_logged.ticks))
                             .arg(running() ? QStringLiteral("running") : QStringLiteral("idle"))
                             .arg(m_stats.holdsLive).arg(decayingCount())
                             .arg(rate(m_stats.holdChanges, m_logged.holdChanges))
                             .arg(rate(m_stats.feeds, m_logged.feeds))
                             .arg(m_stats.starts).arg(m_stats.stops);
    m_logged = m_stats;
}
