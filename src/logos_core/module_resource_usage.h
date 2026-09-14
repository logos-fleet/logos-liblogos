#ifndef MODULE_RESOURCE_USAGE_H
#define MODULE_RESOURCE_USAGE_H

#include <cstdint>
#include <string>
#include <unordered_map>

namespace LogosCore {

// WHAT A MODULE COSTS WHEN IT HAS NO PROCESS TO ASK ABOUT.
//
// The usual answer is process-stats: a module runs in a subprocess, the
// subprocess has a pid, and the OS will say what that pid is using. A module in
// the Native container has no process — it reports the -1 sentinel by
// construction (ADR 0003: a Store app gets no subprocess per module) — so that
// whole route answers nothing about it, and its zeroes are indistinguishable on
// screen from a loaded, idle module.
//
// The container is the only place that knows what such a module IS in the host
// process: the image it was dlopen'd from, and the worker thread its handlers
// run on. This is that measurement.
//
// IT IS A PARTIAL ACCOUNT AND SAYS SO. `memoryBytes` is the module's IMAGE —
// its code and static data — and not its heap: allocations made through the
// host's allocator are the host's and there is no boundary to charge them at.
// `cpuTimeSeconds` is the dispatch thread's, so work a module does on threads
// of its own is not in it. Both are floors, and both are measurements; the
// zeroes they replace were neither.
struct ModuleResourceUsage {
    // Total user+system CPU consumed by the module's own dispatch thread.
    double cpuTimeSeconds = 0.0;
    // The module image's footprint: resident bytes where the OS will say so,
    // otherwise the size of the mapping.
    std::uint64_t memoryBytes = 0;
    // Which of those two `memoryBytes` is, so a consumer can name it rather
    // than guess. False does not mean "no measurement" — it means the platform
    // would not report residency and this is the mapping.
    bool memoryIsResident = false;
};

// A container that can measure its modules WITHOUT a pid implements this.
//
// A liblogos-local mixin rather than a method on ModuleContainer: that
// interface is the logos-container contract package's, shared with containers
// that live outside this repo, and a pid-free measurement is not something
// every container can offer. CompositeModuleLoader asks for it by dynamic_cast
// and a container that does not implement it simply reports nothing, which is
// what a process-based one should do — its modules are process-stats' to
// measure.
class ResourceMeasurable {
public:
    virtual ~ResourceMeasurable() = default;

    // One entry per module this container is currently running. A module that
    // has been terminated must be absent rather than frozen at its last
    // reading.
    virtual std::unordered_map<std::string, ModuleResourceUsage>
    getAllResourceUsage() const = 0;
};

} // namespace LogosCore

#endif // MODULE_RESOURCE_USAGE_H
