// THE SHAPE THE MODULES TAB READS.
//
// logos_core_get_module_stats() has two sources that do not resemble each
// other: process-stats, which reads a PROCESS by pid, and a container, which
// measures a module that has none. Merging them is where every way of being
// wrong about #86 lives — a module missing from the array, a module reported
// with somebody else's numbers, or the original defect, a module reported with
// zeroes that read as "loaded and idle" when nothing was ever measured.
//
// These tests are on that merge alone, with both sources handed in: no core, no
// container, no event loop.

#include <gtest/gtest.h>

#include "module_stats_json.h"

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>

using LogosCore::InProcessCpuHistory;
using LogosCore::ModuleResourceUsage;
using LogosCore::mergeModuleStats;

namespace {

// What process-stats answers for one pid it could read.
nlohmann::json processEntry(const std::string& name, int64_t pid, double cpu, double memoryMb)
{
    return nlohmann::json{
        {"name", name},
        {"pid", pid},
        {"cpu_percent", cpu},
        {"cpu_time_seconds", 1.5},
        {"memory_mb", memoryMb},
    };
}

const nlohmann::json& entryNamed(const nlohmann::json& array, const std::string& name)
{
    for (const auto& entry : array) {
        if (entry.value("name", std::string()) == name)
            return entry;
    }
    ADD_FAILURE() << "no entry named " << name << " in " << array.dump();
    static const nlohmann::json empty = nlohmann::json::object();
    return empty;
}

double zeroPercent(const std::string&, double) { return 0.0; }

} // namespace

TEST(ModuleStatsJsonTest, AProcessModuleKeepsItsFiguresAndIsNamedAsAWholeProcess)
{
    const std::unordered_map<std::string, int64_t> pids{{"chat_module", 4242}};
    nlohmann::json measured = nlohmann::json::array({processEntry("chat_module", 4242, 3.5, 61.25)});

    const nlohmann::json merged = mergeModuleStats(pids, measured, {}, zeroPercent);

    ASSERT_EQ(merged.size(), 1u);
    const nlohmann::json& entry = entryNamed(merged, "chat_module");
    EXPECT_EQ(entry.at("pid").get<int64_t>(), 4242);
    EXPECT_DOUBLE_EQ(entry.at("cpu_percent").get<double>(), 3.5);
    EXPECT_DOUBLE_EQ(entry.at("memory_mb").get<double>(), 61.25);
    // The discriminator a consumer needs in order to say what the number is,
    // rather than having to infer it from the sign of the pid.
    EXPECT_EQ(entry.at("scope").get<std::string>(), "process");
    EXPECT_EQ(entry.at("memory_kind").get<std::string>(), "rss");
}

TEST(ModuleStatsJsonTest, AnInProcessModuleIsReportedWithWhatTheContainerMeasured)
{
    const std::unordered_map<std::string, int64_t> pids{{"delivery_module", -1}};
    // process-stats skips a pid it cannot read, which is every in-process one.
    nlohmann::json measured = nlohmann::json::array();

    ModuleResourceUsage usage;
    usage.cpuTimeSeconds = 0.75;
    usage.memoryBytes = 3u * 1024u * 1024u;
    usage.memoryIsResident = true;

    const nlohmann::json merged = mergeModuleStats(
        pids, measured, {{"delivery_module", usage}},
        [](const std::string&, double) { return 12.5; });

    ASSERT_EQ(merged.size(), 1u);
    const nlohmann::json& entry = entryNamed(merged, "delivery_module");
    EXPECT_EQ(entry.at("pid").get<int64_t>(), -1);
    EXPECT_DOUBLE_EQ(entry.at("cpu_time_seconds").get<double>(), 0.75);
    EXPECT_DOUBLE_EQ(entry.at("cpu_percent").get<double>(), 12.5);
    EXPECT_DOUBLE_EQ(entry.at("memory_mb").get<double>(), 3.0);
    EXPECT_EQ(entry.at("scope").get<std::string>(), "in_process");
    EXPECT_EQ(entry.at("memory_kind").get<std::string>(), "image_resident");
}

TEST(ModuleStatsJsonTest, AMappingRatherThanAResidencyIsSaidToBeOne)
{
    const std::unordered_map<std::string, int64_t> pids{{"libp2p_module", -1}};
    ModuleResourceUsage usage;
    usage.memoryBytes = 1024u * 1024u;
    usage.memoryIsResident = false;

    const nlohmann::json merged = mergeModuleStats(
        pids, nlohmann::json::array(), {{"libp2p_module", usage}}, zeroPercent);

    EXPECT_EQ(entryNamed(merged, "libp2p_module").at("memory_kind").get<std::string>(),
              "image_mapped");
}

TEST(ModuleStatsJsonTest, AModuleNobodyCouldMeasureIsNullNeverZero)
{
    // The whole point of the distinction. Zero is what a loaded, idle module
    // reports; a module that was never measured has to be able to say so.
    const std::unordered_map<std::string, int64_t> pids{{"unmeasurable", -1}};

    const nlohmann::json merged =
        mergeModuleStats(pids, nlohmann::json::array(), {}, zeroPercent);

    ASSERT_EQ(merged.size(), 1u);
    const nlohmann::json& entry = entryNamed(merged, "unmeasurable");
    EXPECT_TRUE(entry.at("cpu_percent").is_null());
    EXPECT_TRUE(entry.at("cpu_time_seconds").is_null());
    EXPECT_TRUE(entry.at("memory_mb").is_null());
    EXPECT_TRUE(entry.at("memory_kind").is_null());
    EXPECT_EQ(entry.at("scope").get<std::string>(), "in_process");
}

TEST(ModuleStatsJsonTest, EveryRunningModuleGetsAnEntryWhicheverContainerItIsIn)
{
    // "One entry per running module" is the contract of the call, and a mixed
    // set is the case that broke it: the in-process members were silently
    // absent from process-stats' array.
    const std::unordered_map<std::string, int64_t> pids{
        {"chat_module", 4242}, {"capability_module", -1}, {"libp2p_module", -1}};
    nlohmann::json measured = nlohmann::json::array({processEntry("chat_module", 4242, 1.0, 10.0)});

    ModuleResourceUsage usage;
    usage.memoryBytes = 2u * 1024u * 1024u;

    const nlohmann::json merged =
        mergeModuleStats(pids, measured, {{"capability_module", usage}}, zeroPercent);

    ASSERT_EQ(merged.size(), 3u);
    EXPECT_DOUBLE_EQ(entryNamed(merged, "capability_module").at("memory_mb").get<double>(), 2.0);
    EXPECT_TRUE(entryNamed(merged, "libp2p_module").at("memory_mb").is_null());
    EXPECT_EQ(entryNamed(merged, "chat_module").at("scope").get<std::string>(), "process");
}

TEST(ModuleStatsJsonTest, AnUnreadableProcessStatsBlobStillLeavesTheInProcessModules)
{
    // process-stats always returns a valid array today, but this merge is what
    // stands between a malformed one and an empty Modules tab.
    const std::unordered_map<std::string, int64_t> pids{{"capability_module", -1}};

    const nlohmann::json merged = mergeModuleStats(
        pids, nlohmann::json("not an array"), {}, zeroPercent);

    ASSERT_TRUE(merged.is_array());
    ASSERT_EQ(merged.size(), 1u);
    EXPECT_EQ(merged.at(0).at("name").get<std::string>(), "capability_module");
}

// ── the percentage, which process-stats cannot keep for us ──────────────────

TEST(InProcessCpuHistoryTest, TheFirstSampleForAModuleIsZero)
{
    InProcessCpuHistory history;

    // There is no earlier reading to subtract, exactly as process-stats answers
    // for a pid it has not seen.
    EXPECT_DOUBLE_EQ(history.percentFor("capability_module", 4.0, 1000), 0.0);
}

TEST(InProcessCpuHistoryTest, ASecondSampleIsCpuOverWallClock)
{
    InProcessCpuHistory history;
    history.percentFor("capability_module", 1.0, 1000);

    // 0.5s of CPU across 2s of wall clock is 25% of one core.
    EXPECT_DOUBLE_EQ(history.percentFor("capability_module", 1.5, 3000), 25.0);
}

TEST(InProcessCpuHistoryTest, ModulesDoNotShareAHistory)
{
    InProcessCpuHistory history;
    history.percentFor("a", 1.0, 1000);
    history.percentFor("b", 100.0, 1000);

    EXPECT_DOUBLE_EQ(history.percentFor("a", 2.0, 2000), 100.0);
}

TEST(InProcessCpuHistoryTest, AReloadedModuleReportsZeroRatherThanANegativePercent)
{
    // Unload and load again and the module gets a FRESH dispatch thread, so its
    // CPU time goes down. The difference is meaningless; a negative percentage
    // would be worse than the zero this replaced.
    InProcessCpuHistory history;
    history.percentFor("capability_module", 9.0, 1000);

    EXPECT_DOUBLE_EQ(history.percentFor("capability_module", 0.1, 2000), 0.0);
}

TEST(InProcessCpuHistoryTest, TwoSamplesAtTheSameInstantAreNotADivisionByZero)
{
    InProcessCpuHistory history;
    history.percentFor("capability_module", 1.0, 1000);

    EXPECT_DOUBLE_EQ(history.percentFor("capability_module", 2.0, 1000), 0.0);
}
