#ifndef NTPCLIENT_H
#define NTPCLIENT_H

#include <QObject>
#include <QUdpSocket>
#include <QHostAddress>
#include <QDateTime>

/**
 * Simple RFC 4330 (SNTP) client for wall-clock synchronization.
 * Queries a pool of NTP servers and computes offset vs system clock.
 */
class NtpClient : public QObject {
    Q_OBJECT

public:
    explicit NtpClient(QObject* parent = nullptr);

    void setServers(const QStringList& hostnames);
    QStringList servers() const { return m_servers; }

    void query();
    bool isQuerying() const { return m_pendingQueries > 0; }

    qint64 lastOffsetMs() const { return m_lastOffsetMs; }
    qint64 lastRoundTripMs() const { return m_lastRoundTripMs; }
    QDateTime lastSyncTime() const { return m_lastSyncTime; }

signals:
    void syncResult(qint64 offsetMs, qint64 roundTripMs, bool success);
    void syncError(const QString& reason);

private slots:
    void onReadyRead();
    void onTimeout();

private:
    QUdpSocket* m_socket;
    QStringList m_servers;
    int m_currentServerIndex;
    int m_pendingQueries;
    qint64 m_queryStartMs;
    qint64 m_lastOffsetMs;
    qint64 m_lastRoundTripMs;
    QDateTime m_lastSyncTime;

    void sendNtpPacket(const QString& hostname);
    qint64 parseNtpPacket(const QByteArray& data);
};

#endif // NTPCLIENT_H
