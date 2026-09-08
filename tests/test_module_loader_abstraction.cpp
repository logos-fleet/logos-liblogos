// =============================================================================
// Tests for the ModuleLoader abstraction seam.
//
// Installs a FakeModuleLoader into ModuleManager's ModuleLoaderRegistry and drives the
// full ModuleManager load/unload/terminateAll path. Proves that:
//   - load(), sendToken(), terminate(), terminateAll() are routed through the
//     loader abstraction (not directly to a subprocess or Qt mechanism).
//   - Dependency-ordered loads call load() in the correct (topo) order.
//   - Error paths (load returns false) prevent sendToken from being called.
// No child processes are spawned; no Qt Remote Objects are used.
// =============================================================================
#include <gtest/gtest.h>
#include "logos_core.h"
#include "qt_test_adapter.h"
#include "module_manager.h"
#include "module_registry.h"
#include "module_loader_registry.h"
#include "module_loader.h"
#include "subprocess_manager.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>

using namespace LogosCore;

// ---------------------------------------------------------------------------
// FakeModuleLoader: records all calls; configurable per-module load result.
// Placed in an anonymous namespace to avoid ODR conflicts with the FakeModuleLoader
// stub in test_module_loader_registry.cpp (same binary, different definition).
// ---------------------------------------------------------------------------
namespace {

struct FakeModuleLoader : public ModuleLoader {
    std::string id() const override { return "fake"; }

    bool canHandle(const ModuleDescriptor&) const override { return true; }

    bool load(const ModuleDescriptor& desc,
              std::function<void(const std::string&)>,
              LoadedModuleHandle& out) override {
        loadCalls.push_back(desc.name);
        if (failOn.count(desc.name)) return false;
        out.name = desc.name;
        out.pid  = 1234;
        out.endpoint = "fake://" + desc.name;
        activeModules.insert(desc.name);
        return true;
    }

    bool sendToken(const std::string& name, const std::string& token) override {
        sendTokenCalls.push_back({name, token});
        return true;
    }

    void terminate(const std::string& name) override {
        terminateCalls.push_back(name);
        activeModules.erase(name);
    }

    void terminateAll() override {
        terminateAllCount++;
        activeModules.clear();
    }

    bool hasModule(const std::string& name) const override {
        return activeModules.count(name) > 0;
    }

    // Call records
    std::vector<std::string>                         loadCalls;
    std::vector<std::pair<std::string,std::string>>  sendTokenCalls;
    std::vector<std::string>                         terminateCalls;
    int                                              terminateAllCount = 0;

    // Modules to fail on load
    std::unordered_set<std::string>                  failOn;
    // Modules currently "running"
    std::unordered_set<std::string>                  activeModules;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Test fixture: installs FakeModuleLoader, cleans up registry after each test.
// ---------------------------------------------------------------------------

class ModuleLoaderAbstractionTest : public ::testing::Test {
protected:
    std::shared_ptr<FakeModuleLoader> fake;

    void SetUp() override {
        logos_core_terminate_all();
        logos_core_clear();
        SubprocessManager::clearAll();

        fake = std::make_shared<FakeModuleLoader>();
        ModuleManager::loaders().clearForTests();
        ModuleManager::loaders().registerLoader(fake);
    }

    void TearDown() override {
        logos_core_terminate_all();
        logos_core_clear();
        SubprocessManager::clearAll();
        // Restore default loader so other test suites aren't affected.
        ModuleManager::loaders().clearForTests();
        ModuleManager::loaders().registerLoader(
            std::make_shared<SubprocessManager>());
    }

    void registerModule(const std::string& name,
                        const std::vector<std::string>& deps = {}) {
        std::string path = "/fake/" + name + "_plugin.so";
        logos_core_register_module(name.c_str(), path.c_str());
        std::vector<const char*> depPtrs;
        for (const auto& d : deps) depPtrs.push_back(d.c_str());
        logos_core_register_module_dependencies(
            name.c_str(),
            depPtrs.empty() ? nullptr : depPtrs.data(),
            static_cast<int>(depPtrs.size()));
    }
};

// =============================================================================
// Basic load/unload routing
// =============================================================================

TEST_F(ModuleLoaderAbstractionTest, LoadModule_CallsFakeModuleLoaderLoad) {
    registerModule("foo");

    int result = logos_core_load_module("foo", LOGOS_LOAD_MODULE_ONLY);
    ASSERT_EQ(result, 1);

    ASSERT_EQ(fake->loadCalls.size(), 1u);
    EXPECT_EQ(fake->loadCalls[0], "foo");
}

TEST_F(ModuleLoaderAbstractionTest, LoadModule_CallsSendTokenAfterLoad) {
    registerModule("foo");

    logos_core_load_module("foo", LOGOS_LOAD_MODULE_ONLY);

    ASSERT_EQ(fake->sendTokenCalls.size(), 1u);
    EXPECT_EQ(fake->sendTokenCalls[0].first, "foo");
    EXPECT_FALSE(fake->sendTokenCalls[0].second.empty());
}

TEST_F(ModuleLoaderAbstractionTest, LoadModule_MarksModuleAsLoaded) {
    registerModule("foo");

    logos_core_load_module("foo", LOGOS_LOAD_MODULE_ONLY);

    EXPECT_EQ(logos_core_is_module_loaded("foo"), 1);
}

TEST_F(ModuleLoaderAbstractionTest, LoadModule_StoresLoaderInRegistry) {
    registerModule("foo");
    logos_core_load_module("foo", LOGOS_LOAD_MODULE_ONLY);

    auto rt = ModuleManager::registry().loaderFor("foo");
    EXPECT_EQ(rt.get(), fake.get());
}

TEST_F(ModuleLoaderAbstractionTest, UnloadModule_CallsFakeModuleLoaderTerminate) {
    registerModule("foo");
    logos_core_load_module("foo", LOGOS_LOAD_MODULE_ONLY);

    int result = logos_core_unload_module("foo", false);
    ASSERT_EQ(result, 1);

    ASSERT_EQ(fake->terminateCalls.size(), 1u);
    EXPECT_EQ(fake->terminateCalls[0], "foo");
}

TEST_F(ModuleLoaderAbstractionTest, UnloadModule_MarksModuleAsUnloaded) {
    registerModule("foo");
    logos_core_load_module("foo", LOGOS_LOAD_MODULE_ONLY);
    logos_core_unload_module("foo", false);

    EXPECT_EQ(logos_core_is_module_loaded("foo"), 0);
}

// =============================================================================
// Dependency-ordered loads
// =============================================================================

TEST_F(ModuleLoaderAbstractionTest, LoadWithDeps_LoadsInTopologicalOrder) {
    // Chain: c depends on b, b depends on a.
    // Expected load order: a, b, c.
    registerModule("a");
    registerModule("b", {"a"});
    registerModule("c", {"b"});

    int result = logos_core_load_module("c", LOGOS_LOAD_REQUIRED_DEPS);
    ASSERT_EQ(result, 1);

    ASSERT_EQ(fake->loadCalls.size(), 3u);
    EXPECT_EQ(fake->loadCalls[0], "a");
    EXPECT_EQ(fake->loadCalls[1], "b");
    EXPECT_EQ(fake->loadCalls[2], "c");
}

TEST_F(ModuleLoaderAbstractionTest, LoadWithDeps_SkipsAlreadyLoadedModules) {
    registerModule("a");
    registerModule("b", {"a"});

    logos_core_load_module("a", LOGOS_LOAD_MODULE_ONLY);
    fake->loadCalls.clear();

    logos_core_load_module("b", LOGOS_LOAD_REQUIRED_DEPS);

    ASSERT_EQ(fake->loadCalls.size(), 1u);
    EXPECT_EQ(fake->loadCalls[0], "b");
}

// LoadsInTopologicalOrder (above) pins the *call sequence*. This one pins the
// *observable end state*: requesting a single top-level module with
// LOGOS_LOAD_REQUIRED_DEPS must leave that module's entire transitive
// dependency closure loaded, as reported by the public query API. This is the
// guarantee callers actually rely on ("load app, get everything it needs"),
// and it exercises a diamond (app → ui, core; ui → core) so a dependency
// reachable by two paths is still loaded exactly once and not skipped.
TEST_F(ModuleLoaderAbstractionTest, LoadWithDeps_LeavesTransitiveClosureLoaded) {
    registerModule("core");
    registerModule("ui",  {"core"});
    registerModule("app", {"ui", "core"});

    // Precondition: nothing in the closure is loaded yet.
    ASSERT_EQ(logos_core_is_module_loaded("app"),  0);
    ASSERT_EQ(logos_core_is_module_loaded("ui"),   0);
    ASSERT_EQ(logos_core_is_module_loaded("core"), 0);

    // Only the top module is requested.
    int result = logos_core_load_module("app", LOGOS_LOAD_REQUIRED_DEPS);
    ASSERT_EQ(result, 1);

    // The whole closure ends up loaded — the deps were auto-resolved.
    EXPECT_EQ(logos_core_is_module_loaded("app"),  1);
    EXPECT_EQ(logos_core_is_module_loaded("ui"),   1);
    EXPECT_EQ(logos_core_is_module_loaded("core"), 1);

    // The diamond's shared dependency loads exactly once despite two paths.
    int coreLoads = 0;
    for (const auto& n : fake->loadCalls)
        if (n == "core") ++coreLoads;
    EXPECT_EQ(coreLoads, 1);
}

// =============================================================================
// terminateAll routing
// =============================================================================

TEST_F(ModuleLoaderAbstractionTest, TerminateAll_CallsFakeTerminateAll) {
    registerModule("foo");
    logos_core_load_module("foo", LOGOS_LOAD_MODULE_ONLY);

    logos_core_terminate_all();

    EXPECT_EQ(fake->terminateAllCount, 1);
    EXPECT_EQ(logos_core_is_module_loaded("foo"), 0);
}

// =============================================================================
// Error paths
// =============================================================================

TEST_F(ModuleLoaderAbstractionTest, LoadModule_ReturnsFalseWhenLoaderLoadFails) {
    registerModule("bad");
    fake->failOn.insert("bad");

    int result = logos_core_load_module("bad", LOGOS_LOAD_MODULE_ONLY);
    EXPECT_EQ(result, 0);
}

TEST_F(ModuleLoaderAbstractionTest, LoadModule_DoesNotCallSendTokenOnLoadFailure) {
    registerModule("bad");
    fake->failOn.insert("bad");

    logos_core_load_module("bad", LOGOS_LOAD_MODULE_ONLY);

    EXPECT_TRUE(fake->sendTokenCalls.empty());
}

TEST_F(ModuleLoaderAbstractionTest, LoadModule_DoesNotMarkAsLoadedOnFailure) {
    registerModule("bad");
    fake->failOn.insert("bad");

    logos_core_load_module("bad", LOGOS_LOAD_MODULE_ONLY);

    EXPECT_EQ(logos_core_is_module_loaded("bad"), 0);
}

TEST_F(ModuleLoaderAbstractionTest, LoadModule_ReturnsFalseForUnknownModule) {
    int result = logos_core_load_module("not_registered", LOGOS_LOAD_MODULE_ONLY);
    EXPECT_EQ(result, 0);
    EXPECT_TRUE(fake->loadCalls.empty());
}
