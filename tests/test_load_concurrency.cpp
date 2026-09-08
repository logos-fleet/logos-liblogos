// Which loads may run at the same time, and which may not.
//
// One global loadMutex() guards the whole load path, so today every load is
// serialized against every other one regardless of which module it names.
// Bringing up a module is dominated by waiting for the child to report its
// verdict — tens of milliseconds warm, hundreds cold — and a frontend that
// loads several modules at startup pays that sum on one thread.
//
// The two halves are a pair, and the second is what stops the first from being
// "fixed" by removing the lock: DIFFERENT modules must overlap, the SAME module
// must not. Both read the marks the stand-in hosts left rather than anything
// the caller was told -- for the same module because the defect being guarded
// against is exactly a second child nobody asked for, and for different
// modules because the caller's stopwatch times the machine as much as the lock.
//
// The third case is a load inside a load, on ONE thread, which the same locks
// forbid for a different reason — see LoadReentrancyTest below.

#include "fake_module_host.h"

#include "module_loader.h"
#include "module_loader_registry.h"
#include "module_manager.h"
#include "subprocess_manager.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>

namespace {

class LoadConcurrencyTest : public FakeHostFixture {
protected:
    void SetUp() override {
        FakeHostFixture::SetUp();
        useFakeHost();
        // NO SINK, and it is not incidental. armReadinessWatch only arms when
        // one is installed, and arming it builds a LogosAPIClient and its
        // replica on the calling thread — here a worker that exits as soon as
        // the load returns, leaving Qt objects owned by a dead thread ("timers
        // can only be used with threads started with QThread", then a teardown
        // that never finishes). What these two tests pin is which loads may
        // overlap; the feed is covered by the load-verdict tests.
        logos::ModuleStateObserver::instance().setSink({});
    }
};

// Two modules that have nothing to do with each other must be able to come up
// at the same time. Each stand-in host stalls before reporting, so the question
// is whether the second host was started while the first was still inside that
// stall: overlapped it was, serialized it cannot have been, because serialized
// the second host does not exist until the first has reported and its load has
// returned.
//
// Asked of the hosts' own marks, NOT of a stopwatch, and the difference is the
// whole point of the rewrite. The obvious form — assert the pair finished in
// under two stalls, on the reasoning that two sequential one-second sleeps
// cannot — is sound about the lock and wrong about the measurement, because it
// also measures the machine. On a loaded host a single load took ~2.1 s, so a
// pair that plainly HAD overlapped (22 ms between the two "loaded" lines, where
// serialized would be a full stall apart) still ran 4.2 s and failed a 2 s
// budget. Every budget has that failure somewhere; widening it only moves it.
// Two marks in one file cannot be reordered by load, so this has no budget.
TEST_F(LoadConcurrencyTest, DifferentModulesLoadConcurrently) {
    plantModule("alpha", "slow-ok");
    plantModule("beta", "slow-ok");

    std::thread a([] { logos_core_load_module("alpha", LOGOS_LOAD_MODULE_ONLY); });
    std::thread b([] { logos_core_load_module("beta", LOGOS_LOAD_MODULE_ONLY); });
    a.join();
    b.join();

    EXPECT_TRUE(logos_core_is_module_loaded("alpha"));
    EXPECT_TRUE(logos_core_is_module_loaded("beta"));

    const HostWindow alpha = hostWindow("alpha");
    const HostWindow beta = hostWindow("beta");
    ASSERT_GE(alpha.reported, 0) << "alpha's host never reported, so there is "
                                    "no window to overlap:" << hostEventLog();
    ASSERT_GE(beta.reported, 0) << "beta's host never reported, so there is "
                                   "no window to overlap:" << hostEventLog();

    // The two windows intersect: each host had started before the other one
    // reported. Both directions, since either alone is satisfied by a run where
    // one host came and went entirely before the other started.
    EXPECT_LT(alpha.entered, beta.reported)
        << "alpha's host did not start until beta had already reported, so the "
           "two loads were serialized:" << hostEventLog();
    EXPECT_LT(beta.entered, alpha.reported)
        << "beta's host did not start until alpha had already reported, so the "
           "two loads were serialized:" << hostEventLog();
}

// Two dependency CHAINS that share a dependency. Each caller asks for its own
// target with_dependencies, so both resolve the same shared_dep first — and it
// must be brought up once, by whichever gets there first, with the other
// answered by the already-loaded no-op inside the same lock.
TEST_F(LoadConcurrencyTest, ChainsSharingADependencyBringItUpOnce) {
    plantModule("shared_dep", "slow-ok");
    plantModule("alpha_app", "slow-ok");
    plantModule("beta_app", "slow-ok");

    const char* dep[] = { "shared_dep" };
    logos_core_register_module_dependencies("alpha_app", dep, 1);
    logos_core_register_module_dependencies("beta_app", dep, 1);

    std::atomic<int> succeeded{0};
    std::thread a([&] { succeeded += logos_core_load_module("alpha_app", LOGOS_LOAD_REQUIRED_DEPS); });
    std::thread b([&] { succeeded += logos_core_load_module("beta_app", LOGOS_LOAD_REQUIRED_DEPS); });
    a.join();
    b.join();

    EXPECT_EQ(succeeded.load(), 2);
    EXPECT_TRUE(logos_core_is_module_loaded("shared_dep"));
    EXPECT_TRUE(logos_core_is_module_loaded("alpha_app"));
    EXPECT_TRUE(logos_core_is_module_loaded("beta_app"));

    EXPECT_EQ(spawnCount("shared_dep"), 1);
    EXPECT_EQ(spawnCount("alpha_app"), 1);
    EXPECT_EQ(spawnCount("beta_app"), 1);
}

// The same module named by two callers at once is one load, not two. Both
// callers are told it is up — "already loaded" is a successful no-op, which
// callers rely on as "ensure loaded" — but only one host may be started.
TEST_F(LoadConcurrencyTest, SameModuleLoadsOnlyOnce) {
    plantModule("solo", "slow-ok");

    std::atomic<int> succeeded{0};
    std::thread a([&] { succeeded += logos_core_load_module("solo", LOGOS_LOAD_MODULE_ONLY); });
    std::thread b([&] { succeeded += logos_core_load_module("solo", LOGOS_LOAD_MODULE_ONLY); });
    a.join();
    b.join();

    EXPECT_EQ(succeeded.load(), 2);
    EXPECT_TRUE(logos_core_is_module_loaded("solo"));
    EXPECT_EQ(spawnCount("solo"), 1);
}

}  // namespace

// ── A load inside a load ────────────────────────────────────────────────────
//
// Not concurrency: one thread, re-entering. It happens because a call out to
// capability_module spins a nested Qt event loop, so a load a frontend posted
// with a queued connection can be delivered inside one already running.
// ModuleManager must refuse rather than proceed — proceeding takes fleetMutex's
// SHARED lock recursively, which is undefined behaviour and deadlocks against a
// queued writer, and capabilityRpcMutex, which is not recursive at all.
//
// Driven through the ModuleLoader seam because that is where the manager is
// holding those locks: load() is called with fleet shared and the module's own
// lock held, which is exactly the state a nested delivery arrives in.

namespace {

class ReenteringLoader : public LogosCore::ModuleLoader {
public:
    std::string id() const override { return "reentering"; }
    bool canHandle(const LogosCore::ModuleDescriptor&) const override { return true; }

    bool load(const LogosCore::ModuleDescriptor& desc,
              std::function<void(const std::string&)>,
              LogosCore::LoadedModuleHandle& out) override {
        if (!inner.empty() && !reentered) {
            reentered = true;
            innerResult = logos_core_load_module(inner.c_str(), LOGOS_LOAD_MODULE_ONLY);
        }
        out.name = desc.name;
        out.pid = 4321;
        active.insert(desc.name);
        return true;
    }

    bool sendToken(const std::string&, const std::string&) override { return true; }
    void terminate(const std::string& name) override { active.erase(name); }
    void terminateAll() override { active.clear(); }
    bool hasModule(const std::string& name) const override { return active.count(name) > 0; }

    std::string inner;          // loaded from inside load(), once
    bool reentered = false;
    int innerResult = -1;

private:
    std::unordered_set<std::string> active;
};

class LoadReentrancyTest : public ::testing::Test {
protected:
    void SetUp() override {
        logos_core_terminate_all();
        logos_core_clear();
        loader = std::make_shared<ReenteringLoader>();
        ModuleManager::loaders().clearForTests();
        ModuleManager::loaders().registerLoader(loader);
    }

    void TearDown() override {
        logos_core_terminate_all();
        logos_core_clear();
        ModuleManager::loaders().clearForTests();
        ModuleManager::loaders().registerLoader(std::make_shared<SubprocessManager>());
    }

    void registerModule(const std::string& name) {
        const std::string path = "/fake/" + name + "_plugin.so";
        logos_core_register_module(name.c_str(), path.c_str());
    }

    std::shared_ptr<ReenteringLoader> loader;
};

TEST_F(LoadReentrancyTest, ALoadStartedInsideALoadIsRefused) {
    registerModule("outer");
    registerModule("inner");
    loader->inner = "inner";

    EXPECT_EQ(logos_core_load_module("outer", LOGOS_LOAD_MODULE_ONLY), 1);

    ASSERT_TRUE(loader->reentered) << "the loader never re-entered, so this "
                                     "asserts nothing about re-entrancy";
    EXPECT_EQ(loader->innerResult, 0);
    EXPECT_FALSE(logos_core_is_module_loaded("inner"));
    EXPECT_TRUE(logos_core_is_module_loaded("outer"));
}

// The refusal must not break "ensure loaded": a caller asking for something
// that is already up gets the truthful yes, re-entrant or not.
TEST_F(LoadReentrancyTest, AReentrantLoadOfAnAlreadyLoadedModuleStillSucceeds) {
    registerModule("outer");
    registerModule("prior");
    ASSERT_EQ(logos_core_load_module("prior", LOGOS_LOAD_MODULE_ONLY), 1);

    loader->inner = "prior";
    EXPECT_EQ(logos_core_load_module("outer", LOGOS_LOAD_MODULE_ONLY), 1);

    ASSERT_TRUE(loader->reentered);
    EXPECT_EQ(loader->innerResult, 1);
}

}  // namespace
