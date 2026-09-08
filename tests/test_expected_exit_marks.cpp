// Telling an orderly teardown apart from a module that died.
//
// onTerminated fires for both and cannot tell them apart from its arguments, so
// a teardown announces itself first with markExitExpected() and the callback
// consumes that mark. consumeExpectedExit()'s contract is that it answers true
// EXACTLY ONCE per announced teardown -- "a module that is unloaded, reloaded
// and then CRASHES is reported as a crash rather than inheriting the earlier
// orderly exit". A mark that is set and never consumed silently converts the
// next real crash into a clean shutdown, which is the one thing a modules_state
// consumer cannot afford to be wrong about.
//
// Each test here announces a teardown one way, then kills the module out of
// band and asks what the feed called it.

#include <gtest/gtest.h>

#include "logos_core.h"
#include "module_state_observer.h"
#include "qt_test_adapter.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct TmpDir {
    fs::path path;

    TmpDir() {
        std::string tmpl = (fs::temp_directory_path() / "logos_marks_XXXXXX").string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (!mkdtemp(buf.data())) throw std::runtime_error("mkdtemp failed");
        path = buf.data();
    }

    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Stand-in for logos_host_qt: reports that it loaded, records its pid where the
// test can find it, then stays up until something kills it. `exec` keeps the pid
// it just wrote — the shell becomes the sleep — which is what makes the recorded
// pid the one the container is supervising.
//
// The status line is what keeps these tests quick: without it the load path has
// no verdict to act on and waits out its compatibility deadline on every load,
// which is ~11 s per test here and nothing to do with what they cover.
constexpr const char* kFakeHostScript = R"sh(#!/bin/sh
name=""
while [ $# -gt 0 ]; do
  case "$1" in
    -n|--name) name="$2"; shift 2 ;;
    *) shift ;;
  esac
done
echo $$ > "$LOGOS_TEST_PID_DIR/$name.pid"
printf '%s\n' "@logos-load-status ok"
exec sleep 300
)sh";

class ExpectedExitMarksTest : public ::testing::Test {
protected:
    void SetUp() override {
        logos_core_terminate_all();
        logos_core_clear();

        fs::path host = tmp.path / "fake_logos_host";
        std::ofstream f(host);
        f << kFakeHostScript;
        f.close();
        fs::permissions(host, fs::perms::owner_all | fs::perms::group_exec |
                                  fs::perms::others_exec);
        setenv("LOGOS_HOST_PATH", host.c_str(), 1);
        setenv("LOGOS_TEST_PID_DIR", tmp.path.c_str(), 1);

        auto& o = logos::ModuleStateObserver::instance();
        o.setSink({});
        o.clearPending();
        {
            std::lock_guard<std::mutex> g(mutex);
            seen.clear();
        }
        // Called from the container's asio thread as well as this one.
        o.setSink([this](const std::vector<logos::ModuleTransition>& batch) {
            std::lock_guard<std::mutex> g(mutex);
            for (const auto& t : batch) seen.push_back(t);
        });
    }

    void TearDown() override {
        auto& o = logos::ModuleStateObserver::instance();
        o.setSink({});
        o.clearPending();
        unsetenv("LOGOS_HOST_PATH");
        unsetenv("LOGOS_TEST_PID_DIR");
        logos_core_terminate_all();
        logos_core_clear();
    }

    void plantModule(const std::string& name) {
        fs::path binary = tmp.path / (name + "_plugin.so");
        std::ofstream f(binary);
        f << "fake\n";
        f.close();
        logos_core_register_module(name.c_str(), binary.string().c_str());
    }

    // The pid the module host recorded for its current run, once it has got far
    // enough to write it.
    pid_t modulePid(const std::string& name) {
        const fs::path pidFile = tmp.path / (name + ".pid");
        for (int i = 0; i < 200; ++i) {
            std::ifstream f(pidFile);
            pid_t pid = 0;
            if (f >> pid && pid > 0) return pid;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return 0;
    }

    void forgetPid(const std::string& name) {
        std::error_code ec;
        fs::remove(tmp.path / (name + ".pid"), ec);
    }

    // The death is reported from the asio thread, so give it a moment to land.
    bool waitForTransitionTo(const std::string& name, const std::string& state) {
        for (int i = 0; i < 300; ++i) {
            {
                std::lock_guard<std::mutex> g(mutex);
                for (const auto& t : seen)
                    if (t.module == name && t.newState == state) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    bool sawTransitionTo(const std::string& name, const std::string& state) {
        std::lock_guard<std::mutex> g(mutex);
        for (const auto& t : seen)
            if (t.module == name && t.newState == state) return true;
        return false;
    }

    void forgetTransitions() {
        std::lock_guard<std::mutex> g(mutex);
        seen.clear();
    }

    TmpDir tmp;
    std::mutex mutex;
    std::vector<logos::ModuleTransition> seen;
};

// Kill the module and wait for the container to notice.
void killAndReap(pid_t pid) {
    ASSERT_GT(pid, 0);
    ASSERT_EQ(::kill(pid, SIGKILL), 0);
}

} // namespace

// The contract, on the path it was written for: an ordinary unload announces
// its teardown, and the mark must not survive it to describe the NEXT death.
TEST_F(ExpectedExitMarksTest, UnloadThenReloadThenCrash_IsReportedAsACrash) {
    plantModule("m");

    ASSERT_EQ(logos_core_load_module("m", LOGOS_LOAD_MODULE_ONLY), 1);
    ASSERT_GT(modulePid("m"), 0);
    ASSERT_EQ(logos_core_unload_module("m", false), 1);
    forgetPid("m");

    ASSERT_EQ(logos_core_load_module("m", LOGOS_LOAD_MODULE_ONLY), 1);
    const pid_t pid = modulePid("m");
    forgetTransitions();
    killAndReap(pid);

    EXPECT_TRUE(waitForTransitionTo("m", logos::module_state::kError))
        << "a killed module must be reported as an error, not as a teardown";
    EXPECT_FALSE(sawTransitionTo("m", logos::module_state::kUnloaded));
}

// The same contract after terminate_all(), which announces a teardown for EVERY
// loaded module at once. Nothing consumes those marks -- the container drops the
// callback for a teardown it performed itself -- so each one waits to describe
// that module's next death as an orderly exit.
TEST_F(ExpectedExitMarksTest, TerminateAllThenReloadThenCrash_IsReportedAsACrash) {
    plantModule("m");

    ASSERT_EQ(logos_core_load_module("m", LOGOS_LOAD_MODULE_ONLY), 1);
    ASSERT_GT(modulePid("m"), 0);
    logos_core_terminate_all();
    forgetPid("m");

    ASSERT_EQ(logos_core_load_module("m", LOGOS_LOAD_MODULE_ONLY), 1);
    const pid_t pid = modulePid("m");
    forgetTransitions();
    killAndReap(pid);

    EXPECT_TRUE(waitForTransitionTo("m", logos::module_state::kError))
        << "a killed module must be reported as an error, not as a teardown";
    EXPECT_FALSE(sawTransitionTo("m", logos::module_state::kUnloaded));
}
