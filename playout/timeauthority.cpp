#include "timeauthority.h"
#include <QElapsedTimer>
#include <QDateTime>
#include <QDebug>
#include <QTime>
#include <QStringList>
#include <QtMath>
#include <atomic>

static TimeAuthority* g_timeAuthority = nullptr;

namespace {
// Drift bookkeeping (W17, STB-5). The header stays as it is (it is not in the stability lane's files), so the state
// rides on the drift timer: only the constructor creates m_driftTimer, always as a DriftTimer.
struct DriftTimer final : QTimer {
    using QTimer::QTimer;
    qint64 baseWallMs = 0, baseMonoNs = 0;   // what the next check compares against; re-based after each correction
    qint64 lastLineMonoNs = 0;
    bool logged = false;
    int pending = 0;                         // corrections since the last line that the rate limit kept out of the log
    qint64 pendingMs = 0;
};
constexpr qint64 kDriftLineEveryNs = 60LL * 1000 * 1000 * 1000;   // at most one drift line a minute
// Test clocks (tests/test_timeauthority.cpp declares the setter itself). Null = the real clocks.
std::atomic<qint64 (*)()> g_testWallMs{nullptr}, g_testMonoNs{nullptr};
qint64 driftWallMs() { auto f = g_testWallMs.load(std::memory_order_relaxed); return f ? f() : QDateTime::currentMSecsSinceEpoch(); }
qint64 driftMonoNs(const QElapsedTimer& t) { auto f = g_testMonoNs.load(std::memory_order_relaxed); return f ? f() : t.nsecsElapsed(); }
} // namespace

namespace timeauthority_testing {
// Both clocks or neither; set before constructing the TimeAuthority under test.
void setClocks(qint64 (*wallMs)(), qint64 (*monoNs)()) { g_testWallMs = wallMs; g_testMonoNs = monoNs; }
}

TimeAuthority::TimeAuthority(QObject* parent)
    : QObject(parent)
    , m_startTime(0)
    , m_ntpOffset(0)
    , m_frameRate(30)
    , m_driftThreshold(0.1)
    , m_driftTimer(new DriftTimer(this))
    , m_mode(Master)
{
    m_elapsedTimer.start();
    m_startTime = QDateTime::currentMSecsSinceEpoch();
    m_processStart = std::chrono::steady_clock::now();
    // The first drift base is the start pair, as before (m_startTime / elapsed 0); later checks move only this base.
    auto* d = static_cast<DriftTimer*>(m_driftTimer);
    d->baseWallMs = g_testWallMs.load() ? driftWallMs() : m_startTime;
    d->baseMonoNs = g_testMonoNs.load() ? driftMonoNs(m_elapsedTimer) : 0;
    connect(m_driftTimer, &QTimer::timeout, this, &TimeAuthority::checkDrift);
    m_driftTimer->start(1000); // Check drift every second
}

void TimeAuthority::setMode(Mode mode)
{
    m_mode = mode;
    qDebug() << "[TimeAuthority] Mode set to:" << (mode == Master ? "Master" : "Slave");
}

TimeAuthority::~TimeAuthority()
{
    // QObject parent will handle cleanup
}

qint64 TimeAuthority::now() const
{
    return QDateTime::currentMSecsSinceEpoch() + m_ntpOffset;
}

qint64 TimeAuthority::monotonic() const
{
    return m_elapsedTimer.nsecsElapsed();
}

qint64 TimeAuthority::timeToFrame(qint64 timestamp) const
{
    qint64 baseTime = m_startTime;
    qint64 elapsed = timestamp - baseTime;
    double seconds = elapsed / 1000.0;
    return static_cast<qint64>(seconds * m_frameRate);
}

qint64 TimeAuthority::frameToTime(int frame) const
{
    double seconds = frame / static_cast<double>(m_frameRate);
    qint64 milliseconds = static_cast<qint64>(seconds * 1000);
    return m_startTime + milliseconds;
}

qint64 TimeAuthority::currentFrame() const
{
    return timeToFrame(now());
}

QString TimeAuthority::toSMPTE(qint64 timestamp) const
{
    qint64 baseTime = m_startTime;
    qint64 elapsed = timestamp - baseTime;
    double totalSeconds = elapsed / 1000.0;
    
    int hours = static_cast<int>(totalSeconds / 3600);
    int minutes = static_cast<int>((totalSeconds - hours * 3600) / 60);
    int seconds = static_cast<int>(totalSeconds - hours * 3600 - minutes * 60);
    int frames = static_cast<int>((totalSeconds - hours * 3600 - minutes * 60 - seconds) * m_frameRate);
    
    return QString("%1:%2:%3:%4")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'))
        .arg(frames, 2, 10, QChar('0'));
}

qint64 TimeAuthority::fromSMPTE(const QString& timecode, const QDateTime& baseDate) const
{
    // Parse HH:MM:SS:FF format
    QStringList parts = timecode.split(':');
    if (parts.size() != 4) {
        return 0;
    }
    
    bool ok;
    int hours = parts[0].toInt(&ok);
    if (!ok) return 0;
    
    int minutes = parts[1].toInt(&ok);
    if (!ok) return 0;
    
    int seconds = parts[2].toInt(&ok);
    if (!ok) return 0;
    
    int frames = parts[3].toInt(&ok);
    if (!ok) return 0;
    
    double totalSeconds = hours * 3600 + minutes * 60 + seconds + (frames / static_cast<double>(m_frameRate));
    qint64 milliseconds = static_cast<qint64>(totalSeconds * 1000);
    
    QDateTime date = baseDate.isValid() ? baseDate : QDateTime::currentDateTimeUtc();
    QTime time = QTime(hours, minutes, seconds);
    date.setTime(time);
    
    return date.toMSecsSinceEpoch() + static_cast<qint64>((frames / static_cast<double>(m_frameRate)) * 1000);
}

void TimeAuthority::setFrameRate(int fps)
{
    m_frameRate = fps;
}

void TimeAuthority::setNtpOffset(qint64 offsetMs)
{
    m_ntpOffset = offsetMs;
}

// These methods are inline in the header, no implementation needed

int TimeAuthority::getCurrentFrame() const
{
    return currentFrame();
}

qint64 TimeAuthority::getCurrentFrameTime() const
{
    int frame = getCurrentFrame();
    return frameToTime(frame);
}

qint64 TimeAuthority::getFrameTime(int frameNumber) const
{
    return frameToTime(frameNumber);
}

int TimeAuthority::getFrameNumber(qint64 timeMs) const
{
    return timeToFrame(timeMs);
}

int TimeAuthority::framesToMilliseconds(int frames) const
{
    double seconds = frames / static_cast<double>(m_frameRate);
    return static_cast<int>(seconds * 1000);
}

int TimeAuthority::millisecondsToFrames(qint64 milliseconds) const
{
    double seconds = milliseconds / 1000.0;
    return static_cast<int>(seconds * m_frameRate);
}

double TimeAuthority::getFrameDuration() const
{
    return 1000.0 / static_cast<double>(m_frameRate);
}

void TimeAuthority::checkDrift()
{
    // Wall clock against monotonic time since the drift base. W17 (STB-5): a correction is reported and then the base moves
    // to now, so one clock step (w32time/NTP, a resume) gives one line. Before, the comparison stayed on the start pair and
    // a single step was logged again every second for ever: 80 421 identical lines per engine on 10-25. Lines are limited
    // to one a minute; corrections in between are counted and summed into the next line (or a summary once the minute is
    // up), so no drift is lost. Smaller drift keeps adding up against the base until it crosses the threshold.
    // m_startTime (frame numbers, SMPTE, time sync) never moves.
    auto* d = static_cast<DriftTimer*>(m_driftTimer);
    const qint64 wall = driftWallMs(), mono = driftMonoNs(m_elapsedTimer);
    const qint64 drift = wall - (d->baseWallMs + (mono - d->baseMonoNs) / 1000000);
    const bool lineDue = !d->logged || mono - d->lastLineMonoNs >= kDriftLineEveryNs;
    const auto wrote = [d, mono]() { d->logged = true; d->lastLineMonoNs = mono; d->pending = 0; d->pendingMs = 0; };

    if (qAbs(drift) > static_cast<qint64>(m_driftThreshold * 1000)) {
        d->baseWallMs = wall; d->baseMonoNs = mono;
        emit timeDriftDetected(drift);
        if (!lineDue) { ++d->pending; d->pendingMs += drift; return; }
        qWarning().noquote() << QString("Time drift detected: %1 ms (re-based%2)").arg(drift)
            .arg(d->pending ? QString("; %1 earlier correction(s), %2 ms in all, since the previous line").arg(d->pending).arg(d->pendingMs) : QString());
        wrote();
    } else if (d->pending && lineDue) {
        qWarning().noquote() << QString("Time drift detected: %1 correction(s), %2 ms in all, since the previous line (re-based each time)")
            .arg(d->pending).arg(d->pendingMs);
        wrote();
    }
}

TimeAuthority* getTimeAuthority()
{
    if (!g_timeAuthority) {
        g_timeAuthority = new TimeAuthority();
    }
    return g_timeAuthority;
}

