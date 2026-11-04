// core/svc.h - process-wide typed service registry (C# fscore Svc). FROZEN (ANFS-F0); the type-erased store is
// implemented by ANFS-F1b (core/svc.cpp).
//
// A module publishes in start() and removes in stop(); consumers look the service up per call and tolerate nullptr
// (module not running, mode switch in progress). Services are shared_ptr, so a consumer holding one keeps it alive
// while a stopping module drops its registration.
//
//   svcSet<IAgentRegistry>(registry);          // in start()
//   if (auto reg = svcGet<IAgentRegistry>()) { ... }
//   svcRemoveIf<IAgentRegistry>(registry);     // in stop(): never removes a successor registered meanwhile
#pragma once

#include <memory>
#include <typeindex>
#include <typeinfo>

namespace anfs {

class Svc {
public:
    static void setRaw(std::type_index type, std::shared_ptr<void> service);
    static std::shared_ptr<void> getRaw(std::type_index type);
    // Removes the registration; when only != nullptr, only if the registered object is that one.
    static void removeRaw(std::type_index type, const void* only = nullptr);
    static void clear();                          // tests
};

template <class T> void svcSet(std::shared_ptr<T> s)
{
    Svc::setRaw(std::type_index(typeid(T)), std::static_pointer_cast<void>(std::move(s)));
}

template <class T> std::shared_ptr<T> svcGet()
{
    return std::static_pointer_cast<T>(Svc::getRaw(std::type_index(typeid(T))));
}

template <class T> bool svcHas() { return Svc::getRaw(std::type_index(typeid(T))) != nullptr; }

template <class T> void svcRemove() { Svc::removeRaw(std::type_index(typeid(T))); }

template <class T> void svcRemoveIf(const std::shared_ptr<T>& s)
{
    Svc::removeRaw(std::type_index(typeid(T)), static_cast<const void*>(s.get()));
}

} // namespace anfs
