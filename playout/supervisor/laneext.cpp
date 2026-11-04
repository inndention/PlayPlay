#include "laneext.h"

#include <QDebug>
#include <QStringList>
#include <algorithm>
#include <vector>

namespace laneext {
namespace {
struct Entry { QString name; int order; InstallFn fn; };
std::vector<Entry>& entries() { static std::vector<Entry> e; return e; }   // function-local: safe across static-init order
}

Registrar::Registrar(const char* name, int order, InstallFn fn) { entries().push_back({QString::fromLatin1(name), order, std::move(fn)}); }

QStringList installAll(const Context& ctx) {
    std::vector<Entry> list = entries();
    std::stable_sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) { return a.order != b.order ? a.order < b.order : a.name < b.name; });
    QStringList done;
    for (const Entry& e : list) {
        if (!e.fn) continue;
        e.fn(ctx);
        done << e.name;
    }
    if (!done.isEmpty()) qInfo().noquote() << "[Supervisor] lane extensions:" << done.join(", ");
    return done;
}

QStringList registered() { QStringList n; for (const Entry& e : entries()) n << e.name; return n; }

} // namespace laneext
