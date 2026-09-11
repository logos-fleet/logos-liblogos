#ifndef LOGOS_TESTS_BARE_MODULE_FIXTURE_H
#define LOGOS_TESTS_BARE_MODULE_FIXTURE_H

// The handles onto fixtures/bare_fixture_module that more than one suite in
// this binary needs. Shared rather than copied because a change to how the
// fixture is located or how the Qt app is brought up has to reach every suite
// that drives the Native container, not just the one being edited.

#include "module_registry.h"

#include <QCoreApplication>

#include <cstdlib>
#include <string>

namespace LogosTests {

// TEST_BARE_MODULE points at fixtures/bare_fixture_module — a hand-written
// Bare module (no Qt, no logos-protocol) built by this suite's CMakeLists.
inline std::string bareModulePath()
{
    const char* p = std::getenv("TEST_BARE_MODULE");
    return p ? p : std::string();
}

inline LogosCore::ModuleDescriptor bareDescriptor(const std::string& name = "bare_fixture")
{
    LogosCore::ModuleDescriptor desc;
    desc.name = name;
    desc.path = bareModulePath();
    desc.format = "bare";
    return desc;
}

// A deferring glue runs a Qt event loop on its worker thread, and
// QThread::exec() refuses to run one without a QCoreApplication ("QEventLoop:
// Cannot be used without QCoreApplication"). Every real host of this container
// has one long before a module loads; this binary has none because most of it
// needs none. Built once, on the main thread, and deliberately never destroyed
// — the process is about to end and nothing else here wants one.
inline void ensureQtApp()
{
    if (QCoreApplication::instance())
        return;
    static int argc = 1;
    static char arg0[] = "logos_core_tests";
    static char* argv[] = {arg0, nullptr};
    static QCoreApplication* app = new QCoreApplication(argc, argv);
    (void)app;
}

}  // namespace LogosTests

#endif  // LOGOS_TESTS_BARE_MODULE_FIXTURE_H
