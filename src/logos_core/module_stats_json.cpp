#include "module_stats_json.h"

#include <unordered_set>

namespace LogosCore {

double InProcessCpuHistory::percentFor(const std::string& moduleName,
                                       double cpuTimeSeconds,
                                       int64_t nowMs)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    double percent = 0.0;
    auto previous = m_previous.find(moduleName);
    if (previous != m_previous.end()) {
        const double elapsed = (nowMs - previous->second.second) / 1000.0;
        if (elapsed > 0)
            percent = ((cpuTimeSeconds - previous->second.first) / elapsed) * 100.0;
    }
    m_previous[moduleName] = {cpuTimeSeconds, nowMs};

    // A module that was unloaded and loaded again gets a FRESH dispatch thread,
    // so its CPU time goes DOWN and the difference means nothing. Reporting the
    // negative percentage that produces would be worse than the zero this call
    // exists to replace.
    return percent < 0.0 ? 0.0 : percent;
}

nlohmann::json mergeModuleStats(
    const std::unordered_map<std::string, int64_t>& pids,
    const nlohmann::json& processStats,
    const std::unordered_map<std::string, ModuleResourceUsage>& usage,
    const std::function<double(const std::string&, double)>& inProcessCpuPercent)
{
    nlohmann::json merged =
        processStats.is_array() ? processStats : nlohmann::json::array();

    std::unordered_set<std::string> reported;
    for (auto& entry : merged) {
        if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string())
            continue;
        reported.insert(entry["name"].get<std::string>());
        // Stamped on every entry, not only the interesting ones: a consumer
        // asking "is this the whole process or one module's share of it" should
        // read the answer rather than infer it from the sign of the pid.
        entry["scope"] = "process";
        entry["memory_kind"] = "rss";
    }

    for (const auto& [name, pid] : pids) {
        // A module process-stats already answered for needs nothing more; a
        // module with a real pid that it could not read (gone, or refused) is
        // left alone rather than re-reported as in-process.
        if (pid >= 0 || reported.count(name))
            continue;

        const auto measured = usage.find(name);
        if (measured == usage.end()) {
            // NULL, NOT 0. Those resources exist and are the host's; nobody
            // measured this module's share of them, and zero is what a loaded,
            // idle module reports.
            merged.push_back({
                {"name", name},
                {"pid", pid},
                {"cpu_percent", nullptr},
                {"cpu_time_seconds", nullptr},
                {"memory_mb", nullptr},
                {"scope", "in_process"},
                {"memory_kind", nullptr},
            });
            continue;
        }

        merged.push_back({
            {"name", name},
            {"pid", pid},
            {"cpu_percent", inProcessCpuPercent(name, measured->second.cpuTimeSeconds)},
            {"cpu_time_seconds", measured->second.cpuTimeSeconds},
            {"memory_mb", measured->second.memoryBytes / (1024.0 * 1024.0)},
            {"scope", "in_process"},
            {"memory_kind",
             measured->second.memoryIsResident ? "image_resident" : "image_mapped"},
        });
    }

    return merged;
}

} // namespace LogosCore
