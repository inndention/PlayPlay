#include "ntpclient.h"
#include <QDateTime>
#include <QTimer>
#include <QtEndian>
#include <cmath>

NtpClient::NtpClient(QObject* parent)
    : QObject(parent)
    , m_socket(new QUdpSocket(this))
    , m_currentServerIndex(0)
    , m_pendingQueries(0)
    , m_queryStartMs(0)
    , m_lastOffsetMs(0)
    , m_lastRoundTripMs(0)
{
    connect(m_socket, &QUdpSocket::readyRead, this, &NtpClient::onReadyRead);
    m_servers << "pool.ntp.org" << "time.windows.com" << "time.google.com";
}

void NtpClient::setServers(const QStringList& hostnames) {
    m_servers = hostnames;
    m_currentServerIndex = 0;
}

void NtpClient::query() {
    if (m_pendingQueries > 0) return;
    if (m_servers.isEmpty()) {
        emit syncError("No NTP servers configured");
        return;
    }
    m_pendingQueries = 0;
    m_currentServerIndex = 0;
    sendNtpPacket(m_servers.first());
}

void NtpClient::sendNtpPacket(const QString& hostname) {
    QHostAddress addr;
    if (!addr.setAddress(hostname)) {
        // Try DNS lookup (simplified: just try next)
        if (m_currentServerIndex + 1 < m_servers.size()) {
            m_currentServerIndex++;
            sendNtpPacket(m_servers.at(m_currentServerIndex));
        } else {
            emit syncError("Could not resolve any NTP server");
        }
        return;
    }

    QByteArray packet(48, 0);
    packet[0] = 0x1B; // LI=0, VN=3, Mode=3 (client)
    packet[1] = 0;     // Stratum
    packet[2] = 6;     // Poll interval
    packet[3] = 0xEC;  // Precision

    // Reference timestamp
    quint32 reftime = QDateTime::currentDateTime().toSecsSinceEpoch() + 2208988800U;
    qToBigEndian(reftime, reinterpret_cast<uchar*>(packet.data() + 16));

    m_socket->writeDatagram(packet, addr, 123);
    m_queryStartMs = QDateTime::currentDateTime().toMSecsSinceEpoch();
    m_pendingQueries++;

    // Timeout fallback
    QTimer::singleShot(3000, this, [this]() {
        if (m_pendingQueries > 0) {
            m_pendingQueries--;
            if (m_currentServerIndex + 1 < m_servers.size()) {
                m_currentServerIndex++;
                sendNtpPacket(m_servers.at(m_currentServerIndex));
            } else {
                emit syncError("All NTP servers timed out");
            }
        }
    });
}

qint64 NtpClient::parseNtpPacket(const QByteArray& data) {
    if (data.size() < 48) return 0;
    const uchar* d = reinterpret_cast<const uchar*>(data.constData());

    quint32 txSeconds = qFromBigEndian<quint32>(d + 40);
    quint32 txFraction = qFromBigEndian<quint32>(d + 44);

    // NTP epoch to Unix epoch
    qint64 ntpTimeMs = (static_cast<qint64>(txSeconds) - 2208988800LL) * 1000LL
                     + (static_cast<qint64>(txFraction) * 1000LL / 4294967296LL);
    return ntpTimeMs;
}

void NtpClient::onReadyRead() {
    while (m_socket->hasPendingDatagrams()) {
        QByteArray data;
        data.resize(static_cast<int>(m_socket->pendingDatagramSize()));
        QHostAddress sender;
        quint16 port;
        m_socket->readDatagram(data.data(), data.size(), &sender, &port);

        qint64 receiveMs = QDateTime::currentDateTime().toMSecsSinceEpoch();
        qint64 ntpTimeMs = parseNtpPacket(data);
        qint64 roundTripMs = receiveMs - m_queryStartMs;

        // Offset = (receive - queryStart)/2 - (local - ntp)
        // Simplified: local clock is ahead of NTP if offset is positive
        qint64 localTimeAtMidpoint = m_queryStartMs + roundTripMs / 2;
        qint64 offsetMs = localTimeAtMidpoint - ntpTimeMs;

        m_lastOffsetMs = offsetMs;
        m_lastRoundTripMs = roundTripMs;
        m_lastSyncTime = QDateTime::currentDateTime();
        m_pendingQueries--;

        emit syncResult(offsetMs, roundTripMs, true);
    }
}

void NtpClient::onTimeout() {
    // Handled in query() timer lambda
}
