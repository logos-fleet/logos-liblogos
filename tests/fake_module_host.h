#ifndef FAKE_MODULE_HOST_H
#define FAKE_MODULE_HOST_H

// A stand-in module host, and the fixture that drives the real load path
// against it. Shared by the load-verdict tests (what the caller is told when a
// child fails) and the load-concurrency tests (what two callers can do at once).

#include <gtest/gtest.h>

#include "logos_core.h"
#include "module_state_observer.h"
#include "qt_test_adapter.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct TmpDir {
    fs::path path;

    TmpDir() {
        std::string tmpl = (fs::temp_directory_path() / "logos_verdict_XXXXXX").string();
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

// Stand-in for logos_host_qt. Its behaviour is the first line of the file the
// daemon names with --path, so one script covers every case and each module
// carries its own. `exec sleep` matters: a plain `sleep` would leave the shell
// as the process the container signals and the sleep behind it orphaned.
//
// Every host writes two marks into ONE log shared by every module in the
// directory: `enter` the moment it starts, `report` as it is about to answer.
// One shared log rather than a file per module because the order of the marks
// across modules is itself the evidence -- see hostWindow() below -- and
// appends of a short line to an O_APPEND fd do not interleave, so the file
// records the real sequence even with several hosts running at once.
//
// `slow-ok` stalls a whole second before reporting. It is not a timeout to be
// waited out: it is the window a test needs a second host to turn up inside,
// wide enough that a loaded machine cannot close it.
constexpr const char* kFakeHostScript = R"sh(#!/bin/sh
path=""
while [ $# -gt 0 ]; do
  case "$1" in
    -p|--path) path="$2"; shift 2 ;;
    *) shift ;;
  esac
done
mod="${path##*/}"; mod="${mod%_plugin.so}"
mark() { [ -n "$path" ] && echo "$1 $mod" >> "${path%/*}/host_events"; }
mark enter
case "$(head -n 1 "$path" 2>/dev/null)" in
  die)         exit 3 ;;
  report-fail) mark report ; printf '%s\n' "@logos-load-status failed undefined symbol: logos_module_install" ; exit 1 ;;
  report-ok)   mark report ; printf '%s\n' "@logos-load-status ok" ; exec sleep 300 ;;
  report-ok-then-die) mark report ; printf '%s\n' "@logos-load-status ok" ; exit 0 ;;
  slow-ok)     sleep 1 ; mark report ; printf '%s\n' "@logos-load-status ok" ; exec sleep 300 ;;
  *)           exec sleep 300 ;;
esac
)sh";

std::set<std::string> loadedModuleNames() {
    std::set<std::string> names;
    char** arr = logos_core_get_loaded_modules();
    if (!arr) return names;
    for (int i = 0; arr[i]; ++i) {
        names.insert(arr[i]);
        delete[] arr[i];
    }
    delete[] arr;
    return names;
}

// A module is registered as known with a placeholder binary, the load runs for
// real, and the lifecycle feed is captured.
class FakeHostFixture : public ::testing::Test {
protected:
    void SetUp() override {
        logos_core_terminate_all();
        logos_core_clear();

        auto& o = logos::ModuleStateObserver::instance();
        o.setSink({});
        o.clearPending();
        seen.clear();
        // Locked: a load reports on the thread that performed it, so with
        // concurrent loads two flushes reach this sink at once.
        o.setSink([this](const std::vector<logos::ModuleTransition>& batch) {
            std::lock_guard<std::mutex> g(seenMutex);
            for (const auto& t : batch) seen.push_back(t);
        });
    }

    void TearDown() override {
        auto& o = logos::ModuleStateObserver::instance();
        o.setSink({});
        o.clearPending();
        unsetenv("LOGOS_HOST_PATH");
        logos_core_terminate_all();
        logos_core_clear();
    }

    void plantModule(const std::string& name, const std::string& contents) {
        fs::path binary = tmp.path / (name + "_plugin.so");
        std::ofstream f(binary);
        f << contents << "\n";
        f.close();
        logos_core_register_module(name.c_str(), binary.string().c_str());
        ASSERT_TRUE(logos_core_is_module_known(name.c_str()));
    }

    // Where a module's host sat in the shared log: the mark that started it,
    // and the mark where it reported its verdict. -1 for a host that never got
    // that far.
    //
    // POSITIONS, not clock readings, and that is the whole point. A busy
    // machine stretches every interval a test could time -- it cannot reorder
    // two marks -- so a question asked of these indices gets the same answer on
    // an idle machine and on one under load.
    struct HostWindow {
        int entered = -1;
        int reported = -1;
    };

    HostWindow hostWindow(const std::string& name) const {
        HostWindow w;
        const std::vector<std::string> events = hostEvents();
        for (int i = 0; i < static_cast<int>(events.size()); ++i) {
            if (w.entered < 0 && events[i] == "enter " + name) w.entered = i;
            if (w.reported < 0 && events[i] == "report " + name) w.reported = i;
        }
        return w;
    }

    // How many hosts the fake host script recorded for this module.
    int spawnCount(const std::string& name) const {
        int n = 0;
        for (const std::string& e : hostEvents())
            if (e == "enter " + name) ++n;
        return n;
    }

    // Every mark from every host in this test, in order. Worth attaching to a
    // failure: it says what actually happened, which "expected 3 < 1" does not.
    std::vector<std::string> hostEvents() const {
        std::ifstream f(tmp.path / "host_events");
        std::vector<std::string> lines;
        for (std::string line; std::getline(f, line); ) lines.push_back(line);
        return lines;
    }

    std::string hostEventLog() const {
        std::string out;
        for (const std::string& e : hostEvents()) out += "\n  " + e;
        return out;
    }

    // Installs the stand-in host for the duration of the test.
    void useFakeHost() {
        fs::path fakeHost = tmp.path / "fake_logos_host";
        std::ofstream f(fakeHost);
        f << kFakeHostScript;
        f.close();
        fs::permissions(fakeHost, fs::perms::owner_all | fs::perms::group_exec |
                                      fs::perms::others_exec);
        ASSERT_TRUE(fs::exists(fakeHost));
        setenv("LOGOS_HOST_PATH", fakeHost.c_str(), 1);
    }

    bool sawTransitionTo(const std::string& name, const std::string& state) const {
        for (const auto& t : seen)
            if (t.module == name && t.newState == state) return true;
        return false;
    }

    std::string reasonFor(const std::string& name, const std::string& state) const {
        for (const auto& t : seen)
            if (t.module == name && t.newState == state && t.reason.has_value())
                return *t.reason;
        return {};
    }

    TmpDir tmp;
    std::mutex seenMutex;
    std::vector<logos::ModuleTransition> seen;
};

}  // namespace

#endif  // FAKE_MODULE_HOST_H
