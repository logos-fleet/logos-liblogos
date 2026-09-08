// A load that reports success must have evidence of success.
//
// logos_core_load_module answers 1 as soon as a child process has been SPAWNED
// and handed its auth token. "Spawned", "the plugin actually loaded" and "the
// module is ready" are three different facts, and collapsing the first into the
// second is what let a module whose plugin never loaded be reported as loaded —
// with the real failure surfacing hops away, in a consumer.
//
// These tests drive the real load path against a stand-in module host named by
// LOGOS_HOST_PATH, so the child's behaviour is the only variable: die on
// startup (the eager-binding case — dlopen refusing a missing symbol), report a
// plugin failure, come up cleanly, or say nothing at all (a host too old to
// report). RealHostLoadVerdictTest repeats the first case against the actual
// logos_host_qt, so nothing here rests on the stand-in being faithful.

#include "fake_module_host.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

namespace {

class LoadVerdictTest : public FakeHostFixture {
protected:
    void SetUp() override {
        FakeHostFixture::SetUp();
        useFakeHost();
    }
};

// THE DEFECT. The child is spawned, takes its token and dies before its plugin
// is ever loaded — the shape of a module built against a stale SDK, where
// dlopen refuses on a missing symbol. None of that reaches the caller.
TEST_F(LoadVerdictTest, ChildDiesOnStartup_LoadReportsFailure) {
    plantModule("ghost", "die");

    EXPECT_EQ(logos_core_load_module("ghost", LOGOS_LOAD_MODULE_ONLY), 0);
    EXPECT_FALSE(logos_core_is_module_loaded("ghost"));
    EXPECT_EQ(loadedModuleNames().count("ghost"), 0u);
}

// The same load must not have claimed `loaded` on the lifecycle feed either: a
// consumer watching modules_state is entitled to the answer the caller got, and
// `error` has to carry a reason.
TEST_F(LoadVerdictTest, ChildDiesOnStartup_FeedReportsErrorAndNeverLoaded) {
    plantModule("ghost", "die");

    ASSERT_EQ(logos_core_load_module("ghost", LOGOS_LOAD_MODULE_ONLY), 0);

    EXPECT_FALSE(sawTransitionTo("ghost", logos::module_state::kLoaded));
    EXPECT_TRUE(sawTransitionTo("ghost", logos::module_state::kError));
    EXPECT_FALSE(reasonFor("ghost", logos::module_state::kError).empty());
}

// The host got far enough to say what went wrong. That diagnostic is what the
// caller and the feed should carry, instead of a success.
TEST_F(LoadVerdictTest, HostReportsPluginFailure_LoadReportsFailureWithTheHostsReason) {
    plantModule("broken", "report-fail");

    EXPECT_EQ(logos_core_load_module("broken", LOGOS_LOAD_MODULE_ONLY), 0);
    EXPECT_FALSE(logos_core_is_module_loaded("broken"));
    EXPECT_NE(reasonFor("broken", logos::module_state::kError)
                  .find("logos_module_install"),
              std::string::npos);
}

// The guard against a fix that simply fails everything: a host that reports its
// plugin loaded must still be reported as loaded.
TEST_F(LoadVerdictTest, HostReportsLoaded_LoadSucceeds) {
    plantModule("healthy", "report-ok");

    EXPECT_EQ(logos_core_load_module("healthy", LOGOS_LOAD_MODULE_ONLY), 1);
    EXPECT_TRUE(logos_core_is_module_loaded("healthy"));
    EXPECT_TRUE(sawTransitionTo("healthy", logos::module_state::kLoaded));
}

// Compatibility: a host built before the status line existed reports nothing.
// It is alive at the deadline, and "I cannot tell" is not evidence of failure.
TEST_F(LoadVerdictTest, HostReportsNothing_LoadStillSucceeds) {
    plantModule("silent", "hang");

    EXPECT_EQ(logos_core_load_module("silent", LOGOS_LOAD_MODULE_ONLY), 1);
    EXPECT_TRUE(logos_core_is_module_loaded("silent"));
}

// A module that comes up and then dies must not be left marked loaded. The
// registry write and the death notice are settled under one lock for exactly
// this: markUnloaded on a module that has not been marked loaded yet does
// nothing, so a death landing in that gap used to be erased by the markLoaded
// behind it, and the module stayed listed as loaded for good. The window is a
// genuine race, so this pins the outcome rather than the branch.
TEST_F(LoadVerdictTest, HostThatLoadsThenDies_IsNotLeftMarkedLoaded) {
    plantModule("brief", "report-ok-then-die");

    logos_core_load_module("brief", LOGOS_LOAD_MODULE_ONLY);

    for (int i = 0; i < 200 && logos_core_is_module_loaded("brief"); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_FALSE(logos_core_is_module_loaded("brief"));
    EXPECT_EQ(loadedModuleNames().count("brief"), 0u);
}

// The same defect against the real logos_host_qt, asked to load a file that is
// not a plugin at all. Nothing here depends on the stand-in above behaving like
// the host; the only stand-in is the module.
class RealHostLoadVerdictTest : public FakeHostFixture {
protected:
    void SetUp() override {
        FakeHostFixture::SetUp();

        const char* host = std::getenv("TEST_REAL_HOST");
        // A skip renders as a pass, so the check sets LOGOS_REQUIRE_TEST_FIXTURES
        // to turn a missing host into a red run rather than silent coverage loss.
        if (!host || !fs::exists(host)) {
            if (std::getenv("LOGOS_REQUIRE_TEST_FIXTURES"))
                FAIL() << "TEST_REAL_HOST does not name a built module host: "
                       << (host ? host : "(unset)");
            GTEST_SKIP() << "TEST_REAL_HOST not set";
        }
        setenv("LOGOS_HOST_PATH", host, 1);
    }
};

TEST_F(RealHostLoadVerdictTest, PluginNeverLoads_LoadReportsFailure) {
    plantModule("not_a_plugin", "this file is not a Qt plugin");

    EXPECT_EQ(logos_core_load_module("not_a_plugin", LOGOS_LOAD_MODULE_ONLY), 0);
    EXPECT_FALSE(logos_core_is_module_loaded("not_a_plugin"));
    EXPECT_EQ(loadedModuleNames().count("not_a_plugin"), 0u);
    EXPECT_FALSE(sawTransitionTo("not_a_plugin", logos::module_state::kLoaded));
    EXPECT_TRUE(sawTransitionTo("not_a_plugin", logos::module_state::kError));
}

// What the real logos_host_qt says when its plugin DOES load.
//
// Driven as a subprocess rather than through logos_core_load_module: a
// successful real-module load goes on to dial the module synchronously to hand
// over its token, and this process has no QCoreApplication for that call's
// event loop. Spawning the host directly asks the one question the daemon's
// stand-in cannot answer — does the real host actually emit the line the daemon
// now waits for — and none of the machinery in between is needed for it.
//
// It matters on its own: without the ok line every healthy load sits out the
// compatibility deadline instead of returning as soon as the module is up.
TEST_F(RealHostLoadVerdictTest, RealHostReportsOkForAPluginThatLoads) {
    const char* host = std::getenv("TEST_REAL_HOST");
    const char* plugin = std::getenv("TEST_PLUGIN");
    ASSERT_TRUE(host && plugin) << "TEST_REAL_HOST / TEST_PLUGIN must be set";

    int out[2], in[2];
    ASSERT_EQ(pipe(out), 0);
    ASSERT_EQ(pipe(in), 0);

    const pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        execl(host, host, "--name", "capability_module", "--path", plugin,
              "--token-source", "stdin", static_cast<char*>(nullptr));
        _exit(127);
    }

    close(in[0]);
    close(out[1]);
    const std::string token = "00000000-0000-0000-0000-000000000000\n";
    ASSERT_GT(write(in[1], token.data(), token.size()), 0);
    close(in[1]);

    // Read until the status line lands or the host exits; a host that comes up
    // stays up, so stopping at the line is what keeps this short.
    const std::string ok = "@logos-load-status ok";
    std::string seen;
    while (seen.find(ok) == std::string::npos) {
        char buf[1024];
        const ssize_t n = read(out[0], buf, sizeof(buf));
        if (n <= 0) break;
        seen.append(buf, static_cast<size_t>(n));
    }
    close(out[0]);
    kill(pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, 0);

    EXPECT_NE(seen.find(ok), std::string::npos) << "host stdout was:\n" << seen;
}

} // namespace
