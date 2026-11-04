#ifndef PP_UDPIN_H
#define PP_UDPIN_H

#include <QByteArray>
#include <QString>

/**
 * PlayPlayTsKit, part 5 (docs/plans/inhouse-deps.md §6.5 stage 2, DEP-16): UDP (multicast) TS input for the analyzer,
 * what `tsp -I ip --default-interface --reuse-port --buffer-size 2097152 --receive-timeout N <addr:port>` does in
 * MuxManager::analyze and tspArgs. Plain sockets (Winsock on Windows), no Qt event loop: the analyzer runs as a
 * blocking command-line pass (PlayPlayMux analyze), never on the Supervisor's GUI thread.
 *
 *  - unicast: binds the given address (or every interface) and port;
 *  - multicast: binds INADDR_ANY:port with SO_REUSEADDR (several listeners may share the group, e.g. a live mux and an
 *    analysis), joins the group on `localAddress` or the default interface; `source@group:port` joins source-specific.
 */
namespace tskit {

struct UdpSpec {
    QString group;                         // address to bind (unicast) or join (multicast); empty = any, unicast
    quint16 port = 0;
    QString source;                        // SSM source address
    QString localAddress;                  // interface address for the multicast join; empty = default interface
    bool isMulticast() const;
    QString toString() const;              // "[source@]group:port" (tsp syntax)
    /// "udp://[source@]group:port[?localaddr=A]", "[source@]group:port", ":port" or "port"; FFmpeg-style query options
    /// other than localaddr / sources are ignored.
    static bool parse(const QString& text, UdpSpec* out, QString* error = nullptr);
};

class UdpReceiver {
public:
    UdpReceiver() = default;
    ~UdpReceiver();
    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    bool open(const UdpSpec& spec, int bufferBytes = 2 * 1024 * 1024, QString* error = nullptr);
    /// Waits up to timeoutMs for one datagram: its size (> 0), 0 on timeout, -1 on a socket error (lastError()).
    int receive(QByteArray* datagram, int timeoutMs);
    void close();
    bool isOpen() const { return m_socket != kInvalid; }
    QString lastError() const { return m_error; }
    /// Monotonic clock (ns) used for arrival stamps.
    static qint64 nowNs();

private:
    static constexpr quintptr kInvalid = ~quintptr(0);
    quintptr m_socket = kInvalid;
    QString m_error;
    QByteArray m_buffer;
};

/// TS bytes of a datagram: raw TS as is; an RTP (version 2) header with CSRCs / extension is stripped.
QByteArray udpTsPayload(const QByteArray& datagram);

}   // namespace tskit

#endif // PP_UDPIN_H
