// =============================================================================
// Tests for the dependency resolver algorithm exposed via logos_core_resolve_dependencies.
//
// The resolver implements Kahn's topological sort over the registered module
// graph. These tests cover:
//   - Empty / unknown input -> empty result
//   - Single module with no deps
//   - Linear chain (a -> b -> c)
//   - Diamond topology (a -> b,c ; b,c -> d)
//   - Multiple requested roots
//   - Circular dependency (must not hang or crash; partial result expected)
//   - Modules already marked as loaded are still included in the resolved list
//     (the resolver only orders them; the caller decides whether to skip loaded ones)
// =============================================================================
#include <gtest/gtest.h>
#include "logos_core.h"
#include "qt_test_adapter.h"
#include <cstring>
#include <set>
#include <string>
#include <vector>

static void clearModuleState() {
    logos_core_terminate_all();
    logos_core_clear();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int resolvedLen(char** arr) {
    if (!arr) return 0;
    int n = 0;
    while (arr[n]) ++n;
    return n;
}

static std::vector<std::string> resolvedToVec(char** arr) {
    std::vector<std::string> v;
    if (!arr) return v;
    for (int i = 0; arr[i]; ++i)
        v.push_back(arr[i]);
    return v;
}

static void freeResolved(char** arr) {
    if (!arr) return;
    for (int i = 0; arr[i]; ++i)
        delete[] arr[i];
    delete[] arr;
}

class DependencyResolverTest : public ::testing::Test {
protected:
    void SetUp() override { clearModuleState(); }
    void TearDown() override { clearModuleState(); }
};

// ---------------------------------------------------------------------------
// Edge cases: empty / unknown input
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, EmptyInput_ReturnsEmpty) {
    char** result = logos_core_resolve_dependencies(nullptr, 0);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);
    delete[] result;
}

TEST_F(DependencyResolverTest, UnknownModule_ReturnsEmpty) {
    const char* names[] = {"ghost_module"};
    char** result = logos_core_resolve_dependencies(names, 1);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);
    freeResolved(result);
}

TEST_F(DependencyResolverTest, AllUnknown_ReturnsEmpty) {
    const char* names[] = {"alpha", "beta", "gamma"};
    char** result = logos_core_resolve_dependencies(names, 3);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);
    freeResolved(result);
}

// ---------------------------------------------------------------------------
// Single module, no dependencies
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, SingleModuleNoDeps_ReturnsSelf) {
    logos_core_register_module("solo", "/path/solo");
    logos_core_register_module_dependencies("solo", nullptr, 0);

    const char* names[] = {"solo"};
    char** result = logos_core_resolve_dependencies(names, 1);

    ASSERT_EQ(resolvedLen(result), 1);
    EXPECT_STREQ(result[0], "solo");
    freeResolved(result);
}

// ---------------------------------------------------------------------------
// Linear chain: a depends on b, b depends on c.
// Expected order: c, b, a.
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, LinearChain_CorrectTopologicalOrder) {
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    logos_core_register_module("c", "/c");
    const char* depsA[] = {"b"};
    const char* depsB[] = {"c"};
    logos_core_register_module_dependencies("a", depsA, 1);
    logos_core_register_module_dependencies("b", depsB, 1);
    logos_core_register_module_dependencies("c", nullptr, 0);

    const char* names[] = {"a"};
    char** result = logos_core_resolve_dependencies(names, 1);
    auto v = resolvedToVec(result);
    freeResolved(result);

    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], "c");
    EXPECT_EQ(v[1], "b");
    EXPECT_EQ(v[2], "a");
}

// ---------------------------------------------------------------------------
// Longer chain: d -> c -> b -> a
// Expected order: a, b, c, d
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, FourNodeChain_CorrectOrder) {
    logos_core_register_module("d", "/d");
    logos_core_register_module("c", "/c");
    logos_core_register_module("b", "/b");
    logos_core_register_module("a", "/a");
    const char* depsD[] = {"c"};
    const char* depsC[] = {"b"};
    const char* depsB[] = {"a"};
    logos_core_register_module_dependencies("d", depsD, 1);
    logos_core_register_module_dependencies("c", depsC, 1);
    logos_core_register_module_dependencies("b", depsB, 1);
    logos_core_register_module_dependencies("a", nullptr, 0);

    const char* names[] = {"d"};
    char** result = logos_core_resolve_dependencies(names, 1);
    auto v = resolvedToVec(result);
    freeResolved(result);

    ASSERT_EQ(v.size(), 4u);
    EXPECT_EQ(v[0], "a");
    EXPECT_EQ(v[1], "b");
    EXPECT_EQ(v[2], "c");
    EXPECT_EQ(v[3], "d");
}

// ---------------------------------------------------------------------------
// Diamond: a -> b, a -> c; b -> d; c -> d
// Expected: d comes before both b and c, which both come before a.
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, DiamondDependency_DSharedRootComesFirst) {
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    logos_core_register_module("c", "/c");
    logos_core_register_module("d", "/d");
    const char* depsA[] = {"b", "c"};
    const char* depsB[] = {"d"};
    const char* depsC[] = {"d"};
    logos_core_register_module_dependencies("a", depsA, 2);
    logos_core_register_module_dependencies("b", depsB, 1);
    logos_core_register_module_dependencies("c", depsC, 1);
    logos_core_register_module_dependencies("d", nullptr, 0);

    const char* names[] = {"a"};
    char** result = logos_core_resolve_dependencies(names, 1);
    auto v = resolvedToVec(result);
    freeResolved(result);

    ASSERT_EQ(v.size(), 4u);

    // d must come before b and c; a must come last.
    auto pos = [&](const std::string& name) {
        for (size_t i = 0; i < v.size(); ++i) if (v[i] == name) return (int)i;
        return -1;
    };

    EXPECT_LT(pos("d"), pos("b"));
    EXPECT_LT(pos("d"), pos("c"));
    EXPECT_EQ(v.back(), "a");
}

// ---------------------------------------------------------------------------
// Multiple requested roots: request both a and x (independent trees).
// Both plus their deps should appear in the result.
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, MultipleRoots_BothTreesIncluded) {
    // Tree 1: a -> b
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    const char* depsA[] = {"b"};
    logos_core_register_module_dependencies("a", depsA, 1);
    logos_core_register_module_dependencies("b", nullptr, 0);

    // Tree 2: x -> y
    logos_core_register_module("x", "/x");
    logos_core_register_module("y", "/y");
    const char* depsX[] = {"y"};
    logos_core_register_module_dependencies("x", depsX, 1);
    logos_core_register_module_dependencies("y", nullptr, 0);

    const char* names[] = {"a", "x"};
    char** result = logos_core_resolve_dependencies(names, 2);
    auto v = resolvedToVec(result);
    freeResolved(result);

    ASSERT_EQ(v.size(), 4u);

    std::set<std::string> s(v.begin(), v.end());
    EXPECT_TRUE(s.count("a"));
    EXPECT_TRUE(s.count("b"));
    EXPECT_TRUE(s.count("x"));
    EXPECT_TRUE(s.count("y"));

    // Ordering constraints still hold within each tree.
    auto pos = [&](const std::string& name) {
        for (size_t i = 0; i < v.size(); ++i) if (v[i] == name) return (int)i;
        return -1;
    };
    EXPECT_LT(pos("b"), pos("a"));
    EXPECT_LT(pos("y"), pos("x"));
}

// ---------------------------------------------------------------------------
// Shared dependency: two requested roots share a dep.
// The shared dep must appear exactly once in the result.
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, SharedDependency_AppearsOnce) {
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    logos_core_register_module("shared", "/shared");
    const char* depsA[] = {"shared"};
    const char* depsB[] = {"shared"};
    logos_core_register_module_dependencies("a", depsA, 1);
    logos_core_register_module_dependencies("b", depsB, 1);
    logos_core_register_module_dependencies("shared", nullptr, 0);

    const char* names[] = {"a", "b"};
    char** result = logos_core_resolve_dependencies(names, 2);
    auto v = resolvedToVec(result);
    freeResolved(result);

    ASSERT_EQ(v.size(), 3u);
    int sharedCount = 0;
    for (const auto& s : v) if (s == "shared") ++sharedCount;
    EXPECT_EQ(sharedCount, 1) << "shared dependency must appear exactly once";
}

// ---------------------------------------------------------------------------
// Mixed known/unknown: requesting both a known and an unknown module.
// Known module and its deps should be resolved; unknown should be silently
// dropped.
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, MixedKnownUnknown_UnknownDropped) {
    logos_core_register_module("known", "/known");
    logos_core_register_module_dependencies("known", nullptr, 0);

    const char* names[] = {"known", "unknown_xyz"};
    char** result = logos_core_resolve_dependencies(names, 2);
    auto v = resolvedToVec(result);
    freeResolved(result);

    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], "known");
}

// ---------------------------------------------------------------------------
// Partially unknown dependency: a depends on b, b depends on "missing".
// "missing" is not registered.  The resolver should silently drop "missing"
// and still return a and b in some valid order.
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, PartiallyUnknownDep_KnownModulesStillResolved) {
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    const char* depsA[] = {"b"};
    const char* depsB[] = {"missing_dep"};
    logos_core_register_module_dependencies("a", depsA, 1);
    logos_core_register_module_dependencies("b", depsB, 1);

    const char* names[] = {"a"};
    char** result = logos_core_resolve_dependencies(names, 1);
    auto v = resolvedToVec(result);
    freeResolved(result);

    // "missing_dep" is unknown so it is skipped; a and b should still appear.
    std::set<std::string> s(v.begin(), v.end());
    EXPECT_TRUE(s.count("a"));
    EXPECT_TRUE(s.count("b"));
    EXPECT_EQ(s.count("missing_dep"), 0u);
}

// ---------------------------------------------------------------------------
// Circular dependency: a -> b -> a.
// Must not hang or crash; the result may be partial.
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, CircularDependency_DoesNotHangOrCrash) {
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    const char* depsA[] = {"b"};
    const char* depsB[] = {"a"};
    logos_core_register_module_dependencies("a", depsA, 1);
    logos_core_register_module_dependencies("b", depsB, 1);

    const char* names[] = {"a"};
    // Must return without hanging.
    char** result = logos_core_resolve_dependencies(names, 1);
    // We don't assert a specific result for cycles — just that it doesn't crash.
    freeResolved(result);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Already-loaded modules still appear in the resolved list.
// The resolver is purely structural; the caller decides whether to skip them.
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, AlreadyLoadedModule_StillIncludedInResolution) {
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    const char* depsA[] = {"b"};
    logos_core_register_module_dependencies("a", depsA, 1);
    logos_core_register_module_dependencies("b", nullptr, 0);

    // Simulate b already loaded.
    logos_core_mark_module_loaded("b");

    const char* names[] = {"a"};
    char** result = logos_core_resolve_dependencies(names, 1);
    auto v = resolvedToVec(result);
    freeResolved(result);

    // Both a and b must be in the result; the resolver doesn't filter loaded ones.
    std::set<std::string> s(v.begin(), v.end());
    EXPECT_TRUE(s.count("a"));
    EXPECT_TRUE(s.count("b"));
}

// ---------------------------------------------------------------------------
// Duplicate names in the request are collapsed to a single entry.
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, DuplicateNamesInRequest_AppearOnce) {
    logos_core_register_module("a", "/a");
    logos_core_register_module_dependencies("a", nullptr, 0);

    const char* names[] = {"a", "a", "a"};
    char** result = logos_core_resolve_dependencies(names, 3);
    auto v = resolvedToVec(result);
    freeResolved(result);

    int count = 0;
    for (const auto& s : v) if (s == "a") ++count;
    EXPECT_EQ(count, 1) << "duplicate requests should collapse to one entry";
}

// ---------------------------------------------------------------------------
// Optional dependencies (metadata.json#optional_dependencies).
//
// They are the third dependency kind: concrete like `dependencies`, but never
// auto-loaded and never a resolution failure when absent. The resolver treats
// them as SOFT edges — they order, they do not expand, and they never cycle.
// ---------------------------------------------------------------------------

static int indexOf(const std::vector<std::string>& v, const std::string& name) {
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i] == name) return static_cast<int>(i);
    return -1;
}

TEST_F(DependencyResolverTest, OptionalDependency_NotPulledIntoClosure) {
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    const char* optA[] = {"b"};
    logos_core_register_module_optional_dependencies("a", optA, 1);

    const char* names[] = {"a"};
    char** result = logos_core_resolve_dependencies(names, 1);
    auto v = resolvedToVec(result);
    freeResolved(result);

    // The whole point: requesting `a` must not bring `b` up. Lifetime is the
    // caller's to manage.
    EXPECT_EQ(v.size(), 1u);
    EXPECT_EQ(indexOf(v, "a"), 0);
    EXPECT_EQ(indexOf(v, "b"), -1) << "an optional dependency must not extend the closure";
}

// ---------------------------------------------------------------------------
// Best-effort optional loading (OptionalLoad::BestEffort).
//
// The same edges, one difference: they EXPAND the closure when the optional
// dependency is installed. What they still never do is fail — absent is
// skipped, and the caller is handed the set whose load failure it must swallow.
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, BestEffort_PullsInAnInstalledOptionalDependency) {
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    const char* optA[] = {"b"};
    logos_core_register_module_optional_dependencies("a", optA, 1);

    const char* names[] = {"a"};
    char** result = logos_core_resolve_dependencies_best_effort(names, 1);
    auto v = resolvedToVec(result);
    freeResolved(result);

    ASSERT_EQ(v.size(), 2u) << "an INSTALLED optional dependency joins the closure";
    // ...and ahead of its dependent, so the dependent's startup calls land.
    EXPECT_LT(indexOf(v, "b"), indexOf(v, "a"));
}

TEST_F(DependencyResolverTest, BestEffort_SkipsAnUninstalledOptionalDependencyInSilence) {
    logos_core_register_module("a", "/a");
    const char* optA[] = {"ghost"};
    logos_core_register_module_optional_dependencies("a", optA, 1);

    const char* names[] = {"a"};
    char** result = logos_core_resolve_dependencies_best_effort(names, 1);
    auto v = resolvedToVec(result);
    freeResolved(result);

    // Not installed is not missing. If this ever reports `ghost`, resolution
    // fails and the dependent does not load — the exact outcome the optional
    // kind exists to prevent.
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], "a");
}

TEST_F(DependencyResolverTest, BestEffort_ReportsWhatIsTolerableToFail) {
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    const char* optA[] = {"b"};
    logos_core_register_module_optional_dependencies("a", optA, 1);

    const char* names[] = {"a"};
    char** result = logos_core_resolve_best_effort_names(names, 1);
    auto v = resolvedToVec(result);
    freeResolved(result);

    // `b` is in the order only because it happened to be installed, so its
    // failure must not fail the load of `a`.
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], "b");
}

TEST_F(DependencyResolverTest, BestEffort_RequiredWinsWhenAModuleIsBothKinds) {
    // c is REQUIRED by a and merely optional to b. Reached either way, it is
    // required — a naive implementation that tags nodes as the queue reaches
    // them would call it tolerable whenever b happened to be visited first,
    // and a genuinely required dependency's failure would stop being fatal.
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    logos_core_register_module("c", "/c");
    const char* depsA[] = {"c"};
    logos_core_register_module_dependencies("a", depsA, 1);
    const char* optB[] = {"c"};
    logos_core_register_module_optional_dependencies("b", optB, 1);

    const char* names[] = {"a", "b"};
    char** result = logos_core_resolve_best_effort_names(names, 2);
    auto v = resolvedToVec(result);
    freeResolved(result);

    EXPECT_EQ(indexOf(v, "c"), -1) << "c is required by a; its failure stays fatal";
}

TEST_F(DependencyResolverTest, BestEffort_StillBreaksCyclesRatherThanReportingThem) {
    // a requires b; b optionally depends on a — the shape optional deps exist
    // to express. Expanding the closure must not turn it into a cycle.
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    const char* depsA[] = {"b"};
    logos_core_register_module_dependencies("a", depsA, 1);
    const char* optB[] = {"a"};
    logos_core_register_module_optional_dependencies("b", optB, 1);

    const char* names[] = {"a"};
    char** result = logos_core_resolve_dependencies_best_effort(names, 1);
    auto v = resolvedToVec(result);
    freeResolved(result);

    ASSERT_EQ(v.size(), 2u) << "resolution must still succeed";
    EXPECT_LT(indexOf(v, "b"), indexOf(v, "a")) << "the hard edge decides the order";
}

// Transitive shape: what an optional dependency drags in behind it.
TEST_F(DependencyResolverTest, BestEffort_PullsTheRequiredDepsOfAnOptionalDep) {
    // app -opt-> extra -req-> helper
    logos_core_register_module("app", "/app");
    logos_core_register_module("extra", "/extra");
    logos_core_register_module("helper", "/helper");
    const char* optApp[] = {"extra"};
    logos_core_register_module_optional_dependencies("app", optApp, 1);
    const char* depsExtra[] = {"helper"};
    logos_core_register_module_dependencies("extra", depsExtra, 1);

    const char* names[] = {"app"};
    char** result = logos_core_resolve_dependencies_best_effort(names, 1);
    auto v = resolvedToVec(result);
    freeResolved(result);

    ASSERT_EQ(v.size(), 3u) << "an optional dep brings its own required deps";
    EXPECT_LT(indexOf(v, "helper"), indexOf(v, "extra"));
}

TEST_F(DependencyResolverTest, BestEffort_ReachesAnOptionalDepOfAnOptionalDep) {
    // app -opt-> extra -opt-> deeper. The second hop is only reachable after
    // `extra` joins the set, so this is what the fixed-point loop is for.
    logos_core_register_module("app", "/app");
    logos_core_register_module("extra", "/extra");
    logos_core_register_module("deeper", "/deeper");
    const char* optApp[] = {"extra"};
    logos_core_register_module_optional_dependencies("app", optApp, 1);
    const char* optExtra[] = {"deeper"};
    logos_core_register_module_optional_dependencies("extra", optExtra, 1);

    const char* names[] = {"app"};
    char** result = logos_core_resolve_dependencies_best_effort(names, 1);
    auto v = resolvedToVec(result);
    freeResolved(result);

    EXPECT_EQ(v.size(), 3u) << "best effort is transitive through optional edges";
    EXPECT_NE(indexOf(v, "deeper"), -1);
}

TEST_F(DependencyResolverTest, BestEffort_AnUnsatisfiableBranchIsDroppedWhole) {
    // app -opt-> extra -req-> ghost(absent). `extra` is installed, but it
    // cannot run, so admitting it would only produce a load that fails.
    logos_core_register_module("app", "/app");
    logos_core_register_module("extra", "/extra");
    const char* optApp[] = {"extra"};
    logos_core_register_module_optional_dependencies("app", optApp, 1);
    const char* depsExtra[] = {"ghost"};
    logos_core_register_module_dependencies("extra", depsExtra, 1);

    const char* names[] = {"app"};
    char** result = logos_core_resolve_dependencies_best_effort(names, 1);
    auto v = resolvedToVec(result);
    freeResolved(result);

    ASSERT_EQ(v.size(), 1u) << "the whole branch goes, not just the missing leaf";
    EXPECT_EQ(v[0], "app");
}

TEST_F(DependencyResolverTest, BestEffort_ReportsAnUninstalledOptionalDependency) {
    logos_core_register_module("app", "/app");
    const char* optApp[] = {"ghost"};
    logos_core_register_module_optional_dependencies("app", optApp, 1);

    char* json = logos_core_optional_load_report("app");
    const std::string report(json ? json : "");
    delete[] json;

    EXPECT_NE(report.find("\"module\":\"ghost\""), std::string::npos) << report;
    EXPECT_NE(report.find("\"named_by\":\"app\""), std::string::npos) << report;
    EXPECT_NE(report.find("\"reason\":\"not_installed\""), std::string::npos) << report;
    // Nothing to blame but the module itself, so no third name is invented.
    EXPECT_EQ(report.find("\"missing\""), std::string::npos) << report;
}

TEST_F(DependencyResolverTest, BestEffort_ReportsWhichRequirementMadeABranchUnsatisfiable) {
    logos_core_register_module("app", "/app");
    logos_core_register_module("extra", "/extra");
    const char* optApp[] = {"extra"};
    logos_core_register_module_optional_dependencies("app", optApp, 1);
    const char* depsExtra[] = {"ghost"};
    logos_core_register_module_dependencies("extra", depsExtra, 1);

    char* json = logos_core_optional_load_report("app");
    const std::string report(json ? json : "");
    delete[] json;

    // The useful half: not just that `extra` was left out, but that `ghost` is
    // why — otherwise a reader has to re-derive the branch by hand.
    EXPECT_NE(report.find("\"module\":\"extra\""), std::string::npos) << report;
    EXPECT_NE(report.find("\"reason\":\"unsatisfiable\""), std::string::npos) << report;
    EXPECT_NE(report.find("\"missing\":\"ghost\""), std::string::npos) << report;
}

TEST_F(DependencyResolverTest, BestEffort_ReportsNothingWhenEveryBranchIsTaken) {
    logos_core_register_module("app", "/app");
    logos_core_register_module("extra", "/extra");
    const char* optApp[] = {"extra"};
    logos_core_register_module_optional_dependencies("app", optApp, 1);

    char* json = logos_core_optional_load_report("app");
    const std::string report(json ? json : "");
    delete[] json;

    EXPECT_EQ(report, "[]") << "a report that always says something says nothing";
}

TEST_F(DependencyResolverTest, BestEffort_AnOptionalDepWithAMissingRequiredDepIsNotFatal) {
    // app -opt-> extra -req-> ghost(absent). `extra` cannot be loaded, so it
    // should be left out — but NOTHING here may fail the load of `app`, which
    // merely named `extra` as optional.
    logos_core_register_module("app", "/app");
    logos_core_register_module("extra", "/extra");
    const char* optApp[] = {"extra"};
    logos_core_register_module_optional_dependencies("app", optApp, 1);
    const char* depsExtra[] = {"ghost"};
    logos_core_register_module_dependencies("extra", depsExtra, 1);

    const char* names[] = {"app"};
    char** result = logos_core_resolve_dependencies_best_effort(names, 1);
    auto v = resolvedToVec(result);
    freeResolved(result);

    EXPECT_NE(indexOf(v, "app"), -1) << "app must still resolve";
    EXPECT_EQ(indexOf(v, "ghost"), -1) << "an absent module is never in the order";
}

TEST_F(DependencyResolverTest, OptionalDependency_AbsentIsNotAFailure) {
    logos_core_register_module("a", "/a");
    const char* optA[] = {"never_installed"};
    logos_core_register_module_optional_dependencies("a", optA, 1);

    const char* names[] = {"a"};
    char** result = logos_core_resolve_dependencies(names, 1);
    auto v = resolvedToVec(result);
    freeResolved(result);

    // An unknown REQUIRED dependency empties the order (resolution failed); an
    // unknown optional one must leave it intact.
    EXPECT_EQ(v.size(), 1u);
    EXPECT_EQ(indexOf(v, "a"), 0);
}

TEST_F(DependencyResolverTest, OptionalDependency_OrderedFirstWhenBothRequested) {
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    const char* optA[] = {"b"};
    logos_core_register_module_optional_dependencies("a", optA, 1);

    const char* names[] = {"a", "b"};
    char** result = logos_core_resolve_dependencies(names, 2);
    auto v = resolvedToVec(result);
    freeResolved(result);

    ASSERT_EQ(v.size(), 2u);
    EXPECT_LT(indexOf(v, "b"), indexOf(v, "a"))
        << "an optional dependency in the same batch should come up first, so "
           "the dependent's startup calls land";
}

TEST_F(DependencyResolverTest, OptionalDependency_MayCloseACycleWithoutFailing) {
    // a requires b; b optionally depends on a. This is exactly the shape an
    // author uses optional dependencies to express, so it must resolve.
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    const char* depsA[] = {"b"};
    logos_core_register_module_dependencies("a", depsA, 1);
    const char* optB[] = {"a"};
    logos_core_register_module_optional_dependencies("b", optB, 1);

    const char* names[] = {"a"};
    char** result = logos_core_resolve_dependencies(names, 1);
    auto v = resolvedToVec(result);
    freeResolved(result);

    // Both present, and ordered by the REQUIRED edge — the soft edge that
    // would have closed the cycle is dropped, not reported.
    ASSERT_EQ(v.size(), 2u);
    EXPECT_LT(indexOf(v, "b"), indexOf(v, "a"));
}

TEST_F(DependencyResolverTest, OptionalDependencies_ReadBackFromTheRegistry) {
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    const char* depsA[] = {"b"};
    logos_core_register_module_dependencies("a", depsA, 1);
    const char* optA[] = {"b"};
    logos_core_register_module_optional_dependencies("a", optA, 1);

    char** required = logos_core_get_module_dependencies("a", false);
    char** optional = logos_core_get_module_optional_dependencies_test("a");
    auto req = resolvedToVec(required);
    auto opt = resolvedToVec(optional);
    freeResolved(required);
    freeResolved(optional);

    // Two edge sets, read separately. A caller that merged them would
    // reintroduce every distinction the split exists to keep.
    EXPECT_EQ(req, std::vector<std::string>{"b"});
    EXPECT_EQ(opt, std::vector<std::string>{"b"});
}
