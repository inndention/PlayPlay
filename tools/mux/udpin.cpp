// Winsock first: Qt headers must not pull in the old winsock.h before it.
#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <cerrno>
#  include <cstring>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include "udpin.h"

#include <QStringList>
#include <QUrlQuery>

#include <chrono>

namespace tskit {

namespace {

#ifdef _WIN32
using Socket = SOCKET;
bool socketsReady() {
    static const bool ok = [] { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0; }();   // once per process
    return ok;
}
int lastSocketError() { return WSAGetLastError(); }
void closeSocket(Socket s) { closesocket(s); }
QString socketErrorText(int code) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, DWORD(code), 0,
                   reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    QString s = msg ? QString::fromWCharArray(msg).trimmed() : QString();
    if (msg) LocalFree(msg);
    return QStringLiteral("%1 (error %2)").arg(s.isEmpty() ? QStringLiteral("socket error") : s).arg(code);
}
#else
using Socket = int;
bool socketsReady() { return true; }
int lastSocketError() { return errno; }
void closeSocket(Socket s) { ::close(s); }
QString socketErrorText(int code) { return QStringLiteral("%1 (errno %2)").arg(QString::fromLocal8Bit(std::strerror(code))).arg(code); }
#endif

bool ipv4(const QString& text, in_addr* out) {
    if (text.isEmpty()) return false;
    const QStringList parts = text.split(QLatin1Char('.'));
    if (parts.size() != 4) return false;
    for (const QString& p : parts) { bool ok = false; const int v = p.toInt(&ok); if (!ok || v < 0 || v > 255) return false; }
    return inet_pton(AF_INET, text.toLatin1().constData(), out) == 1;
}

}   // namespace

bool UdpSpec::isMulticast() const {
    const int first = group.section(QLatin1Char('.'), 0, 0).toInt();
    return first >= 224 && first <= 239;
}

QString UdpSpec::toString() const {
    return (source.isEmpty() ? QString() : source + QLatin1Char('@')) + group + QLatin1Char(':') + QString::number(port);
}

bool UdpSpec::parse(const QString& text, UdpSpec* out, QString* error) {
    auto fail = [&](const QString& e) { if (error) *error = e; return false; };
    QString t = text.trimmed();
    UdpSpec s;
    if (t.startsWith(QLatin1String("udp://"), Qt::CaseInsensitive)) t = t.mid(6);
    const int q = t.indexOf(QLatin1Char('?'));
    if (q >= 0) {
        const QUrlQuery query(t.mid(q + 1));
        s.localAddress = query.queryItemValue(QStringLiteral("localaddr"));
        s.source = query.queryItemValue(QStringLiteral("sources")).section(QLatin1Char(','), 0, 0);
        t = t.left(q);
    }
    if (t.startsWith(QLatin1Char('@'))) t = t.mid(1);    // FFmpeg's udp://@group:port
    const int at = t.indexOf(QLatin1Char('@'));
    if (at >= 0) { s.source = t.left(at); t = t.mid(at + 1); }
    const int colon = t.lastIndexOf(QLatin1Char(':'));
    s.group = colon >= 0 ? t.left(colon) : QString();
    bool ok = false;
    const int port = (colon >= 0 ? t.mid(colon + 1) : t).toInt(&ok);
    if (!ok || port <= 0 || port > 65535) return fail(QStringLiteral("not a UDP address: %1 (use [source@]address:port)").arg(text));
    s.port = quint16(port);
    in_addr a;
    if (!s.group.isEmpty() && !ipv4(s.group, &a)) return fail(QStringLiteral("not an IPv4 address: %1").arg(s.group));
    if (!s.source.isEmpty() && !ipv4(s.source, &a)) return fail(QStringLiteral("not an IPv4 source address: %1").arg(s.source));
    if (!s.localAddress.isEmpty() && !ipv4(s.localAddress, &a)) return fail(QStringLiteral("not an IPv4 local address: %1").arg(s.localAddress));
    if (!s.source.isEmpty() && !s.isMulticast()) return fail(QStringLiteral("a source address needs a multicast group: %1").arg(text));
    if (out) *out = s;
    return true;
}

UdpReceiver::~UdpReceiver() { close(); }

qint64 UdpReceiver::nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void UdpReceiver::close() {
    if (m_socket != kInvalid) closeSocket(Socket(m_socket));
    m_socket = kInvalid;
}

bool UdpReceiver::open(const UdpSpec& spec, int bufferBytes, QString* error) {
    close();
    m_error.clear();
    auto fail = [&](const QString& what) {
        m_error = what + QStringLiteral(": ") + socketErrorText(lastSocketError());
        close();
        if (error) *error = m_error;
        return false;
    };
    if (!socketsReady()) return fail(QStringLiteral("socket library"));
    const Socket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (s == INVALID_SOCKET) return fail(QStringLiteral("socket"));
#else
    if (s < 0) return fail(QStringLiteral("socket"));
#endif
    m_socket = quintptr(s);
    const int one = 1;
    if (::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one)) != 0) return fail(QStringLiteral("SO_REUSEADDR"));
    if (bufferBytes > 0) ::setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufferBytes), sizeof(bufferBytes));   // best effort, like tsp

    in_addr group{}, iface{}, source{};
    iface.s_addr = htonl(INADDR_ANY);
    const bool multicast = spec.isMulticast();
    if (!spec.group.isEmpty() && !ipv4(spec.group, &group)) { m_error = QStringLiteral("bad address ") + spec.group; close(); if (error) *error = m_error; return false; }
    if (!spec.localAddress.isEmpty() && !ipv4(spec.localAddress, &iface)) { m_error = QStringLiteral("bad local address ") + spec.localAddress; close(); if (error) *error = m_error; return false; }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(spec.port);
    local.sin_addr.s_addr = multicast || spec.group.isEmpty() ? htonl(INADDR_ANY) : group.s_addr;
    if (::bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) return fail(QStringLiteral("bind %1").arg(spec.toString()));
    if (multicast) {
        if (!spec.source.isEmpty()) {
            if (!ipv4(spec.source, &source)) { m_error = QStringLiteral("bad source ") + spec.source; close(); if (error) *error = m_error; return false; }
            ip_mreq_source m{};
            m.imr_multiaddr = group; m.imr_sourceaddr = source; m.imr_interface = iface;
            if (::setsockopt(s, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, reinterpret_cast<const char*>(&m), sizeof(m)) != 0) return fail(QStringLiteral("join %1").arg(spec.toString()));
        } else {
            ip_mreq m{};
            m.imr_multiaddr = group; m.imr_interface = iface;
            if (::setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&m), sizeof(m)) != 0) return fail(QStringLiteral("join %1").arg(spec.toString()));
        }
    }
    m_buffer.resize(65536);
    return true;
}

int UdpReceiver::receive(QByteArray* datagram, int timeoutMs) {
    if (!isOpen()) { m_error = QStringLiteral("not open"); return -1; }
    const Socket s = Socket(m_socket);
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    timeval tv;
    tv.tv_sec = qMax(0, timeoutMs) / 1000;
    tv.tv_usec = (qMax(0, timeoutMs) % 1000) * 1000;
    const int r = ::select(int(s) + 1, &rd, nullptr, nullptr, &tv);   // nfds is ignored by Winsock
    if (r == 0) return 0;
    if (r < 0) { m_error = QStringLiteral("select: ") + socketErrorText(lastSocketError()); return -1; }
    const int n = int(::recvfrom(s, m_buffer.data(), int(m_buffer.size()), 0, nullptr, nullptr));
    if (n < 0) {
        const int e = lastSocketError();
#ifdef _WIN32
        if (e == WSAEMSGSIZE || e == WSAECONNRESET) return 0;   // oversized datagram / stale ICMP: skip it
#endif
        m_error = QStringLiteral("recvfrom: ") + socketErrorText(e);
        return -1;
    }
    if (datagram) *datagram = QByteArray(m_buffer.constData(), n);
    return n;
}

QByteArray udpTsPayload(const QByteArray& d) {
    if (d.size() >= 12 && quint8(d[0]) != 0x47 && (quint8(d[0]) & 0xC0) == 0x80) {
        int off = 12 + 4 * (quint8(d[0]) & 0x0F);
        if ((quint8(d[0]) & 0x10) && d.size() >= off + 4) off += 4 + 4 * ((quint8(d[off + 2]) << 8) | quint8(d[off + 3]));
        if (off < d.size() && quint8(d[off]) == 0x47) return d.mid(off);
    }
    return d;
}

}   // namespace tskit
