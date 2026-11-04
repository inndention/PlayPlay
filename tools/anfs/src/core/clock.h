// core/clock.h - injectable clock for every AN-FS timer (agent expiry 15 s, relay 25/30/120 s, transfer prune 5 min,
// gate lockout 10 min, 5 s caches). FROZEN (ANFS-F0); implemented by ANFS-F1b (core/clock.cpp).
//
// Production code never calls std::chrono or Sleep directly for a protocol timer: it asks anfs::clock(), so tests can
// install a ManualClock and advance time instantly. Socket inactivity timeouts (SO_RCVTIMEO) stay real time.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace anfs {

class Clock {
public:
    virtual ~Clock() = default;
    virtual int64_t nowMs() = 0;      // wall clock, epoch milliseconds (Date.now())
    virtual int64_t monoMs() = 0;     // monotonic milliseconds, for deadlines
    virtual void sleepMs(int64_t ms) = 0;
    // Waits on cv (lock held) until pred() is true or ms of this clock's time passed; returns pred().
    // A ManualClock wakes waiters when advanced, so a 25 s relay park finishes as soon as a test advances 25 s.
    virtual bool waitFor(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, int64_t ms,
                         const std::function<bool()>& pred) = 0;
};

class SystemClock final : public Clock {
public:
    int64_t nowMs() override;
    int64_t monoMs() override;
    void sleepMs(int64_t ms) override;
    bool waitFor(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, int64_t ms,
                 const std::function<bool()>& pred) override;
};

// Test clock: time moves only through advance()/set(). Waiters poll in short real-time slices (a few ms) and
// compare against the manual time, so any condition variable works without registration.
class ManualClock final : public Clock {
public:
    explicit ManualClock(int64_t startEpochMs = 1760000000000LL);
    int64_t nowMs() override;
    int64_t monoMs() override;
    void sleepMs(int64_t ms) override;          // returns once the manual time passed now + ms
    bool waitFor(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, int64_t ms,
                 const std::function<bool()>& pred) override;
    void advance(int64_t ms);
    void set(int64_t epochMs);

private:
    std::mutex mu_;
    int64_t now_;
    int64_t mono_ = 0;
};

// The process clock (a SystemClock unless a test installed another one). setClock(nullptr) restores the system clock.
// The caller keeps an installed clock alive until it is replaced.
Clock& clock();
void setClock(Clock* c);

inline int64_t nowMs() { return clock().nowMs(); }
inline int64_t monoMs() { return clock().monoMs(); }

// RAII helper for tests: installs a clock for one scope.
class ScopedClock {
public:
    explicit ScopedClock(Clock& c) { setClock(&c); }
    ~ScopedClock() { setClock(nullptr); }
    ScopedClock(const ScopedClock&) = delete;
    ScopedClock& operator=(const ScopedClock&) = delete;
};

} // namespace anfs
