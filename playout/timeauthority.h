#ifndef TIMEAUTHORITY_H
#define TIMEAUTHORITY_H

#include <QObject>
#include <QDateTime>
#include <QTimer>
#include <chrono>

class TimeAuthority : public QObject {
    Q_OBJECT
    
public:
    enum Mode { Master, Slave };
    
    explicit TimeAuthority(QObject* parent = nullptr);
    ~TimeAuthority();
    
    void setMode(Mode mode);
    Mode mode() const { return m_mode; }
    
    // Get current wall-clock time (UTC milliseconds since epoch)
    qint64 now() const;
    
    // Get monotonic time (nanoseconds since process start)
    qint64 monotonic() const;
    
    // Convert wall-clock time to frame number
    qint64 timeToFrame(qint64 timestamp) const;
    
    // Convert frame number to wall-clock time
    qint64 frameToTime(int frame) const;
    
    // Get current frame number
    qint64 currentFrame() const;
    
    // Format time as SMPTE timecode (HH:MM:SS:FF)
    QString toSMPTE(qint64 timestamp) const;
    
    // Parse SMPTE timecode to timestamp
    qint64 fromSMPTE(const QString& timecode, const QDateTime& baseDate = QDateTime()) const;
    
    // Set frame rate
    void setFrameRate(int fps);
    int frameRate() const { return m_frameRate; }
    
    // Frame-accurate timing
    int getCurrentFrame() const;
    qint64 getCurrentFrameTime() const; // Time in milliseconds for current frame
    qint64 getFrameTime(int frameNumber) const; // Get time for specific frame number
    int getFrameNumber(qint64 timeMs) const; // Get frame number for specific time
    
    // Frame-accurate calculations
    int framesToMilliseconds(int frames) const;
    int millisecondsToFrames(qint64 milliseconds) const;
    double getFrameDuration() const; // Duration of one frame in milliseconds
    
    // Set NTP offset (for synchronization)
    void setNtpOffset(qint64 offsetMs);
    qint64 ntpOffset() const { return m_ntpOffset; }
    
signals:
    void timeDriftDetected(qint64 driftMs);
    
private slots:
    void checkDrift();
    
private:
    QElapsedTimer m_elapsedTimer; // For monotonic time
    qint64 m_startTime;      // Wall-clock start time
    qint64 m_ntpOffset;      // NTP offset in milliseconds
    int m_frameRate;         // Frames per second
    double m_driftThreshold; // Drift threshold in seconds
    QTimer* m_driftTimer;
    Mode m_mode;             // Master or Slave
    
    std::chrono::steady_clock::time_point m_processStart;
};

// Singleton accessor
TimeAuthority* getTimeAuthority();

#endif // TIMEAUTHORITY_H

