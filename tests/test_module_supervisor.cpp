// THE SUPERVISION POLICY — what happens to a module that died without being
// asked to.
//
// Until slice 26 the answer was "nothing": onTerminated recorded `loaded ->
// error` on the lifecycle feed and the module stayed gone until an operator
// noticed. That is the right answer for a Qt plugin whose subprocess segfaulted
// (its state went with it and a restart is a lie), and the wrong one for a `web`
// variant's Wasm host, whose whole crash story is "the image traps, the page
// survives, load a fresh image" — slice 26's fourth acceptance criterion.
//
// So the policy is a POLICY: a budget of restarts within a window, per module,
// off unless a host asks for it. The two halves are tested apart because they
// fail apart:
//
//   * the DECISION is pure — a name, a clock reading and the history — and
//     every interesting case is a clock reading, so the tests hand it one;
//   * the WIRING is "who calls it, with what, and what runs afterwards", and it
//     is driven here through the real ModuleManager load path with a fake
//     loader, because the bug that matters is a death that reaches the policy
//     under the wrong description (an orderly unload reported as a crash, or
//     the reverse).

#include <gtest/gtest.h>

#include "logos_core.h"
#include "module_loader.h"
#include "module_loader_registry.h"
#include "module_manager.h"
#include "module_supervisor.h"
#include "qt_test_adapter.h"
#include "subprocess_manager.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace LogosCore;
using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

SupervisionPolicy budgetOf(int maxRestarts,
                           std::chrono::milliseconds window = 60000ms,
                           std::chrono::milliseconds backoff = 0ms)
{
    SupervisionPolicy p;
    p.maxRestarts = maxRestarts;
    p.window = window;
    p.backoff = backoff;
    return p;
}

} // namespace

// ── the decision ────────────────────────────────────────────────────────────

TEST(ModuleSupervisorPolicy, OffUntilAHostAsksForIt) {
    ModuleSupervisor s;
    EXPECT_EQ(s.policy().maxRestarts, 0);
    EXPECT_EQ(s.onUnexpectedExit("m", Clock::now()).decision,
              ModuleSupervisor::Decision::Disabled);
}

TEST(ModuleSupervisorPolicy, RestartsUpToTheBudgetAndThenStops) {
    ModuleSupervisor s(budgetOf(2));
    const auto t = Clock::now();

    auto first = s.onUnexpectedExit("m", t);
    EXPECT_EQ(first.decision, ModuleSupervisor::Decision::Restart);
    EXPECT_EQ(first.attempt, 1);

    auto second = s.onUnexpectedExit("m", t + 1s);
    EXPECT_EQ(second.decision, ModuleSupervisor::Decision::Restart);
    EXPECT_EQ(second.attempt, 2);

    // The third death inside the window is a crash LOOP, and restarting it
    // again would hide a module that cannot run from the operator who has to
    // fix it.
    auto third = s.onUnexpectedExit("m", t + 2s);
    EXPECT_EQ(third.decision, ModuleSupervisor::Decision::BudgetExhausted);
    EXPECT_EQ(third.attempt, 3);
}

TEST(ModuleSupervisorPolicy, TheBudgetIsPerWindowNotPerLifetime) {
    ModuleSupervisor s(budgetOf(1, 10s));
    const auto t = Clock::now();

    EXPECT_EQ(s.onUnexpectedExit("m", t).decision,
              ModuleSupervisor::Decision::Restart);
    EXPECT_EQ(s.onUnexpectedExit("m", t + 1s).decision,
              ModuleSupervisor::Decision::BudgetExhausted);

    // A module that ran for longer than the window before dying again is not
    // in a loop; it had a bad day twice.
    EXPECT_EQ(s.onUnexpectedExit("m", t + 30s).decision,
              ModuleSupervisor::Decision::Restart);
}

TEST(ModuleSupervisorPolicy, EachModuleSpendsItsOwnBudget) {
    ModuleSupervisor s(budgetOf(1));
    const auto t = Clock::now();

    EXPECT_EQ(s.onUnexpectedExit("a", t).decision,
              ModuleSupervisor::Decision::Restart);
    EXPECT_EQ(s.onUnexpectedExit("b", t).decision,
              ModuleSupervisor::Decision::Restart);
    EXPECT_EQ(s.onUnexpectedExit("a", t + 1s).decision,
              ModuleSupervisor::Decision::BudgetExhausted);
}

TEST(ModuleSupervisorPolicy, ForgettingAModuleReturnsItsBudget) {
    ModuleSupervisor s(budgetOf(1));
    const auto t = Clock::now();

    EXPECT_EQ(s.onUnexpectedExit("m", t).decision,
              ModuleSupervisor::Decision::Restart);
    EXPECT_EQ(s.onUnexpectedExit("m", t + 1s).decision,
              ModuleSupervisor::Decision::BudgetExhausted);

    // An operator who unloads and loads a module again has intervened; the
    // history that describes the PREVIOUS run must not spend the new one's
    // budget.
    s.forget("m");
    EXPECT_EQ(s.onUnexpectedExit("m", t + 2s).decision,
              ModuleSupervisor::Decision::Restart);
}

TEST(ModuleSupervisorPolicy, TheBackoffIsCarriedOnTheVerdict) {
    ModuleSupervisor s(budgetOf(2, 60000ms, 250ms));
    EXPECT_EQ(s.onUnexpectedExit("m", Clock::now()).delay, 250ms);
}

// ── the operator's way in ───────────────────────────────────────────────────

TEST(ModuleSupervisorPolicy, EnvironmentSpellsOutTheBudget) {
    auto p = ModuleSupervisor::policyFromEnvironment("3,10000,250");
    EXPECT_EQ(p.maxRestarts, 3);
    EXPECT_EQ(p.window, 10000ms);
    EXPECT_EQ(p.backoff, 250ms);

    // The count alone is the common form; the rest keeps its default.
    EXPECT_EQ(ModuleSupervisor::policyFromEnvironment("3").maxRestarts, 3);

    // Anything unreadable leaves supervision OFF rather than guessing a budget
    // for it: a typo must not silently restart a crashing module forever.
    EXPECT_EQ(ModuleSupervisor::policyFromEnvironment("").maxRestarts, 0);
    EXPECT_EQ(ModuleSupervisor::policyFromEnvironment("yes please").maxRestarts, 0);
    EXPECT_EQ(ModuleSupervisor::policyFromEnvironment(nullptr).maxRestarts, 0);
    EXPECT_EQ(ModuleSupervisor::policyFromEnvironment("0").maxRestarts, 0);
}

// ── the wiring ──────────────────────────────────────────────────────────────

namespace {

// A loader that KEEPS the termination callback the core handed it, so a test
// can kill a module the way a container does — from the outside, after the load
// has completed.
struct SupervisedFakeLoader : public ModuleLoader {
    std::string id() const override { return "supervised-fake"; }
    bool canHandle(const ModuleDescriptor&) const override { return true; }

    bool load(const ModuleDescriptor& desc,
              std::function<void(const std::string&)> onTerminated,
              LoadedModuleHandle& out) override {
        loadCalls.push_back(desc.name);
        died[desc.name] = std::move(onTerminated);
        out.name = desc.name;
        out.pid = 4321;
        out.endpoint = "fake://" + desc.name;
        active.insert(desc.name);
        return true;
    }

    bool sendToken(const std::string&, const std::string&) override { return true; }

    // A REAL CONTAINER ANNOUNCES THE TEARDOWN IT PERFORMED, on the same
    // callback a crash arrives on — that is the whole reason the expected-exit
    // marks exist. A fake that stayed quiet here would let this file assert a
    // property of a container nobody ships.
    void terminate(const std::string& name) override { kill(name); }

    void terminateAll() override { active.clear(); }
    bool hasModule(const std::string& name) const override { return active.count(name) > 0; }

    // What a container does when a page or a process is gone, however it went.
    void kill(const std::string& name) {
        if (!active.erase(name)) return;
        auto it = died.find(name);
        if (it != died.end() && it->second) it->second(name);
    }

    std::vector<std::string> loadCalls;
    std::unordered_set<std::string> active;
    std::unordered_map<std::string, std::function<void(const std::string&)>> died;
};

int timesLoaded(const SupervisedFakeLoader& loader, const std::string& name) {
    int n = 0;
    for (const auto& called : loader.loadCalls) if (called == name) ++n;
    return n;
}

} // namespace

class SupervisedLoadTest : public ::testing::Test {
protected:
    std::shared_ptr<SupervisedFakeLoader> fake;

    void SetUp() override {
        logos_core_terminate_all();
        logos_core_clear();
        SubprocessManager::clearAll();

        fake = std::make_shared<SupervisedFakeLoader>();
        ModuleManager::loaders().clearForTests();
        ModuleManager::loaders().registerLoader(fake);

        // INLINE, so a test is a straight line. The shipping scheduler posts the
        // restart to the Qt main thread and waits out the backoff, both because
        // the death is announced on a container's own thread; neither is what
        // this file is about.
        LogosCore::setRestartScheduler(
            [](std::chrono::milliseconds, std::function<void()> task) { task(); });
        logos_core_set_supervision_policy(2, 60000, 0);
    }

    void TearDown() override {
        logos_core_set_supervision_policy(0, 0, 0);
        LogosCore::setRestartScheduler({});
        logos_core_terminate_all();
        logos_core_clear();
        SubprocessManager::clearAll();
        ModuleManager::loaders().clearForTests();
        ModuleManager::loaders().registerLoader(std::make_shared<SubprocessManager>());
    }

    void registerModule(const std::string& name) {
        const std::string path = "/fake/" + name + "_plugin.so";
        logos_core_register_module(name.c_str(), path.c_str());
    }
};

TEST_F(SupervisedLoadTest, AModuleThatDiedOnItsOwnIsLoadedAgain) {
    registerModule("crasher");
    ASSERT_TRUE(ModuleManager::loadModule("crasher"));
    ASSERT_EQ(timesLoaded(*fake, "crasher"), 1);

    fake->kill("crasher");

    EXPECT_EQ(timesLoaded(*fake, "crasher"), 2)
        << "a module that died without being asked to was not restarted";
    EXPECT_TRUE(ModuleManager::isModuleLoaded("crasher"));
}

TEST_F(SupervisedLoadTest, AnOrderlyUnloadIsNotARestart) {
    registerModule("tidy");
    ASSERT_TRUE(ModuleManager::loadModule("tidy"));

    // The unload announces the SAME callback a crash arrives on, from inside
    // terminate(). The supervisor must not see it: an operator who unloaded a
    // module and watched it come back would have no way to turn it off.
    ASSERT_TRUE(ModuleManager::unloadModule("tidy"));

    EXPECT_EQ(timesLoaded(*fake, "tidy"), 1);
    EXPECT_FALSE(ModuleManager::isModuleLoaded("tidy"));
}

TEST_F(SupervisedLoadTest, ACrashLoopStopsAtTheBudget) {
    registerModule("loop");
    ASSERT_TRUE(ModuleManager::loadModule("loop"));

    fake->kill("loop");   // restart 1
    fake->kill("loop");   // restart 2
    fake->kill("loop");   // over budget: stays down

    EXPECT_EQ(timesLoaded(*fake, "loop"), 3);
    EXPECT_FALSE(ModuleManager::isModuleLoaded("loop"));
}

TEST_F(SupervisedLoadTest, WithNoPolicyAModuleThatDiedStaysDown) {
    logos_core_set_supervision_policy(0, 0, 0);
    registerModule("unsupervised");
    ASSERT_TRUE(ModuleManager::loadModule("unsupervised"));

    fake->kill("unsupervised");

    EXPECT_EQ(timesLoaded(*fake, "unsupervised"), 1);
    EXPECT_FALSE(ModuleManager::isModuleLoaded("unsupervised"));
}

TEST_F(SupervisedLoadTest, LoadingAModuleAgainByHandReturnsItsBudget) {
    registerModule("resilient");
    ASSERT_TRUE(ModuleManager::loadModule("resilient"));

    fake->kill("resilient");   // restart 1
    fake->kill("resilient");   // restart 2
    fake->kill("resilient");   // over budget

    ASSERT_EQ(timesLoaded(*fake, "resilient"), 3);
    ASSERT_FALSE(ModuleManager::isModuleLoaded("resilient"));

    // The operator has intervened, which is what the budget was protecting
    // them from having to do blindly. The next crash is supervised again.
    ASSERT_TRUE(ModuleManager::loadModule("resilient"));
    fake->kill("resilient");

    EXPECT_EQ(timesLoaded(*fake, "resilient"), 5);
}
