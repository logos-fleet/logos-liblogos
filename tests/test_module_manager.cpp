#include <gtest/gtest.h>
#include "logos_core.h"
#include "logos_core/dependency_gate.h"
#include "logos_core/module_state_observer.h"
#include "qt_test_adapter.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void clearModuleState() {
    logos_core_terminate_all();
    logos_core_clear();
}

// RAII temporary directory (uses mkdtemp, cleaned up on destruction)
struct TmpDir {
    fs::path path;

    TmpDir() {
        std::string tmpl = (fs::temp_directory_path() / "logos_test_XXXXXX").string();
        char* buf = new char[tmpl.size() + 1];
        memcpy(buf, tmpl.c_str(), tmpl.size() + 1);
        if (!mkdtemp(buf)) {
            delete[] buf;
            throw std::runtime_error("mkdtemp failed");
        }
        path = buf;
        delete[] buf;
    }

    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    bool isValid() const { return fs::is_directory(path); }

    // Returns path.string().c_str()-compatible value as std::string
    std::string str() const { return path.string(); }
};

static void createFakeModule(const fs::path& parentDir,
                             const std::string& moduleName,
                             const std::string& mainFile,
                             const std::string& type = "core",
                             const std::vector<std::string>& dependencies = {}) {
    fs::path moduleDir = parentDir / moduleName;
    fs::create_directories(moduleDir);

    nlohmann::json manifest;
    manifest["name"] = moduleName;
    manifest["version"] = "1.0.0";
    manifest["type"] = type;
    manifest["main"] = mainFile;
    manifest["description"] = "Fake test module";
    if (!dependencies.empty())
        manifest["dependencies"] = dependencies;

    std::ofstream mf(moduleDir / "manifest.json");
    mf << manifest.dump();
    mf.close();

    std::ofstream bf(moduleDir / mainFile);
    bf << "fake";
    bf.close();
}

// Helpers to free null-terminated char** arrays returned by the C API.
static void freeStringArray(char** arr) {
    if (!arr) return;
    for (int i = 0; arr[i] != nullptr; ++i)
        delete[] arr[i];
    delete[] arr;
}

static int stringArrayLen(char** arr) {
    if (!arr) return 0;
    int n = 0;
    while (arr[n]) ++n;
    return n;
}

static std::set<std::string> stringArrayToSet(char** arr) {
    std::set<std::string> s;
    if (!arr) return s;
    for (int i = 0; arr[i]; ++i)
        s.insert(arr[i]);
    return s;
}

class ModuleManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        clearModuleState();
    }

    void TearDown() override {
        clearModuleState();
    }
};

// =============================================================================
// Module Query Functions Tests
// =============================================================================

TEST_F(ModuleManagerTest, GetLoadedModules_ReturnsEmptyList) {
    char** result = logos_core_get_loaded_modules();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);
    delete[] result;
}

TEST_F(ModuleManagerTest, GetKnownModules_ReturnsEmptyHash) {
    char** result = logos_core_get_known_modules();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);
    delete[] result;
}

TEST_F(ModuleManagerTest, GetKnownModules_ReturnsCorrectHash) {
    logos_core_register_module("module1", "/path/to/module1.dylib");
    logos_core_register_module("module2", "/path/to/module2.dylib");

    char** result = logos_core_get_known_modules();
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(stringArrayLen(result), 2);

    auto moduleSet = stringArrayToSet(result);
    EXPECT_TRUE(moduleSet.count("module1"));
    EXPECT_TRUE(moduleSet.count("module2"));

    char* path1 = logos_core_get_module_path("module1");
    char* path2 = logos_core_get_module_path("module2");
    ASSERT_NE(path1, nullptr);
    ASSERT_NE(path2, nullptr);
    EXPECT_EQ(std::string(path1), "/path/to/module1.dylib");
    EXPECT_EQ(std::string(path2), "/path/to/module2.dylib");
    delete[] path1;
    delete[] path2;
    freeStringArray(result);
}

// logos_core_get_modules_info returns one rich JSON entry per known module:
// name, path, loaded flag, direct dependencies, direct dependents, and the
// embedded metadata. (Registered fake modules have no plugin file, so their
// metadata is null — the real-plugin metadata is covered separately.)
TEST_F(ModuleManagerTest, GetModulesInfo_ReturnsRichEntryPerModule) {
    logos_core_register_module("module_a", "/path/to/module_a.dylib");
    logos_core_register_module("module_b", "/path/to/module_b.dylib");
    const char* depsA[] = {"module_b"};
    logos_core_register_module_dependencies("module_a", depsA, 1);
    logos_core_mark_module_loaded("module_b");

    char* json = logos_core_get_modules_info();
    ASSERT_NE(json, nullptr);
    nlohmann::json info = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    free(json);

    ASSERT_TRUE(info.is_array());
    ASSERT_EQ(info.size(), 2u);

    auto find = [&](const std::string& n) -> nlohmann::json {
        for (const auto& e : info)
            if (e.value("name", std::string{}) == n) return e;
        return nlohmann::json();
    };

    nlohmann::json a = find("module_a");
    ASSERT_FALSE(a.is_null());
    EXPECT_EQ(a.value("path", std::string{}), "/path/to/module_a.dylib");
    EXPECT_FALSE(a.value("loaded", true));
    // Not loaded ⇒ loaded_at is 0.
    EXPECT_EQ(a.value("loaded_at", int64_t{-1}), 0);
    ASSERT_TRUE(a["dependencies"].is_array());
    ASSERT_EQ(a["dependencies"].size(), 1u);
    EXPECT_EQ(a["dependencies"][0].get<std::string>(), "module_b");
    EXPECT_TRUE(a["dependents"].is_array());
    EXPECT_TRUE(a["dependents"].empty());
    // metadata key is always present; null for a registered (un-processed) module.
    ASSERT_TRUE(a.contains("metadata"));
    EXPECT_TRUE(a["metadata"].is_null());

    nlohmann::json b = find("module_b");
    ASSERT_FALSE(b.is_null());
    EXPECT_TRUE(b.value("loaded", false));
    // Loaded ⇒ loaded_at is a real timestamp (stamped at markLoaded).
    EXPECT_GT(b.value("loaded_at", int64_t{0}), 0);
    // module_a depends on module_b ⇒ module_b lists module_a as a dependent.
    ASSERT_TRUE(b["dependents"].is_array());
    ASSERT_EQ(b["dependents"].size(), 1u);
    EXPECT_EQ(b["dependents"][0].get<std::string>(), "module_a");
}

TEST_F(ModuleManagerTest, GetModulesInfo_EmptyWhenNoModules) {
    char* json = logos_core_get_modules_info();
    ASSERT_NE(json, nullptr);
    nlohmann::json info = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    free(json);
    ASSERT_TRUE(info.is_array());
    EXPECT_TRUE(info.empty());
}

TEST_F(ModuleManagerTest, IsModuleLoaded_ReturnsFalseForUnloaded) {
    EXPECT_EQ(logos_core_is_module_loaded("nonexistent_module"), 0);
}

TEST_F(ModuleManagerTest, IsModuleKnown_ReturnsFalseForUnknown) {
    EXPECT_EQ(logos_core_is_module_known("nonexistent_module"), 0);
}

TEST_F(ModuleManagerTest, IsModuleKnown_ReturnsTrueForKnown) {
    logos_core_register_module("test_module", "/path/to/module");

    EXPECT_EQ(logos_core_is_module_known("test_module"), 1);
}

// =============================================================================
// C String Array Functions Tests
// =============================================================================

TEST_F(ModuleManagerTest, GetLoadedModulesCStr_ReturnsNullTerminatedArrayWhenEmpty) {
    char** result = logos_core_get_loaded_modules();

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);

    delete[] result;
}

TEST_F(ModuleManagerTest, GetKnownModulesCStr_ReturnsNullTerminatedArrayWhenEmpty) {
    char** result = logos_core_get_known_modules();

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);

    delete[] result;
}

TEST_F(ModuleManagerTest, GetKnownModulesCStr_ReturnsCorrectArray) {
    logos_core_register_module("module1", "/path/to/module1");
    logos_core_register_module("module2", "/path/to/module2");

    char** result = logos_core_get_known_modules();

    ASSERT_NE(result, nullptr);
    ASSERT_NE(result[0], nullptr);
    ASSERT_NE(result[1], nullptr);
    EXPECT_EQ(result[2], nullptr);

    auto modules = stringArrayToSet(result);
    EXPECT_TRUE(modules.count("module1"));
    EXPECT_TRUE(modules.count("module2"));

    freeStringArray(result);
}

// =============================================================================
// loadModule Error Cases Tests
// =============================================================================

TEST_F(ModuleManagerTest, LoadModule_ReturnsFalseForUnknownModule) {
    int result = logos_core_load_module("nonexistent_module", LOGOS_LOAD_MODULE_ONLY);
    EXPECT_EQ(result, 0);
}

// =============================================================================
// unloadModule Error Cases Tests
// =============================================================================

TEST_F(ModuleManagerTest, UnloadModule_ReturnsFalseForNotLoaded) {
    int result = logos_core_unload_module("nonexistent_module", false);
    EXPECT_EQ(result, 0);
}

// =============================================================================
// resolveDependencies Function Tests
// =============================================================================

TEST_F(ModuleManagerTest, ResolveDependencies_ReturnsEmptyForEmptyInput) {
    char** result = logos_core_resolve_dependencies(nullptr, 0);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);
    delete[] result;
}

TEST_F(ModuleManagerTest, ResolveDependencies_ReturnsEmptyForUnknownModule) {
    const char* names[] = {"unknown_module"};
    char** result = logos_core_resolve_dependencies(names, 1);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);
    delete[] result;
}

TEST_F(ModuleManagerTest, ResolveDependencies_ReturnsSingleModuleWithNoDeps) {
    logos_core_register_module("module_a", "/path/to/module_a");
    logos_core_register_module_dependencies("module_a", nullptr, 0);

    const char* names[] = {"module_a"};
    char** result = logos_core_resolve_dependencies(names, 1);

    ASSERT_EQ(stringArrayLen(result), 1);
    EXPECT_EQ(std::string(result[0]), "module_a");

    freeStringArray(result);
}

TEST_F(ModuleManagerTest, ResolveDependencies_ReturnsCorrectOrder) {
    logos_core_register_module("module_a", "/path/to/module_a");
    logos_core_register_module("module_b", "/path/to/module_b");
    const char* depsA[] = {"module_b"};
    logos_core_register_module_dependencies("module_a", depsA, 1);
    logos_core_register_module_dependencies("module_b", nullptr, 0);

    const char* names[] = {"module_a"};
    char** result = logos_core_resolve_dependencies(names, 1);

    ASSERT_EQ(stringArrayLen(result), 2);
    EXPECT_EQ(std::string(result[0]), "module_b");
    EXPECT_EQ(std::string(result[1]), "module_a");

    freeStringArray(result);
}

TEST_F(ModuleManagerTest, ResolveDependencies_HandlesTransitiveDeps) {
    logos_core_register_module("module_a", "/path/to/module_a");
    logos_core_register_module("module_b", "/path/to/module_b");
    logos_core_register_module("module_c", "/path/to/module_c");
    const char* depsA[] = {"module_b"};
    const char* depsB[] = {"module_c"};
    logos_core_register_module_dependencies("module_a", depsA, 1);
    logos_core_register_module_dependencies("module_b", depsB, 1);
    logos_core_register_module_dependencies("module_c", nullptr, 0);

    const char* names[] = {"module_a"};
    char** result = logos_core_resolve_dependencies(names, 1);

    ASSERT_EQ(stringArrayLen(result), 3);
    EXPECT_EQ(std::string(result[0]), "module_c");
    EXPECT_EQ(std::string(result[1]), "module_b");
    EXPECT_EQ(std::string(result[2]), "module_a");

    freeStringArray(result);
}

// =============================================================================
// C API: logos_core_load_module LOGOS_LOAD_REQUIRED_DEPS Tests
// =============================================================================

TEST_F(ModuleManagerTest, LoadModuleWithDeps_AbortsForNull) {
    EXPECT_DEATH(logos_core_load_module(nullptr, LOGOS_LOAD_REQUIRED_DEPS), "");
}

TEST_F(ModuleManagerTest, LoadModuleWithDeps_ReturnsZeroForUnknown) {
    int result = logos_core_load_module("unknown_module", LOGOS_LOAD_REQUIRED_DEPS);
    EXPECT_EQ(result, 0);
}

// =============================================================================
// Idempotent-load contract: "already loaded ⇒ success"
//
// logos_core_load_module is an "ensure loaded" guard, not a "load fresh"
// command. Pinning this in tests so the contract documented in
// logos_core.h doesn't quietly regress — basecamp's PluginLoader and
// logoscore-cli's load-module both rely on calling it against modules
// the loader may have already brought up at startup, and we don't want
// a future refactor to start returning 0 in that case (which previously
// caused UI-plugin loads to abort when a core dep was pre-loaded).
//
// We exercise this without a loader: register fake modules, mark them
// loaded via the registry adapter, then call the C entry point. The
// short-circuit at the top of ModuleManager::loadModuleInternal never
// reaches the descriptor / loader path, so no subprocess is spawned.
// =============================================================================

TEST_F(ModuleManagerTest, LoadModule_ReturnsTrueWhenAlreadyLoaded) {
    logos_core_register_module("preloaded", "/fake/path");
    logos_core_mark_module_loaded("preloaded");
    ASSERT_EQ(logos_core_is_module_loaded("preloaded"), 1);

    // First call: module is already loaded ⇒ no-op success.
    EXPECT_EQ(logos_core_load_module("preloaded", LOGOS_LOAD_MODULE_ONLY), 1)
        << "loading an already-loaded module must return 1 (no-op success)";

    // Repeating the call must stay idempotent — still success, still loaded.
    EXPECT_EQ(logos_core_load_module("preloaded", LOGOS_LOAD_MODULE_ONLY), 1);
    EXPECT_EQ(logos_core_is_module_loaded("preloaded"), 1);
}

TEST_F(ModuleManagerTest, LoadModuleWithDeps_ReturnsTrueWhenAllAlreadyLoaded) {
    // Build a tiny dep graph: parent → child. Both pre-marked loaded.
    logos_core_register_module("parent", "/fake/parent");
    logos_core_register_module("child",  "/fake/child");
    const char* deps[] = {"child"};
    logos_core_register_module_dependencies("parent", deps, 1);
    logos_core_mark_module_loaded("child");
    logos_core_mark_module_loaded("parent");

    // LOGOS_LOAD_REQUIRED_DEPS walks the resolved order and calls
    // loadModuleInternal for each; every step short-circuits on
    // isLoaded() and returns true, so the overall call returns 1.
    EXPECT_EQ(logos_core_load_module("parent", LOGOS_LOAD_REQUIRED_DEPS), 1)
        << "LOGOS_LOAD_REQUIRED_DEPS must return 1 when the target and "
           "all of its deps were already loaded before the call";
    EXPECT_EQ(logos_core_is_module_loaded("parent"), 1);
    EXPECT_EQ(logos_core_is_module_loaded("child"),  1);
}

// LOGOS_LOAD_REQUIRED_AND_OPTIONAL: the whole point is that a failure here is
// not a failure of the call. These two run together — the second is what makes
// the first mean anything, since a test that tolerates everything tolerates
// nothing in particular.
TEST_F(ModuleManagerTest, BestEffort_AFailingOptionalDependencyDoesNotFailTheLoad) {
    logos_core_register_module("app", "/fake/app");
    logos_core_register_module("extra", "/fake/extra");
    const char* optApp[] = {"extra"};
    logos_core_register_module_optional_dependencies("app", optApp, 1);
    // `app` itself is already up, so the only load attempted is `extra`'s —
    // and there is no plugin at /fake/extra, so it fails.
    logos_core_mark_module_loaded("app");

    EXPECT_EQ(logos_core_load_module("app", LOGOS_LOAD_REQUIRED_AND_OPTIONAL), 1)
        << "an INSTALLED optional dependency that fails to load must not fail "
           "the load of the module that merely names it";
    EXPECT_EQ(logos_core_is_module_loaded("app"), 1);
    EXPECT_EQ(logos_core_is_module_loaded("extra"), 0)
        << "it really did fail — otherwise this test proves nothing";
}

TEST_F(ModuleManagerTest, BestEffort_AnOptionalDepWhoseOwnRequiredDepIsMissingIsNotFatal) {
    // app -opt-> extra -req-> ghost(not installed).
    // `extra` is unloadable, but `app` only NAMED it as optional. Nothing here
    // may fail app's load — the resolver reports `ghost` as missing, and
    // missing is what loadModuleWithDependencies treats as a hard failure.
    logos_core_register_module("app", "/fake/app");
    logos_core_register_module("extra", "/fake/extra");
    const char* optApp[] = {"extra"};
    logos_core_register_module_optional_dependencies("app", optApp, 1);
    const char* depsExtra[] = {"ghost"};
    logos_core_register_module_dependencies("extra", depsExtra, 1);
    logos_core_mark_module_loaded("app");

    EXPECT_EQ(logos_core_load_module("app", LOGOS_LOAD_REQUIRED_AND_OPTIONAL), 1)
        << "an optional dependency that cannot be satisfied must be left out, "
           "not turned into a resolution failure for the module naming it";
    EXPECT_EQ(logos_core_is_module_loaded("app"), 1);
}

TEST_F(ModuleManagerTest, BestEffort_TheSameFailureIsStillFatalForARequiredDependency) {
    logos_core_register_module("app", "/fake/app");
    logos_core_register_module("extra", "/fake/extra");
    const char* deps[] = {"extra"};
    logos_core_register_module_dependencies("app", deps, 1);
    logos_core_mark_module_loaded("app");

    // Identical fixture, one word changed: `extra` is REQUIRED. The tolerance
    // above is specific to the optional kind, not a test that cannot fail.
    EXPECT_EQ(logos_core_load_module("app", LOGOS_LOAD_REQUIRED_AND_OPTIONAL), 0)
        << "a REQUIRED dependency failing must still fail the load, even when "
           "best-effort optional loading is asked for";
}

// =============================================================================
// Dependency resolution failure: logos_core_load_module(name, LOGOS_LOAD_REQUIRED_DEPS) must
// return 0 when the dependency graph cannot be fully resolved.
//
// The resolver silently drops unknown modules and detects cycles. Before
// this fix, loadModuleWithDependencies only checked whether the *target*
// appeared in the (possibly partial) resolved order — it didn't verify
// the resolution was clean. A module whose transitive dependency was
// unknown would load successfully, violating the contract in logos_core.h
// ("returns 0 when dependency resolution fails").
// =============================================================================

TEST_F(ModuleManagerTest, LoadModuleWithDeps_FailsWhenDirectDependencyUnknown) {
    logos_core_register_module("parent", "/fake/parent");
    const char* deps[] = {"unknown_dep"};
    logos_core_register_module_dependencies("parent", deps, 1);

    // "unknown_dep" is not registered → resolution has missing deps → fail.
    EXPECT_EQ(logos_core_load_module("parent", LOGOS_LOAD_REQUIRED_DEPS), 0)
        << "must return 0 when a direct dependency is unknown";
}

TEST_F(ModuleManagerTest, LoadModuleWithDeps_FailsWhenTransitiveDependencyUnknown) {
    logos_core_register_module("top", "/fake/top");
    logos_core_register_module("mid", "/fake/mid");
    const char* depsTop[] = {"mid"};
    const char* depsMid[] = {"bottom_unknown"};
    logos_core_register_module_dependencies("top", depsTop, 1);
    logos_core_register_module_dependencies("mid", depsMid, 1);

    // "bottom_unknown" not registered → transitive resolution fails.
    EXPECT_EQ(logos_core_load_module("top", LOGOS_LOAD_REQUIRED_DEPS), 0)
        << "must return 0 when a transitive dependency is unknown";
}

TEST_F(ModuleManagerTest, LoadModuleWithDeps_FailsOnCircularDependency) {
    logos_core_register_module("cyc_a", "/fake/cyc_a");
    logos_core_register_module("cyc_b", "/fake/cyc_b");
    const char* depsA[] = {"cyc_b"};
    const char* depsB[] = {"cyc_a"};
    logos_core_register_module_dependencies("cyc_a", depsA, 1);
    logos_core_register_module_dependencies("cyc_b", depsB, 1);

    // Cycle detected → must return 0.
    EXPECT_EQ(logos_core_load_module("cyc_a", LOGOS_LOAD_REQUIRED_DEPS), 0)
        << "must return 0 when a circular dependency is detected";
}

// =============================================================================
// Module Directory Management Tests
// =============================================================================

TEST_F(ModuleManagerTest, AddModulesDir_SetsFirstDirectory) {
    logos_core_add_modules_dir("/tmp/test_modules");
    ASSERT_EQ(logos_core_get_modules_dirs_count(), 1);
    char* dir = logos_core_get_modules_dir_at(0);
    ASSERT_NE(dir, nullptr);
    EXPECT_EQ(std::string(dir), "/tmp/test_modules");
    delete[] dir;
}

TEST_F(ModuleManagerTest, AddModulesDir_AppendsDirectory) {
    logos_core_add_modules_dir("/tmp/dir1");
    logos_core_add_modules_dir("/tmp/dir2");
    logos_core_add_modules_dir("/tmp/dir3");

    ASSERT_EQ(logos_core_get_modules_dirs_count(), 3);

    char* d0 = logos_core_get_modules_dir_at(0);
    char* d1 = logos_core_get_modules_dir_at(1);
    char* d2 = logos_core_get_modules_dir_at(2);
    ASSERT_NE(d0, nullptr);
    ASSERT_NE(d1, nullptr);
    ASSERT_NE(d2, nullptr);
    EXPECT_EQ(std::string(d0), "/tmp/dir1");
    EXPECT_EQ(std::string(d1), "/tmp/dir2");
    EXPECT_EQ(std::string(d2), "/tmp/dir3");
    delete[] d0;
    delete[] d1;
    delete[] d2;
}

TEST_F(ModuleManagerTest, GetModulesDirs_ReturnsEmptyAfterClear) {
    logos_core_add_modules_dir("/tmp/dir1");
    clearModuleState();
    EXPECT_EQ(logos_core_get_modules_dirs_count(), 0);
}

// =============================================================================
// Discovery Tests — fake installed modules
// =============================================================================

TEST_F(ModuleManagerTest, DiscoverInstalledModules_DoesNotCrashWithEmptyDir) {
    TmpDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    logos_core_add_modules_dir(tmpDir.str().c_str());
    logos_core_refresh_modules();

    char** known = logos_core_get_known_modules();
    ASSERT_NE(known, nullptr);
    EXPECT_EQ(known[0], nullptr);
    delete[] known;
}

TEST_F(ModuleManagerTest, DiscoverInstalledModules_DoesNotCrashWithNonexistentDir) {
    logos_core_add_modules_dir("/tmp/nonexistent_dir_12345");
    logos_core_refresh_modules();

    char** known = logos_core_get_known_modules();
    ASSERT_NE(known, nullptr);
    EXPECT_EQ(known[0], nullptr);
    delete[] known;
}

TEST_F(ModuleManagerTest, DiscoverInstalledModules_FindsFakeModulesWithoutCrash) {
    TmpDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    createFakeModule(tmpDir.path, "fake_module_a", "fake_module_a_plugin.so");
    createFakeModule(tmpDir.path, "fake_module_b", "fake_module_b_plugin.so");

    logos_core_add_modules_dir(tmpDir.str().c_str());
    logos_core_refresh_modules();

    char** known = logos_core_get_known_modules();
    ASSERT_NE(known, nullptr);
    EXPECT_EQ(known[0], nullptr);
    delete[] known;
}

TEST_F(ModuleManagerTest, DiscoverInstalledModules_IgnoresModulesWithoutManifest) {
    TmpDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    fs::path moduleDir = tmpDir.path / "no_manifest_module";
    fs::create_directories(moduleDir);
    std::ofstream bf(moduleDir / "plugin.so");
    bf << "fake";
    bf.close();

    logos_core_add_modules_dir(tmpDir.str().c_str());
    logos_core_refresh_modules();

    char** known = logos_core_get_known_modules();
    ASSERT_NE(known, nullptr);
    EXPECT_EQ(known[0], nullptr);
    delete[] known;
}

TEST_F(ModuleManagerTest, DiscoverInstalledModules_IgnoresUiTypeModules) {
    TmpDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    createFakeModule(tmpDir.path, "ui_module", "ui_module_plugin.so", "ui");

    logos_core_add_modules_dir(tmpDir.str().c_str());
    logos_core_refresh_modules();

    char** known = logos_core_get_known_modules();
    ASSERT_NE(known, nullptr);
    EXPECT_EQ(known[0], nullptr);
    delete[] known;
}

TEST_F(ModuleManagerTest, DiscoverInstalledModules_MultipleDirectories) {
    TmpDir tmpDir1;
    TmpDir tmpDir2;
    ASSERT_TRUE(tmpDir1.isValid());
    ASSERT_TRUE(tmpDir2.isValid());

    createFakeModule(tmpDir1.path, "module_in_dir1", "module_in_dir1_plugin.so");
    createFakeModule(tmpDir2.path, "module_in_dir2", "module_in_dir2_plugin.so");

    logos_core_add_modules_dir(tmpDir1.str().c_str());
    logos_core_add_modules_dir(tmpDir2.str().c_str());

    ASSERT_EQ(logos_core_get_modules_dirs_count(), 2);

    logos_core_refresh_modules();

    char** known = logos_core_get_known_modules();
    ASSERT_NE(known, nullptr);
    EXPECT_EQ(known[0], nullptr);
    delete[] known;
}

TEST_F(ModuleManagerTest, DiscoverInstalledModules_InvalidManifestJson) {
    TmpDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    fs::path moduleDir = tmpDir.path / "bad_manifest_module";
    fs::create_directories(moduleDir);
    std::ofstream mf(moduleDir / "manifest.json");
    mf << "{ this is not valid json }}}";
    mf.close();

    logos_core_add_modules_dir(tmpDir.str().c_str());
    logos_core_refresh_modules();

    char** known = logos_core_get_known_modules();
    ASSERT_NE(known, nullptr);
    EXPECT_EQ(known[0], nullptr);
    delete[] known;
}

// =============================================================================
// Loaded-flag preservation across re-registration
// =============================================================================

TEST_F(ModuleManagerTest, RegisterModule_PreservesLoadedFlagOnReregister) {
    logos_core_register_module("test_module", "/path/v1");
    logos_core_mark_module_loaded("test_module");
    ASSERT_EQ(logos_core_is_module_loaded("test_module"), 1);

    logos_core_register_module("test_module", "/path/v2");

    EXPECT_EQ(logos_core_is_module_loaded("test_module"), 1)
        << "Re-registering a known module must preserve its loaded flag";

    char* path = logos_core_get_module_path("test_module");
    ASSERT_NE(path, nullptr);
    EXPECT_EQ(std::string(path), "/path/v2");
    delete[] path;
}

TEST_F(ModuleManagerTest, RegisterDependencies_PreservesLoadedFlag) {
    logos_core_register_module("test_module", "/path/to/module");
    logos_core_mark_module_loaded("test_module");
    ASSERT_EQ(logos_core_is_module_loaded("test_module"), 1);

    const char* deps[] = {"dep_a", "dep_b"};
    logos_core_register_module_dependencies("test_module", deps, 2);

    EXPECT_EQ(logos_core_is_module_loaded("test_module"), 1)
        << "Updating dependencies must not wipe the loaded flag";
    EXPECT_EQ(logos_core_get_module_dependencies_count("test_module"), 2);
}

// =============================================================================
// Dependency version-range gate
//
// A module declares each dependency either as a bare name or as
// { name, version, signer }. The declared range is checked against the
// installed dependency's version before the loader is ever consulted; an
// unsatisfied or unevaluatable range refuses the load exactly the way the
// protocol gate refuses an incompatible major.
//
// The refusal cases run through the real logos_core_load_module and are read
// back off the lifecycle observer, which is where a refusal is reported. The
// permissive cases are asserted on the gate itself, because a load that gets
// PAST the gate goes on to spawn a module host — not something these tests
// can (or should) drive with a placeholder file on disk.
// =============================================================================

class DependencyGateLoadTest : public ::testing::Test {
protected:
    void SetUp() override {
        clearModuleState();
        auto& o = logos::ModuleStateObserver::instance();
        o.setSink({});
        o.clearPending();
        seen.clear();
        o.setSink([this](const std::vector<logos::ModuleTransition>& batch) {
            for (const auto& t : batch) seen.push_back(t);
        });
    }

    void TearDown() override {
        auto& o = logos::ModuleStateObserver::instance();
        o.setSink({});
        o.clearPending();
        clearModuleState();
    }

    // `app` depends on `lib`, which is installed at `installedVersion`.
    void plantGraph(const std::string& range, const std::string& installedVersion) {
        logos_core_register_module("lib", "/path/to/lib");
        ModuleManager::registry().registerModuleVersion("lib", installedVersion);
        logos_core_register_module("app", (tmpDir.path / "app_plugin.so").string().c_str());
        ModuleManager::registry().registerDependencies(
            "app", std::vector<LogosCore::ModuleDependency>{{"lib", range, ""}});
        std::ofstream f(tmpDir.path / "app_plugin.so");
        f << "not a real plugin";
    }

    // The reason recorded on the loading -> error edge for `module`, or "".
    std::string errorReason(const std::string& module) const {
        for (const auto& t : seen) {
            if (t.module == module && t.newState == logos::module_state::kError)
                return t.reason.value_or(std::string{});
        }
        return {};
    }

    TmpDir tmpDir;
    std::vector<logos::ModuleTransition> seen;
};

TEST_F(DependencyGateLoadTest, UnsatisfiedRange_RefusesLoad) {
    plantGraph("^2.0.0", "1.0.0");

    EXPECT_EQ(logos_core_load_module("app", LOGOS_LOAD_MODULE_ONLY), 0);
    EXPECT_EQ(logos_core_is_module_loaded("app"), 0);

    const std::string reason = errorReason("app");
    EXPECT_NE(reason.find("lib"), std::string::npos) << reason;
    EXPECT_NE(reason.find("^2.0.0"), std::string::npos) << reason;
    EXPECT_NE(reason.find("1.0.0"), std::string::npos) << reason;
}

TEST_F(DependencyGateLoadTest, MalformedRange_RefusesLoad) {
    plantGraph("^^2.0.0", "2.1.0");

    EXPECT_EQ(logos_core_load_module("app", LOGOS_LOAD_MODULE_ONLY), 0);

    const std::string reason = errorReason("app");
    EXPECT_NE(reason.find("unparseable"), std::string::npos) << reason;
}

TEST_F(DependencyGateLoadTest, SatisfiedRange_GateAllows) {
    plantGraph("^2.0.0", "2.1.0");

    const auto gate = ModuleManager::dependencyGateFor("app");
    EXPECT_EQ(gate.decision, LogosCore::DependencyGateDecision::Allow);
    EXPECT_TRUE(gate.reason.empty());
}

// The string form of a dependency entry keeps behaving exactly as before: an
// edge, no constraint, nothing for the gate to refuse.
TEST_F(DependencyGateLoadTest, BareNameDependency_GateIsUnconstrained) {
    logos_core_register_module("lib", "/path/to/lib");
    logos_core_register_module("app", "/path/to/app");
    const char* deps[] = {"lib"};
    logos_core_register_module_dependencies("app", deps, 1);

    const auto gate = ModuleManager::dependencyGateFor("app");
    EXPECT_EQ(gate.decision, LogosCore::DependencyGateDecision::Unconstrained);
    EXPECT_EQ(logos_core_get_module_dependencies_count("app"), 1);
}

// Carrying constraints must not widen the name-only shapes the graph and the
// modules-info wire format are built on.
TEST_F(DependencyGateLoadTest, ConstraintsDoNotChangeTheNameOnlyViews) {
    plantGraph("^2.0.0", "2.1.0");

    EXPECT_EQ(ModuleManager::registry().moduleDependencies("app"),
              (std::vector<std::string>{"lib"}));
    EXPECT_EQ(ModuleManager::registry().moduleDependents("lib"),
              (std::vector<std::string>{"app"}));

    char* json = logos_core_get_modules_info();
    ASSERT_NE(json, nullptr);
    nlohmann::json info = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    free(json);
    ASSERT_TRUE(info.is_array());
    for (const auto& entry : info) {
        if (entry.value("name", std::string{}) != "app") continue;
        ASSERT_TRUE(entry["dependencies"].is_array());
        EXPECT_EQ(entry["dependencies"], nlohmann::json::array({"lib"}));
    }
}

// =============================================================================
// End-to-end regression tests using a real Qt module.
// =============================================================================

class RealModuleRegistryTest : public ::testing::Test {
protected:
    std::string modulePath;

    void SetUp() override {
        clearModuleState();

        const char* envPlugin = std::getenv("TEST_PLUGIN");
        if (envPlugin && std::strlen(envPlugin) > 0 &&
            fs::exists(envPlugin)) {
            modulePath = envPlugin;
            return;
        }

        GTEST_SKIP() << "No real test module available. "
                     << "Set TEST_PLUGIN env var to a built Qt plugin (.so/.dylib).";
    }

    void TearDown() override {
        clearModuleState();
    }
};

TEST_F(RealModuleRegistryTest, ProcessModule_RegistersRealModule) {
    char* name = logos_core_process_module(modulePath.c_str());
    ASSERT_NE(name, nullptr) << "process_module failed for " << modulePath;
    EXPECT_NE(std::string(name), "");
    EXPECT_EQ(logos_core_is_module_known(name), 1);
    EXPECT_EQ(logos_core_is_module_loaded(name), 0);
    delete[] name;
}

// For a real plugin, get_modules_info must carry the embedded metadata parsed
// straight from the binary (via ModuleLib::LogosModule) — name + version.
TEST_F(RealModuleRegistryTest, GetModulesInfo_PopulatesEmbeddedMetadata) {
    char* name = logos_core_process_module(modulePath.c_str());
    ASSERT_NE(name, nullptr) << "process_module failed for " << modulePath;
    std::string moduleName(name);
    delete[] name;

    char* json = logos_core_get_modules_info();
    ASSERT_NE(json, nullptr);
    nlohmann::json info = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    free(json);

    ASSERT_TRUE(info.is_array());
    nlohmann::json entry;
    for (const auto& e : info)
        if (e.value("name", std::string{}) == moduleName) { entry = e; break; }
    ASSERT_FALSE(entry.is_null()) << "processed module absent from modules-info";

    EXPECT_FALSE(entry.value("path", std::string{}).empty());
    ASSERT_TRUE(entry["metadata"].is_object())
        << "real plugin must yield a non-null metadata object";
    EXPECT_EQ(entry["metadata"].value("name", std::string{}), moduleName);
    EXPECT_FALSE(entry["metadata"].value("version", std::string{}).empty())
        << "built test modules declare a version in metadata.json";
}

// The version a dependent's range is evaluated against is the dependency's own
// embedded stamp, cached at discovery without loading the plugin. Closes the
// loop from a real binary's metadata to a gate decision.
TEST_F(RealModuleRegistryTest, EmbeddedVersionFeedsTheDependencyGate) {
    char* name = logos_core_process_module(modulePath.c_str());
    ASSERT_NE(name, nullptr) << "process_module failed for " << modulePath;
    const std::string dep(name);
    delete[] name;

    const std::string version = ModuleManager::registry().moduleVersion(dep);
    ASSERT_FALSE(version.empty()) << "built test modules declare a version";

    logos_core_register_module("dependent", "/path/to/dependent");
    ModuleManager::registry().registerDependencies(
        "dependent", std::vector<LogosCore::ModuleDependency>{{dep, "=" + version, ""}});
    EXPECT_EQ(ModuleManager::dependencyGateFor("dependent").decision,
              LogosCore::DependencyGateDecision::Allow);

    ModuleManager::registry().registerDependencies(
        "dependent", std::vector<LogosCore::ModuleDependency>{{dep, "=" + version + "-nope.1", ""}});
    EXPECT_EQ(ModuleManager::dependencyGateFor("dependent").decision,
              LogosCore::DependencyGateDecision::Refuse);
}

// =============================================================================
// The production dependency-range path, end to end from a real binary.
//
// This is the ONLY coverage of it. Every other constraint test injects entries
// through the constraint-carrying registerDependencies overload, which has no
// production caller. The real path is
//     processModuleInternal -> ModuleLib::extractMetadata
//                           -> toGateDependencies -> ModuleInfo::dependencies
//                           -> dependencyGateFor
// and it was measurably untested: dropping the constraints where the registry
// assigns them (keeping only the names) left all 218 tests green, i.e. it
// silently reinstated the bug the gate exists to fix.
//
// It stayed uncovered because no metadata.json in the fleet uses the object
// form, so no shipped module declares a range. tests/fixtures supplies the
// missing input: a real Qt plugin whose embedded blob declares
//     "dependencies": [ { "name": "range_probe_dep", "version": "^9.0.0" } ]
// =============================================================================

namespace {
// Mirrors tests/fixtures/dep_range_fixture.json. A drift here shows up as an
// assertion, not as a test that quietly stops covering anything.
constexpr const char* kFixtureModule = "dep_range_fixture";
constexpr const char* kFixtureDep    = "range_probe_dep";
constexpr const char* kFixtureRange  = "^9.0.0";
}  // namespace

class RealDependencyRangeTest : public ::testing::Test {
protected:
    std::string fixturePath;

    void SetUp() override {
        clearModuleState();

        const char* env = std::getenv("TEST_PLUGIN_DEP_RANGE");
        if (env && std::strlen(env) > 0 && fs::exists(env)) {
            fixturePath = env;
            return;
        }

        // A skipped test renders as a pass, and a pass here would mean the
        // production path is unguarded again. CI sets LOGOS_REQUIRE_TEST_FIXTURES
        // so an absent fixture is a red run; a local dev without one still skips.
        const char* strict = std::getenv("LOGOS_REQUIRE_TEST_FIXTURES");
        if (strict && std::strcmp(strict, "0") != 0) {
            FAIL() << "TEST_PLUGIN_DEP_RANGE is unset or missing (got '"
                   << (env ? env : "") << "') while LOGOS_REQUIRE_TEST_FIXTURES is "
                   << "set: the dependency-range fixture must be supplied, not skipped.";
        }

        GTEST_SKIP() << "No dependency-range fixture available. Set "
                     << "TEST_PLUGIN_DEP_RANGE to tests/fixtures' built plugin.";
    }

    void TearDown() override {
        clearModuleState();
    }

    // Discovers the fixture through the real registry entry point and returns
    // the name it registered under.
    std::string discover() {
        char* name = logos_core_process_module(fixturePath.c_str());
        EXPECT_NE(name, nullptr) << "process_module failed for " << fixturePath;
        if (!name) return {};
        std::string registered(name);
        delete[] name;
        return registered;
    }
};

// The range survives the trip from the plugin's embedded metadata into the
// registry's dependency entries. Fails the moment the registry keeps names and
// drops constraints.
TEST_F(RealDependencyRangeTest, EmbeddedRangeReachesTheRegistry) {
    ASSERT_EQ(discover(), kFixtureModule);

    const auto entries =
        ModuleManager::registry().moduleDependencyEntries(kFixtureModule);
    ASSERT_EQ(entries.size(), 1u) << "fixture declares exactly one dependency";
    EXPECT_EQ(entries[0].name, kFixtureDep);
    EXPECT_EQ(entries[0].versionRange, kFixtureRange)
        << "the range declared in the plugin's embedded metadata was dropped "
        << "before it reached ModuleInfo::dependencies";

    // The name-only view the graph and the modules-info wire shape are built on
    // is unchanged by carrying the constraint.
    EXPECT_EQ(ModuleManager::registry().moduleDependencies(kFixtureModule),
              (std::vector<std::string>{kFixtureDep}));
}

// ...and it decides. An installed dependency outside the declared range must be
// refused; the refusal names the range, so a gate that saw no constraint cannot
// produce this message.
TEST_F(RealDependencyRangeTest, EmbeddedRangeRefusesAnOutOfRangeDependency) {
    ASSERT_EQ(discover(), kFixtureModule);

    logos_core_register_module(kFixtureDep, "/path/to/range_probe_dep");
    ModuleManager::registry().registerModuleVersion(kFixtureDep, "1.2.3");

    const auto gate = ModuleManager::dependencyGateFor(kFixtureModule);
    EXPECT_EQ(gate.decision, LogosCore::DependencyGateDecision::Refuse)
        << "installed 1.2.3 does not satisfy " << kFixtureRange
        << ", so the gate must refuse; decision was "
        << static_cast<int>(gate.decision);
    EXPECT_EQ(gate.dependency, kFixtureDep);
    EXPECT_EQ(gate.range, kFixtureRange);
    EXPECT_EQ(gate.installedVersion, "1.2.3");
    EXPECT_NE(gate.reason.find(kFixtureRange), std::string::npos) << gate.reason;
}

// Positive control on the same fixture: satisfy the range and the gate reports
// Allow, not the Unconstrained a dropped constraint would yield.
TEST_F(RealDependencyRangeTest, EmbeddedRangeAllowsAnInRangeDependency) {
    ASSERT_EQ(discover(), kFixtureModule);

    logos_core_register_module(kFixtureDep, "/path/to/range_probe_dep");
    ModuleManager::registry().registerModuleVersion(kFixtureDep, "9.4.1");

    EXPECT_EQ(ModuleManager::dependencyGateFor(kFixtureModule).decision,
              LogosCore::DependencyGateDecision::Allow)
        << "a satisfied range must read as Allow; Unconstrained here means the "
        << "gate never saw the constraint the plugin declared";
}

// =============================================================================
// The same production path, for a constraint that cannot be READ.
//
// `"version": 2` comes back empty from any string accessor, so a decoder that
// only asks for the string unconstrains the edge and the gate waves it through
// — the precise fail-open ModuleDependency::malformedConstraint exists to
// refuse. The decode itself now lives in logos-module, so this fixture is what
// proves the flag still survives the mapping at liblogos' boundary; break
// toGateDependencies and EmbeddedMalformedConstraintRefuses goes red.
//
// The fixture also carries a bare-name entry alongside the malformed one, so
// the mixed-form array is covered here rather than in a decode unit test.
// =============================================================================

namespace {
// Mirrors tests/fixtures/dep_malformed_fixture.json.
constexpr const char* kMalformedModule  = "dep_malformed_fixture";
constexpr const char* kMalformedPlainDep = "plain_probe_dep";
constexpr const char* kMalformedBadDep   = "malformed_probe_dep";
}  // namespace

class RealMalformedConstraintTest : public ::testing::Test {
protected:
    std::string fixturePath;

    void SetUp() override {
        clearModuleState();

        const char* env = std::getenv("TEST_PLUGIN_DEP_MALFORMED");
        if (env && std::strlen(env) > 0 && fs::exists(env)) {
            fixturePath = env;
            return;
        }

        // A skip renders as a pass, and a pass here would mean the fail-open is
        // unguarded again. See RealDependencyRangeTest for the same contract.
        const char* strict = std::getenv("LOGOS_REQUIRE_TEST_FIXTURES");
        if (strict && std::strcmp(strict, "0") != 0) {
            FAIL() << "TEST_PLUGIN_DEP_MALFORMED is unset or missing (got '"
                   << (env ? env : "") << "') while LOGOS_REQUIRE_TEST_FIXTURES is "
                   << "set: the malformed-constraint fixture must be supplied, "
                   << "not skipped.";
        }

        GTEST_SKIP() << "No malformed-constraint fixture available. Set "
                     << "TEST_PLUGIN_DEP_MALFORMED to tests/fixtures' built plugin.";
    }

    void TearDown() override { clearModuleState(); }

    std::string discover() {
        char* name = logos_core_process_module(fixturePath.c_str());
        EXPECT_NE(name, nullptr) << "process_module failed for " << fixturePath;
        if (!name) return {};
        std::string registered(name);
        delete[] name;
        return registered;
    }
};

// The flag survives the trip from the plugin's embedded metadata into the
// registry's entries, and the bare-name sibling is left unconstrained.
TEST_F(RealMalformedConstraintTest, EmbeddedMalformedConstraintReachesTheRegistry) {
    ASSERT_EQ(discover(), kMalformedModule);

    const auto entries =
        ModuleManager::registry().moduleDependencyEntries(kMalformedModule);
    ASSERT_EQ(entries.size(), 2u) << "fixture declares a bare and an object entry";
    EXPECT_EQ(entries[0].name, kMalformedPlainDep);
    EXPECT_FALSE(entries[0].malformedConstraint);
    EXPECT_TRUE(entries[0].versionRange.empty());
    EXPECT_EQ(entries[1].name, kMalformedBadDep);
    EXPECT_TRUE(entries[1].malformedConstraint)
        << "a non-string `version` reached the registry as an UNCONSTRAINED "
        << "edge — the fail-open this flag exists to refuse";
    EXPECT_TRUE(entries[1].versionRange.empty());
}

// ...and it refuses. Both dependencies are installed and readable, so nothing
// but the unreadable constraint can produce this decision.
TEST_F(RealMalformedConstraintTest, EmbeddedMalformedConstraintRefuses) {
    ASSERT_EQ(discover(), kMalformedModule);

    logos_core_register_module(kMalformedPlainDep, "/path/to/plain_probe_dep");
    ModuleManager::registry().registerModuleVersion(kMalformedPlainDep, "1.0.0");
    logos_core_register_module(kMalformedBadDep, "/path/to/malformed_probe_dep");
    ModuleManager::registry().registerModuleVersion(kMalformedBadDep, "1.0.0");

    const auto gate = ModuleManager::dependencyGateFor(kMalformedModule);
    EXPECT_EQ(gate.decision, LogosCore::DependencyGateDecision::Refuse)
        << "an unreadable constraint must refuse, not unconstrain the edge; "
        << "decision was " << static_cast<int>(gate.decision);
    EXPECT_EQ(gate.dependency, kMalformedBadDep);
    EXPECT_NE(gate.reason.find("not a string"), std::string::npos) << gate.reason;
}

// =============================================================================
// Snapshot listing — the record modules_state receives
// =============================================================================
//
// `type` and `version` shipped hardcoded empty on this wire. They are the
// module's own claims, and the only place they exist is the metadata.json
// embedded in its plugin — which allModulesInfo already parses and hands to the
// snapshot builder. These two tests pin both halves: the real value when the
// blob is there, and "" (present, not omitted) when it is not.

// The record for `module` in a snapshot listing, or a null json when absent.
static nlohmann::json snapshotRecord(const nlohmann::json& listing,
                                     const std::string& module) {
    auto mods = listing.is_object() ? listing.find("modules") : listing.end();
    if (mods == listing.end() || !mods->is_array())
        return nlohmann::json();
    for (const auto& r : *mods)
        if (r.value("module", std::string{}) == module)
            return r;
    return nlohmann::json();
}

TEST_F(ModuleManagerTest, SnapshotListing_EmptyTypeAndVersionWithoutMetadata) {
    // Registered but never processed: no plugin was read, so the registry holds
    // no metadata blob and allModulesInfo reports `metadata` as null.
    logos_core_register_module("module_a", "/path/to/module_a.dylib");

    nlohmann::json listing = nlohmann::json::parse(
        ModuleManager::buildSnapshotListingJson(), nullptr, /*allow_exceptions=*/false);
    nlohmann::json rec = snapshotRecord(listing, "module_a");
    ASSERT_TRUE(rec.is_object()) << "registered module absent from the snapshot";

    // PRESENT AND EMPTY, not omitted: the wire type declares both as `tstr`,
    // and the sentinel default here would survive a missing key.
    EXPECT_EQ(rec.value("type", std::string{"<absent>"}), "");
    EXPECT_EQ(rec.value("version", std::string{"<absent>"}), "");
}

TEST_F(RealModuleRegistryTest, SnapshotListing_CarriesEmbeddedTypeAndVersion) {
    char* name = logos_core_process_module(modulePath.c_str());
    ASSERT_NE(name, nullptr) << "process_module failed for " << modulePath;
    std::string moduleName(name);
    delete[] name;

    // Read what the plugin declares back through the same registry JSON the
    // snapshot is built from, rather than hardcoding the values of whichever
    // module TEST_PLUGIN happens to point at.
    char* json = logos_core_get_modules_info();
    ASSERT_NE(json, nullptr);
    nlohmann::json info = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    free(json);
    ASSERT_TRUE(info.is_array());
    nlohmann::json entry;
    for (const auto& e : info)
        if (e.value("name", std::string{}) == moduleName) { entry = e; break; }
    ASSERT_TRUE(entry.is_object()) << "processed module absent from modules-info";
    ASSERT_TRUE(entry["metadata"].is_object())
        << "real plugin must yield a non-null metadata object";
    const std::string declaredType = entry["metadata"].value("type", std::string{});
    const std::string declaredVersion = entry["metadata"].value("version", std::string{});
    ASSERT_FALSE(declaredVersion.empty())
        << "built test modules declare a version in metadata.json";

    nlohmann::json listing = nlohmann::json::parse(
        ModuleManager::buildSnapshotListingJson(), nullptr, /*allow_exceptions=*/false);
    nlohmann::json rec = snapshotRecord(listing, moduleName);
    ASSERT_TRUE(rec.is_object()) << "processed module absent from the snapshot";

    EXPECT_EQ(rec.value("version", std::string{}), declaredVersion)
        << "the snapshot must carry the module's real version, not \"\"";
    EXPECT_EQ(rec.value("type", std::string{}), declaredType);
}

// =============================================================================
// Security regression: privileged-name impersonation during discovery (F-022).
//
// Module identity used to be taken from the name embedded in the plugin's own
// Qt metadata, ignoring the trusted package name the package manager scanned.
// That let a package installed under an innocuous name ship a binary whose
// embedded metadata claims a privileged name (e.g. "capability_module"), and
// the registry would key the module under that privileged name — wiring the
// attacker's plugin into the impersonated module's token/trust relationships.
//
// The discovery path (logos_core_refresh_modules → discoverInstalledModules)
// must bind identity to the *trusted package name* (InstalledPackage::name)
// and refuse a plugin whose embedded name disagrees.
//
// These tests use the real TEST_PLUGIN as the impersonating payload: we first
// read its real embedded name via the raw process-module path, then plant a
// package whose manifest name differs from it, and assert the embedded name
// never leaks into the registry.
// =============================================================================

class ImpersonationRegistryTest : public ::testing::Test {
protected:
    std::string modulePath;       // real TEST_PLUGIN on disk
    std::string embeddedName;     // the name baked into TEST_PLUGIN's metadata

    void SetUp() override {
        clearModuleState();

        const char* envPlugin = std::getenv("TEST_PLUGIN");
        if (!envPlugin || std::strlen(envPlugin) == 0 || !fs::exists(envPlugin)) {
            GTEST_SKIP() << "No real test module available. "
                         << "Set TEST_PLUGIN env var to a built Qt plugin (.so/.dylib).";
        }
        modulePath = envPlugin;

        // Discover the plugin's self-asserted embedded name via the raw
        // process-module path (which intentionally trusts the embedded name).
        // This is the name an attacker's binary would carry to impersonate.
        char* name = logos_core_process_module(modulePath.c_str());
        ASSERT_NE(name, nullptr) << "process_module failed for " << modulePath;
        embeddedName = name;
        delete[] name;
        ASSERT_FALSE(embeddedName.empty());

        // Wipe the scratch registration + modules dirs so each test below
        // starts from a clean registry.
        clearModuleState();
    }

    void TearDown() override {
        clearModuleState();
    }

    // Plant a package directory named `packageName` whose manifest declares
    // name=packageName but whose main binary is a byte copy of the real
    // TEST_PLUGIN (embedding `embeddedName`).
    void plantPackage(const fs::path& parentDir, const std::string& packageName) {
        const std::string mainFile = packageName + "_plugin.so";
        createFakeModule(parentDir, packageName, mainFile);  // manifest + placeholder
        std::error_code ec;
        fs::copy_file(modulePath, parentDir / packageName / mainFile,
                      fs::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "failed to copy real plugin into package dir: " << ec.message();
    }
};

// The core repro: an "innocent_helper" package carrying a binary that claims
// the privileged embedded name must NOT register under that privileged name,
// and must not silently bind it either. Before the fix the registry keyed the
// module under `embeddedName`, so is_module_known(embeddedName) was 1.
TEST_F(ImpersonationRegistryTest, Discovery_RefusesPrivilegedNameImpersonation) {
    // Only meaningful when the trusted package name differs from the embedded
    // one (true for the capability_module fixture: package "innocent_helper"
    // vs embedded "capability_module").
    const std::string packageName = "innocent_helper";
    ASSERT_NE(packageName, embeddedName);

    TmpDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    plantPackage(tmpDir.path, packageName);

    logos_core_add_modules_dir(tmpDir.str().c_str());
    logos_core_refresh_modules();

    // The impersonated privileged identity must never enter the registry.
    EXPECT_EQ(logos_core_is_module_known(embeddedName.c_str()), 0)
        << "a package must not be able to claim the embedded name '"
        << embeddedName << "' it does not legitimately own";

    // And the lying package is refused outright (its binary's identity does
    // not match its package name), so the innocuous name isn't bound either.
    EXPECT_EQ(logos_core_is_module_known(packageName.c_str()), 0)
        << "a package whose binary impersonates another module must be refused";

    char** known = logos_core_get_known_modules();
    ASSERT_NE(known, nullptr);
    EXPECT_EQ(known[0], nullptr) << "no module should be registered from a lying package";
    freeStringArray(known);
}

// Positive control: an honest package whose manifest name matches the binary's
// embedded name still registers normally. The fix must not break legitimate
// discovery of (even reserved-named) modules installed under their true name.
TEST_F(ImpersonationRegistryTest, Discovery_HonestPackageRegistersUnderItsName) {
    TmpDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    plantPackage(tmpDir.path, embeddedName);  // manifest name == embedded name

    logos_core_add_modules_dir(tmpDir.str().c_str());
    logos_core_refresh_modules();

    EXPECT_EQ(logos_core_is_module_known(embeddedName.c_str()), 1)
        << "an honest package (manifest name == embedded name) must register";

    char* path = logos_core_get_module_path(embeddedName.c_str());
    ASSERT_NE(path, nullptr);
    EXPECT_NE(std::string(path), "");
    delete[] path;
}

// =============================================================================
// Cascading unload: logos_core_unload_module(name, true)
//
// The cascade is exercised without real Qt modules. We:
//   1. Set up fake manifests on disk (PackageManagerLib scan sees the
//      dependency edges).
//   2. Register the same modules directly in ModuleRegistry so it believes
//      they exist (the fake .so files are not loadable Qt plugins, so
//      refresh_modules alone wouldn't populate the registry).
//   3. Register placeholder "processes" + mark loaded so hasProcess() returns
//      true — `terminateProcess` on a placeholder is a no-op but still
//      removes the entry cleanly.
// =============================================================================

class CascadeUnloadTest : public ::testing::Test {
protected:
    TmpDir tmpDir;

    void SetUp() override {
        clearModuleState();
        logos_core_clear_processes();
    }

    void TearDown() override {
        clearModuleState();
        logos_core_clear_processes();
    }

    // Registers a module in both ModuleRegistry and as a loaded fake process.
    void setupLoaded(const std::string& name,
                     const std::vector<std::string>& deps = {}) {
        std::string path = (tmpDir.path / name / (name + "_plugin.so")).string();
        logos_core_register_module(name.c_str(), path.c_str());
        std::vector<const char*> depPtrs;
        depPtrs.reserve(deps.size());
        for (const auto& d : deps) depPtrs.push_back(d.c_str());
        logos_core_register_module_dependencies(
            name.c_str(),
            depPtrs.empty() ? nullptr : depPtrs.data(),
            static_cast<int>(depPtrs.size()));

        logos_core_register_process(name.c_str());
        logos_core_mark_module_loaded(name.c_str());
    }

    void writeManifestsAndScan(
        const std::vector<std::tuple<std::string, std::vector<std::string>>>& modules)
    {
        for (const auto& [name, deps] : modules) {
            createFakeModule(tmpDir.path, name, name + "_plugin.so", "core", deps);
        }
        logos_core_add_modules_dir(tmpDir.str().c_str());
        logos_core_refresh_modules();
    }
};

TEST_F(CascadeUnloadTest, UnloadWithDependents_ReturnsZeroWhenTargetNotLoaded) {
    // Module is known but not loaded.
    logos_core_register_module("foo", "/foo");
    int result = logos_core_unload_module("foo", true);
    EXPECT_EQ(result, 0);
}

TEST_F(CascadeUnloadTest, UnloadWithDependents_NoDependents_UnloadsTargetOnly) {
    // Single loaded module with no dependents on disk → cascade is just the
    // target.
    writeManifestsAndScan({ {"solo", {}} });
    setupLoaded("solo");

    ASSERT_EQ(logos_core_is_module_loaded("solo"), 1);

    int result = logos_core_unload_module("solo", true);
    EXPECT_EQ(result, 1);
    EXPECT_EQ(logos_core_is_module_loaded("solo"), 0);
    EXPECT_EQ(logos_core_has_process("solo"), 0);
}

TEST_F(CascadeUnloadTest, UnloadWithDependents_RecursiveDependentsLeavesFirst) {
    // Graph: a -> b -> c (a depends on b, b depends on c).
    // Unloading c should also bring down b and a, in the order a, b, c.
    writeManifestsAndScan({
        {"c", {}},
        {"b", {"c"}},
        {"a", {"b"}},
    });
    setupLoaded("c", {});
    setupLoaded("b", {"c"});
    setupLoaded("a", {"b"});

    ASSERT_EQ(logos_core_is_module_loaded("a"), 1);
    ASSERT_EQ(logos_core_is_module_loaded("b"), 1);
    ASSERT_EQ(logos_core_is_module_loaded("c"), 1);

    int result = logos_core_unload_module("c", true);
    EXPECT_EQ(result, 1);

    EXPECT_EQ(logos_core_is_module_loaded("a"), 0);
    EXPECT_EQ(logos_core_is_module_loaded("b"), 0);
    EXPECT_EQ(logos_core_is_module_loaded("c"), 0);

    EXPECT_EQ(logos_core_has_process("a"), 0);
    EXPECT_EQ(logos_core_has_process("b"), 0);
    EXPECT_EQ(logos_core_has_process("c"), 0);
}

// An OPTIONAL dependent must SURVIVE its dependency going away. That is the
// half of "lifetime is managed externally" the teardown path owns: a module
// that declared it can tolerate absence is not entitled to be killed when the
// thing it tolerates disappears.
TEST_F(CascadeUnloadTest, UnloadWithDependents_OptionalDependentSurvives) {
    writeManifestsAndScan({{"provider", {}}, {"consumer", {}}});
    setupLoaded("provider", {});
    setupLoaded("consumer", {});

    const char* opt[] = {"provider"};
    logos_core_register_module_optional_dependencies("consumer", opt, 1);

    ASSERT_EQ(logos_core_is_module_loaded("provider"), 1);
    ASSERT_EQ(logos_core_is_module_loaded("consumer"), 1);

    int result = logos_core_unload_module("provider", true);
    EXPECT_EQ(result, 1);

    EXPECT_EQ(logos_core_is_module_loaded("provider"), 0);
    EXPECT_EQ(logos_core_is_module_loaded("consumer"), 1)
        << "an optional dependent must not be cascaded down with its dependency";
}

TEST_F(CascadeUnloadTest, UnloadWithDependents_UnloadedDependentsIgnored) {
    // b depends on c. Only c is loaded; b is known but not loaded. Cascade
    // should only touch c. b stays unloaded (not "failed to unload").
    writeManifestsAndScan({
        {"c", {}},
        {"b", {"c"}},
    });
    setupLoaded("c", {});
    // Register b in registry but don't mark it loaded.
    logos_core_register_module("b", "/b");
    const char* depsB[] = {"c"};
    logos_core_register_module_dependencies("b", depsB, 1);

    int result = logos_core_unload_module("c", true);
    EXPECT_EQ(result, 1);

    EXPECT_EQ(logos_core_is_module_loaded("c"), 0);
    EXPECT_EQ(logos_core_is_module_loaded("b"), 0);
}

TEST_F(CascadeUnloadTest, UnloadWithDependents_DiamondDependents) {
    // Diamond: a -> b -> d ; a -> c -> d. Unloading d should bring down
    // a, b, c in some valid order (a before b and c; b and c before d).
    writeManifestsAndScan({
        {"d", {}},
        {"b", {"d"}},
        {"c", {"d"}},
        {"a", {"b", "c"}},
    });
    setupLoaded("d", {});
    setupLoaded("b", {"d"});
    setupLoaded("c", {"d"});
    setupLoaded("a", {"b", "c"});

    int result = logos_core_unload_module("d", true);
    EXPECT_EQ(result, 1);

    EXPECT_EQ(logos_core_is_module_loaded("a"), 0);
    EXPECT_EQ(logos_core_is_module_loaded("b"), 0);
    EXPECT_EQ(logos_core_is_module_loaded("c"), 0);
    EXPECT_EQ(logos_core_is_module_loaded("d"), 0);
}

TEST_F(CascadeUnloadTest, UnloadWithDependents_AbortsForNull) {
    EXPECT_DEATH(logos_core_unload_module(nullptr, true), "");
}

TEST_F(RealModuleRegistryTest, ProcessModule_PreservesLoadedFlagOnReprocess) {
    char* name1 = logos_core_process_module(modulePath.c_str());
    ASSERT_NE(name1, nullptr) << "process_module failed for " << modulePath;
    std::string modName = name1;
    delete[] name1;

    ASSERT_EQ(logos_core_is_module_known(modName.c_str()), 1);

    logos_core_mark_module_loaded(modName.c_str());
    ASSERT_EQ(logos_core_is_module_loaded(modName.c_str()), 1);

    char* name2 = logos_core_process_module(modulePath.c_str());
    ASSERT_NE(name2, nullptr);
    EXPECT_EQ(std::string(name2), modName);
    delete[] name2;

    EXPECT_EQ(logos_core_is_module_loaded(modName.c_str()), 1)
        << "Re-processing a loaded module must preserve its loaded flag";

    // Verify it still appears in the loaded list
    char** loaded = logos_core_get_loaded_modules();
    auto loadedSet = stringArrayToSet(loaded);
    freeStringArray(loaded);
    EXPECT_TRUE(loadedSet.count(modName))
        << "get_loaded_modules() must still report the module as loaded";
}

// =============================================================================
// Dependency graph queries:
//   logos_core_get_module_dependencies(name, recursive)
//   logos_core_get_module_dependents(name, recursive)
//
// These read from the in-process registry. We populate it with
// logos_core_register_module + logos_core_register_module_dependencies
// (which in turn trigger recomputeDependentsLocked), then check both the
// direct and recursive traversals against known-shaped graphs.
// =============================================================================

class DependencyQueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        clearModuleState();
    }

    void TearDown() override {
        clearModuleState();
    }

    // Register a module with a (possibly empty) direct dependency list. Path
    // value isn't exercised by the queries — anything non-empty is fine.
    void reg(const std::string& name,
             const std::vector<std::string>& deps = {}) {
        logos_core_register_module(name.c_str(), ("/" + name).c_str());
        std::vector<const char*> depPtrs;
        depPtrs.reserve(deps.size());
        for (const auto& d : deps) depPtrs.push_back(d.c_str());
        logos_core_register_module_dependencies(
            name.c_str(),
            depPtrs.empty() ? nullptr : depPtrs.data(),
            static_cast<int>(depPtrs.size()));
    }
};

TEST_F(DependencyQueryTest, GetModuleDependencies_UnknownName_ReturnsEmpty) {
    char** deps = logos_core_get_module_dependencies("ghost", false);
    EXPECT_EQ(stringArrayLen(deps), 0);
    freeStringArray(deps);

    deps = logos_core_get_module_dependencies("ghost", true);
    EXPECT_EQ(stringArrayLen(deps), 0);
    freeStringArray(deps);
}

TEST_F(DependencyQueryTest, GetModuleDependents_UnknownName_ReturnsEmpty) {
    char** d = logos_core_get_module_dependents("ghost", false);
    EXPECT_EQ(stringArrayLen(d), 0);
    freeStringArray(d);

    d = logos_core_get_module_dependents("ghost", true);
    EXPECT_EQ(stringArrayLen(d), 0);
    freeStringArray(d);
}

TEST_F(DependencyQueryTest, GetModuleDependencies_NoDeps_ReturnsEmpty) {
    reg("leaf");
    char** deps = logos_core_get_module_dependencies("leaf", false);
    EXPECT_EQ(stringArrayLen(deps), 0);
    freeStringArray(deps);
    deps = logos_core_get_module_dependencies("leaf", true);
    EXPECT_EQ(stringArrayLen(deps), 0);
    freeStringArray(deps);
}

TEST_F(DependencyQueryTest, GetModuleDependencies_DirectVsRecursive) {
    // Chain: a -> b -> c. Direct deps of a = {b}. Recursive deps of a = {b, c}.
    reg("c");
    reg("b", {"c"});
    reg("a", {"b"});

    char** direct = logos_core_get_module_dependencies("a", false);
    EXPECT_EQ(stringArrayToSet(direct), (std::set<std::string>{"b"}));
    freeStringArray(direct);

    char** recursive = logos_core_get_module_dependencies("a", true);
    EXPECT_EQ(stringArrayToSet(recursive), (std::set<std::string>{"b", "c"}));
    freeStringArray(recursive);
}

TEST_F(DependencyQueryTest, GetModuleDependents_DirectVsRecursive) {
    // Chain: a -> b -> c. Direct dependents of c = {b}. Recursive = {b, a}.
    reg("c");
    reg("b", {"c"});
    reg("a", {"b"});

    char** direct = logos_core_get_module_dependents("c", false);
    EXPECT_EQ(stringArrayToSet(direct), (std::set<std::string>{"b"}));
    freeStringArray(direct);

    char** recursive = logos_core_get_module_dependents("c", true);
    EXPECT_EQ(stringArrayToSet(recursive), (std::set<std::string>{"b", "a"}));
    freeStringArray(recursive);
}

TEST_F(DependencyQueryTest, GetModuleDependencies_Diamond_RecursiveDeduplicates) {
    // Diamond: a -> b -> d ; a -> c -> d. Recursive deps of a must include
    // {b, c, d} with no duplicate entries for d.
    reg("d");
    reg("b", {"d"});
    reg("c", {"d"});
    reg("a", {"b", "c"});

    char** recursive = logos_core_get_module_dependencies("a", true);
    std::set<std::string> got = stringArrayToSet(recursive);
    int n = stringArrayLen(recursive);
    freeStringArray(recursive);

    EXPECT_EQ(got, (std::set<std::string>{"b", "c", "d"}));
    // No duplicate d entries — set and array length must agree.
    EXPECT_EQ(n, static_cast<int>(got.size()));
}

TEST_F(DependencyQueryTest, GetModuleDependents_Diamond_RecursiveDeduplicates) {
    // Same diamond — d has {b, c} as direct dependents and {b, c, a}
    // transitively. The BFS must not report a twice even though both
    // b and c list it as a dependent.
    reg("d");
    reg("b", {"d"});
    reg("c", {"d"});
    reg("a", {"b", "c"});

    char** direct = logos_core_get_module_dependents("d", false);
    EXPECT_EQ(stringArrayToSet(direct), (std::set<std::string>{"b", "c"}));
    freeStringArray(direct);

    char** recursive = logos_core_get_module_dependents("d", true);
    std::set<std::string> got = stringArrayToSet(recursive);
    int n = stringArrayLen(recursive);
    freeStringArray(recursive);
    EXPECT_EQ(got, (std::set<std::string>{"a", "b", "c"}));
    EXPECT_EQ(n, static_cast<int>(got.size()));
}

TEST_F(DependencyQueryTest, GetModuleDependencies_SelfNotIncluded) {
    reg("leaf");
    reg("root", {"leaf"});
    char** recursive = logos_core_get_module_dependencies("root", true);
    std::set<std::string> got = stringArrayToSet(recursive);
    freeStringArray(recursive);
    EXPECT_EQ(got.count("root"), 0u);
    EXPECT_EQ(got, (std::set<std::string>{"leaf"}));
}

TEST_F(DependencyQueryTest, GetModuleDependencies_AbortsForNull) {
    EXPECT_DEATH(logos_core_get_module_dependencies(nullptr, false), "");
}

TEST_F(DependencyQueryTest, GetModuleDependents_AbortsForNull) {
    EXPECT_DEATH(logos_core_get_module_dependents(nullptr, false), "");
}

// =============================================================================
// Derived access-restriction computation (graph + policy -> allowed callers)
// =============================================================================
//
// computeDerivedAllowedCallers() is the registry-backed counterpart of the
// pure derivation seam: it reads the live dependency graph + loaded set + the
// access policy and returns what core would register with capability_module for
// a target — without any RPC. We drive it with the test registry adapters
// (register_module / register_module_dependencies / mark_module_loaded) and the
// ModuleManager::setAccessPolicy entry point.

class DerivedRestrictionsManagerTest : public ::testing::Test {
protected:
    void SetUp() override { clearModuleState(); }
    void TearDown() override {
        // Clear the policy so it doesn't leak into other suites.
        ModuleManager::setAccessPolicy("");
        clearModuleState();
    }

    // Register `name` with `deps` declared as dependencies.
    static void reg(const std::string& name, const std::vector<std::string>& deps) {
        logos_core_register_module(name.c_str(), ("/fake/" + name).c_str());
        std::vector<const char*> d;
        for (const auto& s : deps) d.push_back(s.c_str());
        logos_core_register_module_dependencies(name.c_str(), d.data(),
                                                static_cast<int>(d.size()));
    }

    static std::set<std::string> derived(const std::string& target) {
        auto v = ModuleManager::computeDerivedAllowedCallers(target);
        return std::set<std::string>(v.begin(), v.end());
    }

    // Register `name` with `deps` declared as OPTIONAL dependencies.
    static void regOptional(const std::string& name,
                            const std::vector<std::string>& deps) {
        logos_core_register_module(name.c_str(), ("/fake/" + name).c_str());
        std::vector<const char*> d;
        for (const auto& s : deps) d.push_back(s.c_str());
        logos_core_register_module_optional_dependencies(name.c_str(), d.data(),
                                                         static_cast<int>(d.size()));
    }

    // Minimal enforce policy with no explicit restrictions — turns derivation on.
    static const char* enforceEnvelope() {
        return "{\"version\":1,\"mode\":\"enforce\",\"restrictions\":{}}";
    }
};

// An OPTIONAL dependent is a caller too. The declaration is what grants the
// right to call; whether the loader had to supply the target is a different
// question. Without this the call is refused between two loaded modules and
// the caller sees a default value rather than an error — the exact fail-open
// shape that is hardest to diagnose from the call site.
TEST_F(DerivedRestrictionsManagerTest, LoadedOptionalDependentIsAllowed) {
    reg("b", {});
    regOptional("a", {"b"});
    logos_core_mark_module_loaded("b");
    logos_core_mark_module_loaded("a");
    ModuleManager::setAccessPolicy(enforceEnvelope());

    EXPECT_EQ(derived("b"),
              (std::set<std::string>{"a", "core", "core_service"}));
}

TEST_F(DerivedRestrictionsManagerTest, UnloadedOptionalDependentExcluded) {
    reg("b", {});
    regOptional("a", {"b"});
    logos_core_mark_module_loaded("b");  // a left unloaded
    ModuleManager::setAccessPolicy(enforceEnvelope());

    EXPECT_EQ(derived("b"), (std::set<std::string>{"core", "core_service"}));
}

// Declaring a name in both edge sets must not double-count it into the list.
TEST_F(DerivedRestrictionsManagerTest, RequiredAndOptionalDependentDedupes) {
    reg("b", {});
    logos_core_register_module("a", "/fake/a");
    const char* d[] = {"b"};
    logos_core_register_module_dependencies("a", d, 1);
    logos_core_register_module_optional_dependencies("a", d, 1);
    logos_core_mark_module_loaded("b");
    logos_core_mark_module_loaded("a");
    ModuleManager::setAccessPolicy(enforceEnvelope());

    auto v = ModuleManager::computeDerivedAllowedCallers("b");
    int occurrences = 0;
    for (const auto& c : v) if (c == "a") ++occurrences;
    EXPECT_EQ(occurrences, 1);
}

TEST_F(DerivedRestrictionsManagerTest, LoadedDependentPlusTrusted) {
    // a depends on b; both loaded. b's allowed callers = {a} ∪ trusted.
    reg("b", {});
    reg("a", {"b"});
    logos_core_mark_module_loaded("b");
    logos_core_mark_module_loaded("a");
    ModuleManager::setAccessPolicy(enforceEnvelope());

    EXPECT_EQ(derived("b"),
              (std::set<std::string>{"a", "core", "core_service"}));
}

TEST_F(DerivedRestrictionsManagerTest, UnloadedDependentExcluded) {
    // a declares b but is NOT loaded — a must not appear in b's callers.
    reg("b", {});
    reg("a", {"b"});
    logos_core_mark_module_loaded("b");  // a left unloaded
    ModuleManager::setAccessPolicy(enforceEnvelope());

    EXPECT_EQ(derived("b"), (std::set<std::string>{"core", "core_service"}));
}

TEST_F(DerivedRestrictionsManagerTest, ZeroDependentsIsTrustedOnly) {
    reg("solo", {});
    logos_core_mark_module_loaded("solo");
    ModuleManager::setAccessPolicy(enforceEnvelope());

    EXPECT_EQ(derived("solo"), (std::set<std::string>{"core", "core_service"}));
}

TEST_F(DerivedRestrictionsManagerTest, NoEnforcePolicyDerivesNothing) {
    reg("b", {});
    reg("a", {"b"});
    logos_core_mark_module_loaded("b");
    logos_core_mark_module_loaded("a");
    // No policy set at all -> derivation off -> empty.
    EXPECT_TRUE(derived("b").empty());

    // A non-enforce policy is also inert.
    ModuleManager::setAccessPolicy(
        "{\"version\":1,\"mode\":\"audit\",\"restrictions\":{}}");
    EXPECT_TRUE(derived("b").empty());
}

TEST_F(DerivedRestrictionsManagerTest, ExplicitPolicyOverridesDerived) {
    reg("b", {});
    reg("a", {"b"});
    logos_core_mark_module_loaded("b");
    logos_core_mark_module_loaded("a");
    // Explicit entry for b names only "x" — replaces the derived {a, trusted}.
    ModuleManager::setAccessPolicy(
        "{\"version\":1,\"mode\":\"enforce\",\"restrictions\":{"
        "\"b\":{\"allowedCallers\":[\"x\"]}}}");

    EXPECT_EQ(derived("b"), (std::set<std::string>{"x"}));
}

TEST_F(DerivedRestrictionsManagerTest, ExemptTargetsNeverDerived) {
    reg("capability_module", {});
    logos_core_mark_module_loaded("capability_module");
    ModuleManager::setAccessPolicy(enforceEnvelope());

    EXPECT_TRUE(derived("capability_module").empty());
}

TEST_F(DerivedRestrictionsManagerTest, UnloadDropsDependentFromCallers) {
    reg("b", {});
    reg("a", {"b"});
    logos_core_mark_module_loaded("b");
    logos_core_mark_module_loaded("a");
    ModuleManager::setAccessPolicy(enforceEnvelope());
    EXPECT_TRUE(derived("b").count("a"));

    // Unloading a (it stays known, dependency edge remains) drops it.
    ModuleManager::registry().markUnloaded("a");
    EXPECT_FALSE(derived("b").count("a"));
    EXPECT_EQ(derived("b"), (std::set<std::string>{"core", "core_service"}));
}

TEST_F(DerivedRestrictionsManagerTest, TrustedDependentNotDuplicated) {
    // A loaded dependent that shares a trusted name must appear exactly once in
    // the registered list (the set-based `derived()` helper would hide a dup, so
    // inspect the raw vector here).
    reg("b", {});
    reg("core", {"b"});
    logos_core_mark_module_loaded("b");
    logos_core_mark_module_loaded("core");
    ModuleManager::setAccessPolicy(enforceEnvelope());

    auto callers = ModuleManager::computeDerivedAllowedCallers("b");
    EXPECT_EQ(std::count(callers.begin(), callers.end(), std::string("core")), 1);
}

// ── The deny-by-default switch, both directions ─────────────────────────────
//
// These two are the contract for the operator-facing flag (`mode: "enforce"`,
// reached as `logoscore --access-policy enforce` / `LogosBasecamp
// --access-policy enforce`). They share one scenario deliberately: the SAME
// undeclared pair must be allowed with the flag off and denied with it on, and
// the declared pair must survive the flip. A change that denied everything
// would pass the "denied" half on its own, so the declared-caller assertion is
// the one carrying the weight.
class DenyByDefaultFlagTest : public DerivedRestrictionsManagerTest {
protected:
    // target        — the module being reached
    // declared      — loaded, and declares `target` as a dependency
    // undeclared    — loaded, declares nothing (the shape D-a found in this
    //                 tree: counter_qml calling package_manager with
    //                 "dependencies": [])
    void SetUp() override {
        DerivedRestrictionsManagerTest::SetUp();
        reg("target", {});
        reg("declared", {"target"});
        reg("undeclared", {});
        logos_core_mark_module_loaded("target");
        logos_core_mark_module_loaded("declared");
        logos_core_mark_module_loaded("undeclared");
    }
};

TEST_F(DenyByDefaultFlagTest, FlagOff_UndeclaredCallerStaysUnrestricted) {
    // No policy installed — the default every host has today. Core derives
    // nothing, so it registers NO restriction for `target`, and
    // capability_module's unrestricted-target path leaves `undeclared ->
    // target` working exactly as before.
    EXPECT_TRUE(derived("target").empty());

    // Same for a policy that isn't in enforce mode: still off, still open.
    ModuleManager::setAccessPolicy(
        "{\"version\":1,\"mode\":\"audit\",\"restrictions\":{}}");
    EXPECT_TRUE(derived("target").empty());
}

TEST_F(DenyByDefaultFlagTest, FlagOn_DeclaredCallerAllowed_UndeclaredRefused) {
    ModuleManager::setAccessPolicy(enforceEnvelope());

    const auto callers = derived("target");
    // A restriction IS registered now — that is what makes the target closed.
    ASSERT_FALSE(callers.empty());
    // The declared dependent keeps working…
    EXPECT_TRUE(callers.count("declared"))
        << "enforce must not break a caller that declared the target";
    // …and the undeclared caller is not on the list, so capability_module
    // refuses to mint it a token.
    EXPECT_FALSE(callers.count("undeclared"))
        << "enforce must refuse a caller that never declared the target";
}

TEST_F(DenyByDefaultFlagTest, FlagOn_ExplicitPolicyCanReadmitAnUndeclaredCaller) {
    // The escape hatch an operator needs when a real deployment has a caller
    // that legitimately can't declare its target (out-of-process ui_qml
    // plugins, for one): an explicit entry replaces the derived list verbatim.
    ModuleManager::setAccessPolicy(
        "{\"version\":1,\"mode\":\"enforce\",\"restrictions\":{"
        "\"target\":{\"allowedCallers\":[\"declared\",\"undeclared\"]}}}");

    const auto callers = derived("target");
    EXPECT_TRUE(callers.count("declared"));
    EXPECT_TRUE(callers.count("undeclared"));
}

TEST_F(DenyByDefaultFlagTest, FlagIsReversible) {
    // Clearing the policy must restore today's behaviour byte-for-byte, not
    // leave a latched restriction behind (hosts call setAccessPolicy once per
    // boot, but a restart in the same process must not inherit enforcement).
    ModuleManager::setAccessPolicy(enforceEnvelope());
    ASSERT_FALSE(derived("target").empty());

    ModuleManager::setAccessPolicy("");
    EXPECT_TRUE(derived("target").empty());
}
