// core/svc.cpp - the type-erased store behind svcSet/svcGet/svcRemove (ANFS-F1b; C# FsCoreSvc.cs).
//
// Readers take a shared lock (every request looks services up); writers are module start/stop. A replaced or removed
// service is released only after the lock is dropped: its destructor may itself look up or remove services (a store
// that unregisters a hook), which would otherwise deadlock on the registry lock.
#include "core/svc.h"

#include <shared_mutex>
#include <unordered_map>
#include <utility>

namespace anfs {

namespace {

struct Registry {
    std::shared_mutex mu;
    std::unordered_map<std::type_index, std::shared_ptr<void>> map;
};

// Never destroyed: a static destructor at exit could release services after their modules' code is gone, and a
// late svcGet from another static destructor would touch a dead map. Modules remove their services in stop().
Registry& registry()
{
    static Registry* r = new Registry();
    return *r;
}

} // namespace

void Svc::setRaw(std::type_index type, std::shared_ptr<void> service)
{
    if (!service) {   // C# Svc.Set(null) removes
        removeRaw(type);
        return;
    }
    std::shared_ptr<void> old;
    {
        Registry& r = registry();
        std::unique_lock<std::shared_mutex> g(r.mu);
        std::shared_ptr<void>& slot = r.map[type];
        old = std::move(slot);
        slot = std::move(service);
    }
    // old is released here, outside the lock
}

std::shared_ptr<void> Svc::getRaw(std::type_index type)
{
    Registry& r = registry();
    std::shared_lock<std::shared_mutex> g(r.mu);
    auto it = r.map.find(type);
    return it == r.map.end() ? nullptr : it->second;
}

void Svc::removeRaw(std::type_index type, const void* only)
{
    std::shared_ptr<void> old;
    {
        Registry& r = registry();
        std::unique_lock<std::shared_mutex> g(r.mu);
        auto it = r.map.find(type);
        if (it == r.map.end()) return;
        if (only && it->second.get() != only) return;   // a successor registered meanwhile stays
        old = std::move(it->second);
        r.map.erase(it);
    }
}

void Svc::clear()
{
    std::unordered_map<std::type_index, std::shared_ptr<void>> old;
    {
        Registry& r = registry();
        std::unique_lock<std::shared_mutex> g(r.mu);
        old.swap(r.map);
    }
}

} // namespace anfs
