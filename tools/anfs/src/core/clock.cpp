// core/clock.cpp - SystemClock, ManualClock and the process clock (ANFS-F1b).
//
// The process clock is one atomic pointer: production never installs anything, tests install a ManualClock for a
// scope (ScopedClock). Waits are written so a huge timeout (INT64_MAX as "forever") never overflows a time_point.
#include "core/clock.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace anfs {

namespace {

// 30 days: longer waits are clamped (a time_point + INT64_MAX ms would overflow); callers loop on their predicate.
constexpr int64_t kMaxWaitMs = 30LL * 24 * 3600 * 1000;
// ManualClock waiters re-check the manual time this often (real time); advance() does not know their condition
// variables, so polling is what lets any cv work without registration.
constexpr auto kManualSlice = std::chrono::milliseconds(2);

SystemClock& systemClock()
{
    static SystemClock c;
    return c;
}

std::atomic<Clock*> g_clock{nullptr};

int64_t saturatingAdd(int64_t a, int64_t b)
{
    if (b > 0 && a > INT64_MAX - b) return INT64_MAX;
    return a + b;
}

} // namespace

int64_t SystemClock::nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

int64_t SystemClock::monoMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void SystemClock::sleepMs(int64_t ms)
{
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(std::min(ms, kMaxWaitMs)));
}

bool SystemClock::waitFor(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, int64_t ms,
                          const std::function<bool()>& pred)
{
    if (ms <= 0) return pred();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::min(ms, kMaxWaitMs));
    return cv.wait_until(lock, deadline, [&] { return pred(); });
}

ManualClock::ManualClock(int64_t startEpochMs) : now_(startEpochMs) {}

int64_t ManualClock::nowMs()
{
    std::lock_guard<std::mutex> g(mu_);
    return now_;
}

int64_t ManualClock::monoMs()
{
    std::lock_guard<std::mutex> g(mu_);
    return mono_;
}

void ManualClock::sleepMs(int64_t ms)
{
    if (ms <= 0) return;
    const int64_t until = saturatingAdd(monoMs(), ms);
    while (monoMs() < until) std::this_thread::sleep_for(kManualSlice);
}

bool ManualClock::waitFor(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, int64_t ms,
                          const std::function<bool()>& pred)
{
    if (pred()) return true;
    if (ms <= 0) return false;
    const int64_t until = saturatingAdd(monoMs(), ms);
    for (;;) {
        // wait_for releases the caller's lock, so whoever changes the predicate's state can still take it.
        cv.wait_for(lock, kManualSlice);
        if (pred()) return true;
        if (monoMs() >= until) return pred();
    }
}

void ManualClock::advance(int64_t ms)
{
    std::lock_guard<std::mutex> g(mu_);
    now_ += ms;
    if (ms > 0) mono_ = saturatingAdd(mono_, ms);   // monotonic time never runs backwards
}

void ManualClock::set(int64_t epochMs)
{
    std::lock_guard<std::mutex> g(mu_);
    now_ = epochMs;   // a wall-clock jump: deadlines (mono) are unaffected, as with a real clock change
}

Clock& clock()
{
    Clock* c = g_clock.load(std::memory_order_acquire);
    return c ? *c : systemClock();
}

void setClock(Clock* c)
{
    g_clock.store(c, std::memory_order_release);
}

} // namespace anfs
