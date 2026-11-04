#ifndef PLAYPLAY_HEALTHENDPOINT_H
#define PLAYPLAY_HEALTHENDPOINT_H

#include <QString>
#include <QtGlobal>
#include <atomic>
#include <functional>
#include <string>
#include <thread>

/**
 * Tiny HTTP/1.1 listener on 127.0.0.1 with its own thread and no Qt event loop (docs/plans/perf-supervisor-app.md §5.1,
 * W4). It answers while the process's main thread is frozen, which is exactly what was missing on 10-25: the hung
 * Supervisor's REST port accepted connections and never replied, so nobody could tell a busy loop from a dead one.
 *
 *   - Winsock, bound to 127.0.0.1 only, SO_EXCLUSIVEADDRUSE (it never shares or steals a port). Port 0 = ephemeral;
 *     port() returns the bound one.
 *   - One request per connection, served one at a time: read the request line (1 s receive timeout, 8 KB cap), call
 *     the handler, write the reply with Connection: close (1 s send timeout). A client that connects and says nothing
 *     costs at most a second.
 *   - The handler runs on the endpoint thread: it must only read atomics / immutable data, never Qt objects owned by
 *     another thread, and never block.
 *   - start() binds synchronously (the caller learns at once that the port is taken); stop() wakes the thread with an
 *     event and joins it (bounded by one request's timeouts).
 *
 * Only GET and HEAD are served. Paths are passed to the handler without the query string.
 */
class HealthEndpoint {
public:
    struct Reply {
        int status = 200;
        std::string contentType = "application/json";
        std::string body;
    };
    /// method ("GET"/"HEAD") and path ("/healthz") -> reply; return status 404 for paths it does not serve.
    using Handler = std::function<Reply(const std::string& method, const std::string& path)>;

    HealthEndpoint() = default;
    ~HealthEndpoint();
    HealthEndpoint(const HealthEndpoint&) = delete;
    HealthEndpoint& operator=(const HealthEndpoint&) = delete;

    /// Binds 127.0.0.1:port and starts the thread. A running endpoint is stopped first. False (and *error) if the
    /// port cannot be bound.
    bool start(quint16 port, Handler handler, QString* error = nullptr);
    void stop();
    bool isRunning() const { return m_running.load(); }
    quint16 port() const { return m_port.load(); }
    quint64 requestsServed() const { return m_served.load(); }

private:
    void run();

    Handler m_handler;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<quint16> m_port{0};
    std::atomic<quint64> m_served{0};
    quintptr m_listen = ~quintptr(0);   // SOCKET (INVALID_SOCKET when closed)
    void* m_stopEvent = nullptr;        // HANDLE
    void* m_acceptEvent = nullptr;      // WSAEVENT
};

#endif // PLAYPLAY_HEALTHENDPOINT_H
