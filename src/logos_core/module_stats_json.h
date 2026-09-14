#ifndef MODULE_STATS_JSON_H
#define MODULE_STATS_JSON_H

#include "module_resource_usage.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace LogosCore {

// THE MERGE BEHIND logos_core_get_module_stats().
//
// The call has two sources that do not resemble each other. process-stats reads
// a PROCESS by pid and skips anything it cannot read. A container measures a
// module that has no process at all. Every way of being wrong about #86 lives
// in putting those together: a running module missing from the array, a module
// reported with the host's numbers, or the original defect — zeroes that read
// as "loaded and idle" for something nobody ever measured.
//
// Lifted out of logos_core.cpp because none of that needs a core to exercise.

// The CPU percentage over the previous sample, which process-stats keeps per
// PID and therefore cannot keep for a set of modules that all report -1.
//
// `nowMs` is a parameter rather than a clock read inside, so the arithmetic is
// testable without sleeping.
class InProcessCpuHistory {
public:
    // Zero on the first sample for a module: there is no earlier reading to
    // subtract, which is also what process-stats answers for an unseen pid.
    double percentFor(const std::string& moduleName, double cpuTimeSeconds, int64_t nowMs);

private:
    std::mutex m_mutex;
    std::unordered_map<std::string, std::pair<double, int64_t>> m_previous;
};

// One entry per name in `pids`, whichever container the module runs in.
//
//   * a module process-stats measured keeps its figures and is stamped
//     scope "process" / memory_kind "rss";
//   * a module with no pid that a container measured is reported from `usage`,
//     scope "in_process", with memory_kind saying whether the figure is the
//     image's residency or its mapping;
//   * a module nobody could measure gets NULLs — never zeroes, which are what a
//     loaded idle module reports.
//
// `processStats` is process-stats' array; anything else is treated as empty,
// so a malformed blob costs the process-based entries and not the whole tab.
nlohmann::json mergeModuleStats(
    const std::unordered_map<std::string, int64_t>& pids,
    const nlohmann::json& processStats,
    const std::unordered_map<std::string, ModuleResourceUsage>& usage,
    const std::function<double(const std::string& name, double cpuTimeSeconds)>&
        inProcessCpuPercent);

} // namespace LogosCore

#endif // MODULE_STATS_JSON_H
