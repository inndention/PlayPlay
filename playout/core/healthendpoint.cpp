#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif
#include "healthendpoint.h"
#include <cstring>

namespace {

const char* reasonPhrase(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 503: return "Service Unavailable";
    default: return status >= 500 ? "Server Error" : "Status";
    }
}

std::string buildResponse(const HealthEndpoint::Reply& r, bool head) {
    std::string out;
    out.reserve(160 + r.body.size());
    out += "HTTP/1.1 " + std::to_string(r.status) + " " + reasonPhrase(r.status) + "\r\n";
    out += "Content-Type: " + (r.contentType.empty() ? std::string("text/plain") : r.contentType) + "\r\n";
    out += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    out += "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    if (!head) out += r.body;
    return out;
}

} // namespace

HealthEndpoint::~HealthEndpoint() { stop(); }

#ifdef _WIN32

bool HealthEndpoint::start(quint16 port, Handler handler, QString* error) {
    stop();
    auto fail = [&](const QString& why) { if (error) *error = why; return false; };
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return fail(QStringLiteral("WSAStartup failed"));
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { const int e = WSAGetLastError(); WSACleanup(); return fail(QStringLiteral("socket() failed: %1").arg(e)); }
    // Never share the port with another listener and never take over one in use (a second Supervisor must not
    // answer for the first one's health).
    BOOL excl = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(s, 8) != 0) {
        const int e = WSAGetLastError();
        ::closesocket(s); WSACleanup();
        return fail(QStringLiteral("127.0.0.1:%1 not available (WSA error %2)").arg(port).arg(e));
    }
    int len = sizeof(a);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    // Accepts are waited for together with the stop event: stop() never waits for a poll interval.
    WSAEVENT ev = WSACreateEvent();
    WSAEventSelect(s, ev, FD_ACCEPT);
    m_listen = quintptr(s);
    m_acceptEvent = ev;
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_handler = std::move(handler);
    m_port.store(ntohs(a.sin_port));
    m_running.store(true);
    m_thread = std::thread([this]() { run(); });
    return true;
}

void HealthEndpoint::stop() {
    if (!m_thread.joinable()) return;
    SetEvent(static_cast<HANDLE>(m_stopEvent));
    m_thread.join();
    ::closesocket(SOCKET(m_listen));
    WSACloseEvent(static_cast<WSAEVENT>(m_acceptEvent));
    CloseHandle(static_cast<HANDLE>(m_stopEvent));
    m_listen = quintptr(INVALID_SOCKET);
    m_acceptEvent = m_stopEvent = nullptr;
    m_running.store(false);
    m_port.store(0);
    WSACleanup();
}

void HealthEndpoint::run() {
    const HANDLE waits[2] = {static_cast<HANDLE>(m_stopEvent), static_cast<HANDLE>(m_acceptEvent)};
    const SOCKET ls = SOCKET(m_listen);
    for (;;) {
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0 + 1) break;                       // stop event (or a failed wait: leave quietly)
        WSANETWORKEVENTS ne{};
        WSAEnumNetworkEvents(ls, static_cast<WSAEVENT>(m_acceptEvent), &ne);   // resets the event
        for (;;) {
            SOCKET c = ::accept(ls, nullptr, nullptr);
            if (c == INVALID_SOCKET) break;                      // WSAEWOULDBLOCK: nothing more queued
            // An accepted socket inherits the listener's event selection (and non-blocking mode): undo both, then
            // bound every call with timeouts so a silent client costs at most a second.
            WSAEventSelect(c, nullptr, 0);
            u_long blocking = 0;
            ioctlsocket(c, FIONBIO, &blocking);
            DWORD tmo = 1000;
            ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
            ::setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
            char buf[8192];
            int got = 0;
            const ULONGLONG t0 = GetTickCount64();
            while (got < int(sizeof(buf)) - 1 && GetTickCount64() - t0 < 1500) {
                const int n = ::recv(c, buf + got, int(sizeof(buf)) - 1 - got, 0);
                if (n <= 0) break;
                got += n;
                buf[got] = 0;
                if (std::strstr(buf, "\r\n\r\n") || std::strstr(buf, "\n\n")) break;
            }
            buf[got] = 0;
            Reply r;
            bool head = false;
            const char* sp1 = std::strchr(buf, ' ');
            const char* sp2 = sp1 ? std::strchr(sp1 + 1, ' ') : nullptr;
            if (!sp1 || !sp2) {
                r.status = 400; r.contentType = "text/plain"; r.body = "bad request\n";
            } else {
                const std::string method(buf, size_t(sp1 - buf));
                std::string path(sp1 + 1, size_t(sp2 - sp1 - 1));
                const size_t q = path.find('?');
                if (q != std::string::npos) path.resize(q);
                head = method == "HEAD";
                if (method != "GET" && !head) { r.status = 405; r.contentType = "text/plain"; r.body = "GET only\n"; }
                else if (m_handler) r = m_handler(method, path);
                else { r.status = 404; r.contentType = "text/plain"; r.body = "not found\n"; }
            }
            const std::string out = buildResponse(r, head);
            size_t sent = 0;
            while (sent < out.size()) {
                const int n = ::send(c, out.data() + sent, int(out.size() - sent), 0);
                if (n <= 0) break;
                sent += size_t(n);
            }
            ::shutdown(c, SD_SEND);
            ::closesocket(c);
            m_served.fetch_add(1);
            if (WaitForSingleObject(waits[0], 0) == WAIT_OBJECT_0) return;
        }
    }
}

#else   // not Windows: no endpoint (the playout stack is Windows-only today)

bool HealthEndpoint::start(quint16, Handler, QString* error) {
    if (error) *error = QStringLiteral("health endpoint is implemented for Windows only");
    return false;
}
void HealthEndpoint::stop() {}
void HealthEndpoint::run() {}

#endif
