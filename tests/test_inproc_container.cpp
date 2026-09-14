// The Native container, end to end against a real Bare module image.
//
// TEST_BARE_MODULE points at fixtures/bare_fixture_module — a hand-written
// Bare module (no Qt, no logos-protocol) built by this suite's CMakeLists. The
// container dlopens it exactly as it would dlopen a module-builder `bare`
// artifact, so what is exercised here is the production path and not a double
// of it.
//
// No LogosAPI is handed to the container in these tests. That is deliberate and
// it is what makes them hermetic: with no host API the container loads, drives
// and unloads the module without publishing it, so everything below runs with
// no core, no capability_module, no transport and no event loop. Publication is
// the one step these tests do not cover, and it is covered by the load path in
// test_module_manager.

#include <gtest/gtest.h>

#include "bare_module_abi.h"
#include "bare_module_glue.h"
#include "bare_module_fixture.h"
#include "inproc_container.h"
#include "bare_module_loader.h"
#include "composite_module_loader.h"
#include "module_registry.h"
#include "module_resource_usage.h"

#include <process_stats/process_stats.h>

#include <logos_async_dispatch.h>
#include <logos_protocol.h>
#include <logos_types.h>
#include <token_manager.h>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QVariantMap>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#  include <dlfcn.h>
#endif

using LogosTests::bareDescriptor;
using LogosTests::bareModulePath;
using LogosTests::ensureQtApp;

// ── the ABI resolver ────────────────────────────────────────────────────────

TEST(BareModuleAbiTest, MissingImageIsAnErrorNotACrash)
{
    LogosCore::BareModuleAbi abi;
    std::string error;
    EXPECT_FALSE(LogosCore::openBareModule("/nonexistent/not_a_bare.so", abi, &error));
    EXPECT_FALSE(abi);
    EXPECT_NE(error.find("/nonexistent/not_a_bare.so"), std::string::npos)
        << "the diagnostic must name the path that failed; got: " << error;
}

TEST(BareModuleAbiTest, EmptyPathIsRejected)
{
    LogosCore::BareModuleAbi abi;
    std::string error;
    EXPECT_FALSE(LogosCore::openBareModule("", abi, &error));
    EXPECT_FALSE(error.empty());
}

TEST(BareModuleAbiTest, ResolvesTheWholeModuleImplAbi)
{
    ASSERT_FALSE(bareModulePath().empty()) << "TEST_BARE_MODULE is not set";

    LogosCore::BareModuleAbi abi;
    std::string error;
    ASSERT_TRUE(LogosCore::openBareModule(bareModulePath(), abi, &error)) << error;

    EXPECT_NE(abi.dispatch, nullptr);
    EXPECT_NE(abi.getMethods, nullptr);
    EXPECT_NE(abi.stringFree, nullptr);
    EXPECT_NE(abi.setContext, nullptr);
    EXPECT_NE(abi.setEmitCallback, nullptr);
    EXPECT_NE(abi.acceptToken, nullptr);
    EXPECT_NE(abi.protocolVersion, nullptr);
    // The conditional half — this fixture is written against the current
    // protocol, so all of it is present.
    EXPECT_NE(abi.acceptInboundToken, nullptr);
    EXPECT_NE(abi.setCallCaller, nullptr);
    EXPECT_NE(abi.aboutToUnload, nullptr);

    LogosCore::closeBareModule(abi);
    EXPECT_EQ(abi.handle, nullptr);
}

TEST(BareModuleAbiTest, ProtocolMajorIsTheOnlyCompatibilityRule)
{
    std::string reason;
    EXPECT_TRUE(LogosCore::bareModuleProtocolCompatible(LOGOS_PROTOCOL_VERSION_STRING, &reason));
    EXPECT_TRUE(reason.empty());

    // No stamp at all: a pre-protocol build, loaded permissively but SAID SO.
    EXPECT_TRUE(LogosCore::bareModuleProtocolCompatible("", &reason));
    EXPECT_FALSE(reason.empty());

    const std::string otherMajor =
        std::to_string(LOGOS_PROTOCOL_VERSION_MAJOR + 1) + ".0.0";
    EXPECT_FALSE(LogosCore::bareModuleProtocolCompatible(otherMajor, &reason));
    EXPECT_NE(reason.find(otherMajor), std::string::npos);
}

// ── the generic glue ────────────────────────────────────────────────────────

class BareModuleGlueTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_FALSE(bareModulePath().empty()) << "TEST_BARE_MODULE is not set";
        std::string error;
        ASSERT_TRUE(LogosCore::openBareModule(bareModulePath(), m_abi, &error)) << error;
        m_glue = std::make_unique<LogosCore::BareModuleGlue>("bare_fixture", "1.2.3", m_abi);
    }

    void TearDown() override
    {
        m_glue.reset();
        LogosCore::closeBareModule(m_abi);
    }

    LogosCore::BareModuleAbi m_abi;
    std::unique_ptr<LogosCore::BareModuleGlue> m_glue;
};

TEST_F(BareModuleGlueTest, PublishesTheContractTheModuleReports)
{
    const QJsonArray methods = m_glue->getMethods();
    ASSERT_FALSE(methods.isEmpty());

    QStringList names;
    for (const QJsonValue& v : methods)
        names << v.toObject().value("name").toString();

    EXPECT_TRUE(names.contains("add"));
    EXPECT_TRUE(names.contains("bump"));
    // Events ride inside getMethods() tagged "event" — the glue passes the
    // module's array through whole rather than filtering it, because that is
    // the array LogosProviderObject::getMethods() is defined to return.
    EXPECT_TRUE(names.contains("ticked"));

    EXPECT_EQ(m_glue->providerName(), QStringLiteral("bare_fixture"));
    EXPECT_EQ(m_glue->providerVersion(), QStringLiteral("1.2.3"));
}

TEST_F(BareModuleGlueTest, DispatchesByNameWithJson)
{
    const QVariant sum = m_glue->callMethod(QStringLiteral("add"),
                                            QVariantList{1, 2});
    ASSERT_TRUE(sum.isValid());
    EXPECT_EQ(sum.toLongLong(), 3);
}

TEST_F(BareModuleGlueTest, AVoidMethodAnswersTrueRatherThanNothing)
{
    // The module returns JSON null for `bump`. An invalid QVariant is this
    // slot's FAILURE token, so a void method that answered it would be
    // indistinguishable from a call that did not run.
    const QVariant ran = m_glue->callMethod(QStringLiteral("bump"), QVariantList{7});
    ASSERT_TRUE(ran.isValid());
    EXPECT_TRUE(ran.toBool());

    EXPECT_EQ(m_glue->callMethod(QStringLiteral("total"), {}).toLongLong(), 7);
}

TEST_F(BareModuleGlueTest, AResultMethodRematerialisesLogosResult)
{
    const QVariant v = m_glue->callMethod(QStringLiteral("describe"), {});
    ASSERT_TRUE(v.canConvert<LogosResult>())
        << "a method whose published returnType is `result` must come back as a "
           "LogosResult, exactly as it would from the generated Qt glue";
    const LogosResult r = v.value<LogosResult>();
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.value.toMap().value("name").toString(), QStringLiteral("bare_fixture"));
}

// The same classification, under the OTHER spelling of the same return type.
//
// `returnType` is written by whichever generator produced the module, and the
// two in this tree disagree: logos-cpp-sdk publishes the LIDL text (`result`,
// lidlTypeToPublishedName), and logos-rust-sdk publishes the Qt type name
// (`LogosResult`, qt_type_name). A glue that knows only the first classifies
// every `result` method of every RUST module as an ordinary value, and the
// module's {success, value, error} map reaches the caller UNOPENED -- so a
// failure arrives as a successful call returning a map, and the error channel
// the contract promises is silently gone.
//
// Measured on the iOS smoke host (logos-workspace#10): chat_module's `init`
// failed and the caller was told it had succeeded, because the map was never
// examined. What the caller has to see is the failure.
TEST_F(BareModuleGlueTest, TheQtSpellingOfAResultIsTheSameResult)
{
    const QVariant v = m_glue->callMethod(QStringLiteral("describeQt"), {});
    ASSERT_TRUE(v.canConvert<LogosResult>())
        << "a method published with returnType `LogosResult` -- what the Rust "
           "SDK emits for a `result` -- must be classified the same as one "
           "published as `result`";
    const LogosResult r = v.value<LogosResult>();
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error.toString(), QStringLiteral("describeQt says no"));
}

TEST_F(BareModuleGlueTest, AnUnknownMethodIsAnInvalidQVariant)
{
    EXPECT_FALSE(m_glue->callMethod(QStringLiteral("no_such_method"), {}).isValid());
}

TEST_F(BareModuleGlueTest, EventsReachTheProvidersListener)
{
    QString seenEvent;
    QVariantList seenData;
    m_glue->setEventListener([&](const QString& name, const QVariantList& data) {
        seenEvent = name;
        seenData = data;
    });

    m_glue->callMethod(QStringLiteral("tick"), QVariantList{42});

    EXPECT_EQ(seenEvent, QStringLiteral("ticked"));
    ASSERT_EQ(seenData.size(), 1);
    EXPECT_EQ(seenData.front().toLongLong(), 42);
}

TEST_F(BareModuleGlueTest, AnEventWithNoListenerIsDroppedNotCrashed)
{
    // No setEventListener call: the emit callback is installed from the
    // constructor, so this is the window a module's context-ready hook fires in.
    EXPECT_NO_FATAL_FAILURE(m_glue->callMethod(QStringLiteral("tick"), QVariantList{1}));
}

TEST_F(BareModuleGlueTest, ContextIsStampedAcrossTheAbi)
{
    m_glue->deliverContext(QStringLiteral("/modules/bare_fixture"),
                           QStringLiteral("inst-1"),
                           QStringLiteral("/data/bare_fixture/inst-1"));
    EXPECT_EQ(m_glue->callMethod(QStringLiteral("context"), {}).toString(),
              QStringLiteral("/modules/bare_fixture|inst-1|/data/bare_fixture/inst-1"));
}

TEST_F(BareModuleGlueTest, TheCallerIsPushedAroundEveryDispatch)
{
    m_glue->callMethod(QStringLiteral("add"), QVariantList{1, 1});
    const QString caller = m_glue->callMethod(QStringLiteral("caller"), {}).toString();
    // Nothing opened a CallerScope, so the document is the fail-closed one —
    // and the point of the assertion is that a document was pushed AT ALL. A
    // glue that never called logos_module_set_call_caller would leave this
    // empty, and the module would read whatever the previous dispatch left.
    EXPECT_TRUE(caller.contains(QStringLiteral("kind")))
        << "no caller document reached the module; got: " << caller.toStdString();
}

// ── the container ───────────────────────────────────────────────────────────

TEST(InProcContainerTest, ClaimsOnlyBareModules)
{
    LogosCore::InProcContainer container;
    EXPECT_TRUE(container.canHandle(bareDescriptor()));

    LogosCore::ModuleDescriptor qtPlugin = bareDescriptor();
    qtPlugin.format = "qt-plugin";
    EXPECT_FALSE(container.canHandle(qtPlugin));

    // The EMPTY format is the Qt loader's, and this container must never claim
    // it: two loaders claiming the same descriptor makes the answer depend on
    // registration order.
    LogosCore::ModuleDescriptor unspecified = bareDescriptor();
    unspecified.format.clear();
    EXPECT_FALSE(container.canHandle(unspecified));
}

TEST(InProcContainerTest, ReportsTheInProcessPidSentinel)
{
    ASSERT_FALSE(bareModulePath().empty()) << "TEST_BARE_MODULE is not set";
    LogosCore::InProcContainer container;
    LogosCore::LoadedModuleHandle handle;

    ASSERT_TRUE(container.launch(bareDescriptor(), "", {}, nullptr, handle));

    EXPECT_EQ(handle.pid, LogosCore::InProcContainer::kInProcPid);
    EXPECT_EQ(handle.pid, -1) << "the sentinel is documented as -1 in LoadedModuleHandle";
    EXPECT_EQ(handle.name, "bare_fixture");
    EXPECT_EQ(handle.endpoint, "inproc://bare_fixture");

    EXPECT_TRUE(container.hasModule("bare_fixture"));
    ASSERT_TRUE(container.pid("bare_fixture").has_value());
    EXPECT_EQ(*container.pid("bare_fixture"), -1);

    const auto pids = container.getAllPids();
    ASSERT_EQ(pids.size(), 1u);
    EXPECT_EQ(pids.at("bare_fixture"), -1);

    container.terminate("bare_fixture");
    EXPECT_FALSE(container.hasModule("bare_fixture"));
    EXPECT_FALSE(container.pid("bare_fixture").has_value());
    EXPECT_TRUE(container.getAllPids().empty());
}

TEST(InProcContainerTest, AModuleThatCannotBeOpenedIsALoadErrorNotACrash)
{
    LogosCore::InProcContainer container;
    LogosCore::ModuleDescriptor desc = bareDescriptor();
    desc.path = "/nonexistent/broken_bare.so";

    LogosCore::LoadedModuleHandle handle;
    EXPECT_FALSE(container.launch(desc, "", {}, nullptr, handle));
    EXPECT_FALSE(container.hasModule("bare_fixture"));
}

TEST(InProcContainerTest, AnImageWithoutTheAbiIsRefused)
{
    // The test binary itself is a perfectly loadable image that exports none of
    // the module-impl ABI — the exact shape of "a file named like a module that
    // is not one".
    const char* self = std::getenv("TEST_NON_MODULE_IMAGE");
    if (!self) GTEST_SKIP() << "TEST_NON_MODULE_IMAGE is not set";

    LogosCore::BareModuleAbi abi;
    std::string error;
    EXPECT_FALSE(LogosCore::openBareModule(self, abi, &error));
    EXPECT_NE(error.find("logos_module_"), std::string::npos)
        << "the diagnostic must name the missing entry point; got: " << error;
}

TEST(InProcContainerTest, AnUnloadedModuleCanBeLoadedAgain)
{
    ASSERT_FALSE(bareModulePath().empty()) << "TEST_BARE_MODULE is not set";
    LogosCore::InProcContainer container;
    LogosCore::LoadedModuleHandle handle;

    ASSERT_TRUE(container.launch(bareDescriptor(), "", {}, nullptr, handle));
    container.terminate("bare_fixture");

    // The reload is the assertion: a container that let the unload leave state
    // behind — a name still in its map, an emit callback still pointing at a
    // destroyed glue — either refuses this or reaches into freed memory.
    //
    // The IMAGE is state it deliberately does leave behind, and this is where
    // that shows: the second load re-uses the mapping the first one made rather
    // than re-running the image's initialisers (closeBareModule, #96). Nothing
    // in the host depends on the difference, and a module that needs to
    // re-initialise is handed a fresh context on every load.
    ASSERT_TRUE(container.launch(bareDescriptor(), "", {}, nullptr, handle));
    EXPECT_TRUE(container.hasModule("bare_fixture"));
    container.terminate("bare_fixture");
}

TEST(InProcContainerTest, TerminationIsAnnouncedExactlyOnce)
{
    ASSERT_FALSE(bareModulePath().empty()) << "TEST_BARE_MODULE is not set";
    LogosCore::InProcContainer container;
    LogosCore::LoadedModuleHandle handle;

    int announced = 0;
    std::string announcedName;
    ASSERT_TRUE(container.launch(bareDescriptor(), "", {},
                                 [&](const std::string& n) { ++announced; announcedName = n; },
                                 handle));
    container.terminate("bare_fixture");
    EXPECT_EQ(announced, 1);
    EXPECT_EQ(announcedName, "bare_fixture");

    // A second terminate for a module that is already gone is a no-op, not a
    // second announcement — the registry would otherwise see a module die twice.
    container.terminate("bare_fixture");
    EXPECT_EQ(announced, 1);
}

TEST(InProcContainerTest, LaunchIsTheLoadVerdict)
{
    ASSERT_FALSE(bareModulePath().empty()) << "TEST_BARE_MODULE is not set";
    LogosCore::InProcContainer container;
    LogosCore::LoadedModuleHandle handle;

    // Unlike a subprocess, there is nothing to wait for: the image is open and
    // its ABI resolved before launch() returns.
    ASSERT_TRUE(container.launch(bareDescriptor(), "", {}, nullptr, handle));
    EXPECT_EQ(container.awaitLoad("bare_fixture", std::chrono::milliseconds(0)).verdict,
              LogosCore::LoadVerdict::Loaded);

    container.terminate("bare_fixture");
    EXPECT_EQ(container.awaitLoad("bare_fixture", std::chrono::milliseconds(0)).verdict,
              LogosCore::LoadVerdict::Failed);
}

TEST(InProcContainerTest, TerminateAllStopsEveryModule)
{
    ASSERT_FALSE(bareModulePath().empty()) << "TEST_BARE_MODULE is not set";
    LogosCore::InProcContainer container;
    LogosCore::LoadedModuleHandle handle;

    ASSERT_TRUE(container.launch(bareDescriptor("bare_one"), "", {}, nullptr, handle));
    ASSERT_TRUE(container.launch(bareDescriptor("bare_two"), "", {}, nullptr, handle));
    EXPECT_EQ(container.getAllPids().size(), 2u);

    container.terminateAll();
    EXPECT_TRUE(container.getAllPids().empty());
}

TEST(InProcContainerTest, TheSameModuleCannotBeLaunchedTwice)
{
    ASSERT_FALSE(bareModulePath().empty()) << "TEST_BARE_MODULE is not set";
    LogosCore::InProcContainer container;
    LogosCore::LoadedModuleHandle handle;

    ASSERT_TRUE(container.launch(bareDescriptor(), "", {}, nullptr, handle));
    EXPECT_FALSE(container.launch(bareDescriptor(), "", {}, nullptr, handle));
    container.terminate("bare_fixture");
}

// ── the format loader, and the pair ─────────────────────────────────────────

TEST(BareModuleFormatLoaderTest, ClaimsOnlyBareAndNeedsNoArguments)
{
    LogosCore::BareModuleFormatLoader loader;
    EXPECT_EQ(loader.id(), "bare");

    LogosCore::ModuleDescriptor desc = bareDescriptor();
    EXPECT_TRUE(loader.canHandle(desc));
    // Non-empty, because CompositeModuleLoader reads an empty host binary as
    // "cannot load".
    EXPECT_EQ(loader.resolveHostBinary(desc), desc.path);
    EXPECT_TRUE(loader.buildArguments(desc).empty());

    desc.format = "qt-plugin";
    EXPECT_FALSE(loader.canHandle(desc));
    desc.format.clear();
    EXPECT_FALSE(loader.canHandle(desc));
}

TEST(BareModuleFormatLoaderTest, PairsIntoAModuleLoaderTheRegistryUnderstands)
{
    ASSERT_FALSE(bareModulePath().empty()) << "TEST_BARE_MODULE is not set";

    auto container = std::make_shared<LogosCore::InProcContainer>();
    LogosCore::CompositeModuleLoader loader(
        container, std::make_shared<LogosCore::BareModuleFormatLoader>());

    EXPECT_EQ(loader.id(), "bare+inproc");
    EXPECT_TRUE(loader.canHandle(bareDescriptor()));

    LogosCore::LoadedModuleHandle handle;
    ASSERT_TRUE(loader.load(bareDescriptor(), nullptr, handle));
    EXPECT_EQ(handle.pid, -1);
    EXPECT_TRUE(loader.hasModule("bare_fixture"));
    EXPECT_TRUE(loader.sendToken("bare_fixture", "a-token"));
    loader.terminate("bare_fixture");
    EXPECT_FALSE(loader.hasModule("bare_fixture"));
}

// ── what the snapshot says about a module with no process ───────────────────

TEST(InProcModulesInfoTest, ReportsTheSentinelPidForAModuleWithNoProcess)
{
    // Straight at the registry: the container's own pid reporting is covered
    // above, and what this pins is the SNAPSHOT — the shape
    // logos_core_get_modules_info hands a consumer, which is where "is this
    // module in the Native container?" has to be answerable.
    ModuleRegistry registry;
    registry.registerModule("inproc_mod", "/modules/inproc_mod/inproc_mod_bare.so");

    auto entryFor = [&](const char* name) {
        for (const auto& e : registry.allModulesInfo())
            if (e.value("name", std::string()) == name) return e;
        return nlohmann::json(nullptr);
    };

    // Not loaded: pid is NULL, not -1. "no process" and "not running" are
    // different answers and the sentinel must not be used for both.
    auto before = entryFor("inproc_mod");
    ASSERT_TRUE(before.is_object());
    EXPECT_FALSE(before.value("loaded", true));
    EXPECT_TRUE(before.at("pid").is_null());

    LogosCore::LoadedModuleHandle handle;
    handle.name = "inproc_mod";
    handle.pid = LogosCore::InProcContainer::kInProcPid;
    handle.endpoint = "inproc://inproc_mod";
    registry.markLoaded("inproc_mod", nullptr, std::move(handle));

    auto after = entryFor("inproc_mod");
    ASSERT_TRUE(after.is_object());
    EXPECT_TRUE(after.value("loaded", false));
    ASSERT_TRUE(after.at("pid").is_number());
    EXPECT_EQ(after.at("pid").get<int64_t>(), -1);

    // `format` is the snapshot's other new field, and it is stamped at
    // DISCOVERY (processManifestModuleInternal) — which registerModule, the
    // direct graph mutator used here, does not go through. So it reads as the
    // Qt-plugin default even for a `_bare` path, and the entry always carries
    // the key: a consumer reading it never has to handle its absence.
    ASSERT_TRUE(after.contains("format"));
    EXPECT_EQ(after.at("format").get<std::string>(), std::string());
}

// ── two Bare modules in one process ─────────────────────────────────────────
//
// TEST_BARE_MODULE_TWIN is the SAME fixture sources built a second time: every
// module-impl C ABI symbol has the same name in both images, which is not an
// accident of the fixture but what the ABI requires of every Bare module. The
// container opens them RTLD_LOCAL for exactly this reason, and these are the
// tests that say so.

namespace {

std::string twinModulePath()
{
    const char* p = std::getenv("TEST_BARE_MODULE_TWIN");
    return p ? p : std::string();
}

// The rendezvous both deferred-dispatch tests need: a completion is delivered
// on the glue's worker thread and asserted on the test's, so every observation
// has to cross a lock and the test has to be able to WAIT for one rather than
// sample for it. Which thread delivered is recorded too — that a completion did
// not arrive on the delivering thread is one of the properties under test.
class CompletionCollector {
public:
    auto listener()
    {
        return [this](const QString& name, const QVariantList& data) {
            if (name != logos::callCompleteEvent() || data.size() != 2)
                return;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_ids << data.at(0).toString();
                m_values << data.at(1);
                m_threads.push_back(std::this_thread::get_id());
            }
            m_cv.notify_all();
        };
    }

    // False on timeout, so a caller can ASSERT_TRUE it and say what was missed.
    bool waitFor(int count, std::chrono::seconds timeout = std::chrono::seconds(10))
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, timeout, [&] { return m_ids.size() >= count; });
    }

    // Snapshots, all taken under the lock the listener writes under.
    QStringList ids() const { std::lock_guard<std::mutex> lock(m_mutex); return m_ids; }
    QVariant value(int i) const { std::lock_guard<std::mutex> lock(m_mutex); return m_values.at(i); }
    std::thread::id thread(int i) const { std::lock_guard<std::mutex> lock(m_mutex); return m_threads.at(i); }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    QStringList m_ids;
    QVariantList m_values;
    std::vector<std::thread::id> m_threads;
};

} // namespace

TEST(TwoBareModulesTest, IdenticalSymbolsResolveToDifferentImages)
{
    ASSERT_FALSE(bareModulePath().empty()) << "TEST_BARE_MODULE is not set";
    ASSERT_FALSE(twinModulePath().empty()) << "TEST_BARE_MODULE_TWIN is not set";
    ASSERT_NE(bareModulePath(), twinModulePath());

    LogosCore::BareModuleAbi first;
    LogosCore::BareModuleAbi second;
    std::string error;
    ASSERT_TRUE(LogosCore::openBareModule(bareModulePath(), first, &error)) << error;
    ASSERT_TRUE(LogosCore::openBareModule(twinModulePath(), second, &error)) << error;

    EXPECT_NE(first.handle, second.handle);
    // The property that matters. Both images export `logos_module_dispatch`;
    // under RTLD_GLOBAL the second dlopen would leave the first definition in
    // the process namespace and BOTH handles would resolve to one image, which
    // is silent — every call would land in whichever module was loaded first
    // and answer plausibly.
    EXPECT_NE(reinterpret_cast<void*>(first.dispatch),
              reinterpret_cast<void*>(second.dispatch))
        << "both handles resolved logos_module_dispatch to the same address: the "
           "images are sharing one definition";

    LogosCore::closeBareModule(first);
    LogosCore::closeBareModule(second);
}

TEST(TwoBareModulesTest, EachModuleKeepsItsOwnState)
{
    ASSERT_FALSE(bareModulePath().empty()) << "TEST_BARE_MODULE is not set";
    ASSERT_FALSE(twinModulePath().empty()) << "TEST_BARE_MODULE_TWIN is not set";

    LogosCore::InProcContainer container;
    LogosCore::LoadedModuleHandle handle;

    LogosCore::ModuleDescriptor a = bareDescriptor("bare_a");
    LogosCore::ModuleDescriptor b = bareDescriptor("bare_b");
    b.path = twinModulePath();

    ASSERT_TRUE(container.launch(a, "", {}, nullptr, handle));
    ASSERT_TRUE(container.launch(b, "", {}, nullptr, handle));

    // Drive each through its own glue: the fixture's counter is a module global,
    // so a shared image shows up as one counter answering for both names.
    LogosCore::BareModuleAbi abiA;
    LogosCore::BareModuleAbi abiB;
    std::string error;
    ASSERT_TRUE(LogosCore::openBareModule(bareModulePath(), abiA, &error)) << error;
    ASSERT_TRUE(LogosCore::openBareModule(twinModulePath(), abiB, &error)) << error;

    LogosCore::BareModuleGlue glueA("bare_a", "1.0.0", abiA);
    LogosCore::BareModuleGlue glueB("bare_b", "1.0.0", abiB);

    // RELATIVE readings. Both images outlive every unload in this process
    // (closeBareModule, #96), so each counter carries whatever an earlier test
    // bumped it by. What this test is about is which image a bump LANDS in, and
    // a delta says that as precisely as an absolute would — while an absolute
    // would also, silently, be asserting that no earlier test touched it.
    const long long beforeA = glueA.callMethod(QStringLiteral("total"), {}).toLongLong();
    const long long beforeB = glueB.callMethod(QStringLiteral("total"), {}).toLongLong();

    glueA.callMethod(QStringLiteral("bump"), QVariantList{5});
    glueB.callMethod(QStringLiteral("bump"), QVariantList{100});

    EXPECT_EQ(glueA.callMethod(QStringLiteral("total"), {}).toLongLong() - beforeA, 5);
    EXPECT_EQ(glueB.callMethod(QStringLiteral("total"), {}).toLongLong() - beforeB, 100);

    container.terminateAll();
    LogosCore::closeBareModule(abiA);
    LogosCore::closeBareModule(abiB);
}

// ── the dispatch leaves the delivering thread ───────────────────────────────

TEST_F(BareModuleGlueTest, ADeferredDispatchAnswersTheSentinelAndCompletesLater)
{
    CompletionCollector completions;
    m_glue->setEventListener(completions.listener());

    ensureQtApp();
    m_glue->enableDeferredDispatch();

    const QVariant answer = m_glue->callMethod(QStringLiteral("add"), QVariantList{2, 3});

    // NOT the result: the pending-call sentinel. This is the whole point — the
    // delivering thread is released rather than blocked, because in the Native
    // container it is the host's own Qt main thread and every reply the module
    // may be waiting for has to arrive on it.
    QString callId;
    ASSERT_TRUE(logos::isPendingCallSentinel(answer, &callId))
        << "a published in-process module must answer the sentinel, not the result";
    EXPECT_FALSE(callId.isEmpty());

    ASSERT_TRUE(completions.waitFor(1))
        << "no completion event arrived for the deferred dispatch";
    EXPECT_EQ(completions.ids().at(0), callId);
    EXPECT_EQ(completions.value(0).toLongLong(), 5);
    EXPECT_NE(completions.thread(0), std::this_thread::get_id())
        << "the handler ran on the delivering thread after all";
}

TEST_F(BareModuleGlueTest, DeferredDispatchesRunOneAtATimeInOrder)
{
    // logos_module_impl.h: "the host serializes dispatch calls (one at a time)".
    // One worker with one event loop is what keeps that true whatever the
    // module's declared concurrency, and the fixture's counter is what shows it:
    // ten increments through a shared global land as ten, in order.
    CompletionCollector completions;
    m_glue->setEventListener(completions.listener());

    ensureQtApp();
    m_glue->enableDeferredDispatch();

    QStringList issued;
    for (int i = 0; i < 10; ++i) {
        QString callId;
        const QVariant answer = m_glue->callMethod(QStringLiteral("bump"), QVariantList{1});
        ASSERT_TRUE(logos::isPendingCallSentinel(answer, &callId));
        issued << callId;
    }

    ASSERT_TRUE(completions.waitFor(10));
    EXPECT_EQ(completions.ids(), issued)
        << "completions did not arrive in the order the calls were made";

    // Ten serialized increments, not "somewhere between one and ten": the read
    // queues behind them and completes, so all eleven dispatches ran.
    QString finalId;
    const QVariant pending = m_glue->callMethod(QStringLiteral("total"), {});
    ASSERT_TRUE(logos::isPendingCallSentinel(pending, &finalId));
    ASSERT_TRUE(completions.waitFor(11));
    EXPECT_EQ(completions.ids().size(), 11);
}

TEST_F(BareModuleGlueTest, AnUnpublishedGlueStillAnswersInline)
{
    // Deferral is a promise to deliver over the event channel, and a glue the
    // container never published has none. It must therefore keep answering the
    // real value — which is also what every other test in this file relies on.
    const QVariant sum = m_glue->callMethod(QStringLiteral("add"), QVariantList{4, 4});
    QString ignored;
    EXPECT_FALSE(logos::isPendingCallSentinel(sum, &ignored));
    EXPECT_EQ(sum.toLongLong(), 8);
}

TEST(InProcContainerTest, TerminateStopsTheDispatchThreadBeforeClosingTheImage)
{
    // The reload is the assertion, as it is for the image itself: a container
    // that closed the image with its worker still inside it would crash here
    // rather than fail.
    ASSERT_FALSE(bareModulePath().empty()) << "TEST_BARE_MODULE is not set";
    LogosCore::InProcContainer container;
    LogosCore::LoadedModuleHandle handle;

    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(container.launch(bareDescriptor(), "", {}, nullptr, handle)) << "round " << i;
        container.terminate("bare_fixture");
        EXPECT_FALSE(container.hasModule("bare_fixture"));
    }
}

// ── the identity boundary between two in-process modules ────────────────────

TEST(TwoBareModulesTest, AnIdentityCannotPresentACredentialIssuedToAnother)
{
    // What the Native container buys per module, asserted at the seam it buys
    // it from. InProcContainer::launch calls LogosAPI::forIdentity (which
    // isolates) and sendToken calls TokenManager::adoptCredentialFor; the two
    // together are the whole of a Bare module's identity, and this is the
    // property they exist for: A's store answers A's credential and holds
    // nothing that was issued to C.
    //
    // The REJECTION is ModuleProxy's, and it is constant-time there
    // (logos-protocol's isAuthorized, covered by that repo's suite). What is
    // asserted here is the half this repo owns — that a token issued to C is
    // never in A's hand to present in the first place, so the comparison A can
    // provoke is against a value it does not hold.
    const QString a = QStringLiteral("bare_identity_a");
    const QString c = QStringLiteral("bare_identity_c");
    const QString target = QStringLiteral("bare_identity_target");

    ASSERT_TRUE(TokenManager::isolateIdentity(a));
    ASSERT_TRUE(TokenManager::isolateIdentity(c));
    ASSERT_TRUE(TokenManager::isIsolated(a));
    ASSERT_TRUE(TokenManager::isIsolated(c));

    ASSERT_TRUE(TokenManager::adoptCredentialFor(a, QStringLiteral("credential-for-a")));
    ASSERT_TRUE(TokenManager::adoptCredentialFor(c, QStringLiteral("credential-for-c")));

    for (const QString& key : TokenManager::bootstrapKeys()) {
        EXPECT_EQ(TokenManager::forIdentity(a).getToken(key), QStringLiteral("credential-for-a"));
        EXPECT_EQ(TokenManager::forIdentity(c).getToken(key), QStringLiteral("credential-for-c"));
    }

    // The pair token capability_module mints for C, filed in C's store the way
    // LogosAPIClient files it after a requestModule.
    TokenManager::forIdentity(c).saveToken(target, QStringLiteral("pair-token-c-to-target"));

    EXPECT_TRUE(TokenManager::forIdentity(a).getToken(target).isEmpty())
        << "identity A can see the token issued to C";
    EXPECT_FALSE(TokenManager::forIdentity(a).hasToken(target));

    // And neither leaked into the ambient ring, which is the store every
    // un-isolated caller in this image reads.
    EXPECT_TRUE(TokenManager::instance().getToken(target).isEmpty())
        << "an isolated identity's token reached the host's ambient ring";
}

// ── a call that races the unload ────────────────────────────────────────────
//
// The sentinel is a PROMISE: callMethod answered "the result will arrive on the
// event channel under this id", and the consumer transport then waits for that
// id — for its whole call timeout if nothing ever comes. So the one thing
// teardown may not do is take the promise's delivery mechanism away between the
// promise and the delivery. A consumer calling a Bare module while the host
// unloads it is not exotic: unload is asynchronous with every other module's
// traffic by construction.

TEST(BareModuleGlueUnloadRaceTest, EveryCallQueuedBeforeTheStopIsStillAnswered)
{
    ASSERT_FALSE(bareModulePath().empty()) << "TEST_BARE_MODULE is not set";
    ensureQtApp();

    // WHY A STALL, AND NOT JUST A TIGHT LOOP. QThread::quit() interrupts the
    // worker's event dispatcher, so the calls already POSTED behind the one in
    // flight are never delivered — measured here as 8 of 9 lost. That, not the
    // few instructions between "this glue defers" and the posting that follows,
    // is the wide half of the race, and it needs a dispatch to be genuinely in
    // flight when the stop lands. `stall` is the fixture method that puts one
    // there.
    constexpr int kRounds = 10;
    constexpr int kBurst = 8;

    for (int round = 0; round < kRounds; ++round) {
        LogosCore::BareModuleAbi abi;
        std::string error;
        ASSERT_TRUE(LogosCore::openBareModule(bareModulePath(), abi, &error)) << error;

        {
            LogosCore::BareModuleGlue glue("bare_fixture", "1.0.0", abi);
            CompletionCollector completions;
            glue.setEventListener(completions.listener());
            glue.enableDeferredDispatch();

            std::mutex issuedMutex;
            QStringList issued;

            auto issue = [&](const QString& method, const QVariantList& args) {
                QString callId;
                const QVariant answer = glue.callMethod(method, args);
                // A call that arrives after the stop is refused outright; only
                // a SENTINEL is a promise, and only a promise owes a completion.
                if (logos::isPendingCallSentinel(answer, &callId)) {
                    std::lock_guard<std::mutex> lock(issuedMutex);
                    issued << callId;
                }
            };

            auto issuedSoFar = [&] {
                std::lock_guard<std::mutex> lock(issuedMutex);
                return issued.size();
            };

            issue(QStringLiteral("stall"), QVariantList{120});

            // The burst is issued from a thread of its own, because that is the
            // shape of the race: the thread tearing a module down is never the
            // thread calling it.
            std::thread caller([&] {
                for (int i = 0; i < kBurst; ++i)
                    issue(QStringLiteral("add"), QVariantList{1, 1});
            });

            // Stop once a few are definitely queued behind the stall; the rest
            // of the burst is still being issued while this runs, which is the
            // concurrent half.
            while (issuedSoFar() < 3)
                std::this_thread::yield();
            // Grace enough for the stall to finish, so this is never the
            // "worker still inside the image" path.
            const bool stopped = glue.stopDispatch(5000);
            caller.join();
            EXPECT_TRUE(stopped) << "round " << round;

            QStringList expected;
            {
                std::lock_guard<std::mutex> lock(issuedMutex);
                expected = issued;
            }
            ASSERT_GT(expected.size(), 1) << "round " << round
                << ": nothing was queued behind the stall, so nothing raced";

            // Two seconds is already generous: every one of these either ran or
            // was answered before stopDispatch returned. The failure this
            // guards against is a caller burning its whole 10s call timeout
            // waiting for a completion that is never coming.
            ASSERT_TRUE(completions.waitFor(static_cast<int>(expected.size()),
                                            std::chrono::seconds(2)))
                << "round " << round << ": " << completions.ids().size() << " of "
                << expected.size() << " calls were answered; the rest are stranded "
                   "sentinels with no completion";
            // Same ids, same order, no duplicates — a call answered twice is as
            // wrong as one never answered.
            EXPECT_EQ(completions.ids(), expected) << "round " << round;
        }

        LogosCore::closeBareModule(abi);
    }
}

TEST(BareModuleGlueUnloadRaceTest, ACallThatArrivesAfterTheStopIsRefusedNotDispatched)
{
    ASSERT_FALSE(bareModulePath().empty()) << "TEST_BARE_MODULE is not set";
    ensureQtApp();

    LogosCore::BareModuleAbi abi;
    std::string error;
    ASSERT_TRUE(LogosCore::openBareModule(bareModulePath(), abi, &error)) << error;

    // The fixture's counter is a module global and the image is shared with
    // every other test in this process, so the reading is RELATIVE. Each probe
    // is its own scope: two live glues over one image would fight over the
    // module's single emit callback.
    auto readTotal = [&] {
        LogosCore::BareModuleGlue probe("bare_probe", "1.0.0", abi);
        return probe.callMethod(QStringLiteral("total"), {}).toLongLong();
    };

    const long long before = readTotal();

    {
        LogosCore::BareModuleGlue glue("bare_fixture", "1.0.0", abi);
        glue.enableDeferredDispatch();
        ASSERT_TRUE(glue.stopDispatch(5000));

        // aboutToUnload has already run by the time the container stops the
        // worker, so the module has been told it is going away. Answering the
        // caller's call by walking into that image anyway is the wrong half of
        // the bargain: what the caller needs is a prompt NO.
        QString callId;
        const QVariant refused = glue.callMethod(QStringLiteral("bump"), QVariantList{7});
        EXPECT_FALSE(logos::isPendingCallSentinel(refused, &callId))
            << "a retired glue promised a completion it has no worker to deliver";
        EXPECT_FALSE(refused.isValid())
            << "a refused void call must answer this slot's failure token";

        // A `result` method can say WHY, and it is the shape its callers
        // already unpack.
        const LogosResult described =
            glue.callMethod(QStringLiteral("describe"), {}).value<LogosResult>();
        EXPECT_FALSE(described.success);
        EXPECT_TRUE(described.error.toString().contains(QStringLiteral("bare_fixture")))
            << "the error should name the module that is going away; got: "
            << described.error.toString().toStdString();
    }

    EXPECT_EQ(readTotal(), before)
        << "the refused bump reached the module image anyway";

    LogosCore::closeBareModule(abi);
}

// ── what an in-process module COSTS ─────────────────────────────────────────
//
// process-stats measures a module by its pid, and this container's pid is the
// -1 sentinel, so it can say nothing at all about one — and its zeroes read on
// screen as "loaded and idle" rather than "never measured" (#86). The container
// is the one place that knows what the module actually is in this process: one
// image and one worker thread. These are the tests for measuring those.

namespace {

// An address certainly inside the TEST BINARY's image, for telling the host's
// image apart from a module's.
void anAddressInTheHostImage() {}

} // namespace

TEST(InProcModuleUsageTest, MeasuresALoadedModuleThatHasNoPid)
{
    ASSERT_FALSE(bareModulePath().empty()) << "TEST_BARE_MODULE is not set";
    LogosCore::InProcContainer container;
    LogosCore::LoadedModuleHandle handle;

    EXPECT_TRUE(container.getAllResourceUsage().empty())
        << "nothing is loaded, so there is nothing to measure";

    ASSERT_TRUE(container.launch(bareDescriptor(), "", {}, nullptr, handle));

    const auto usage = container.getAllResourceUsage();
    ASSERT_EQ(usage.size(), 1u);
    ASSERT_EQ(usage.count("bare_fixture"), 1u);
    const LogosCore::ModuleResourceUsage& measured = usage.at("bare_fixture");

    // The figure that #86 is about. A module that is loaded has an image, and
    // an image has a size: zero here is the answer that started this.
    EXPECT_GT(measured.memoryBytes, 0u);
    EXPECT_GE(measured.cpuTimeSeconds, 0.0);

    container.terminate("bare_fixture");
    EXPECT_TRUE(container.getAllResourceUsage().empty())
        << "a terminated module must stop being measured, not keep its last reading";
}

TEST(InProcModuleUsageTest, TheMemoryItReportsIsTheMODULESImage)
{
    ASSERT_FALSE(bareModulePath().empty()) << "TEST_BARE_MODULE is not set";
    LogosCore::InProcContainer container;
    LogosCore::LoadedModuleHandle handle;
    ASSERT_TRUE(container.launch(bareDescriptor(), "", {}, nullptr, handle));

    // dlopen is reference-counted, so opening the fixture a second time is the
    // SAME mapping the container is holding rather than a second copy of it —
    // which is what makes this an independent reading of the same image.
    LogosCore::BareModuleAbi probe;
    std::string error;
    ASSERT_TRUE(LogosCore::openBareModule(bareModulePath(), probe, &error)) << error;
    const ProcessStats::ImageStatsData moduleImage =
        ProcessStats::getImageStats(reinterpret_cast<const void*>(probe.dispatch));
    LogosCore::closeBareModule(probe);

    const ProcessStats::ImageStatsData hostImage =
        ProcessStats::getImageStats(reinterpret_cast<const void*>(&anAddressInTheHostImage));

    ASSERT_TRUE(moduleImage.resolved);
    ASSERT_TRUE(hostImage.resolved);
    ASSERT_NE(moduleImage.mappedBytes, hostImage.mappedBytes)
        << "the two images are the same size, so this test cannot tell them apart";

    const LogosCore::ModuleResourceUsage measured =
        container.getAllResourceUsage().at("bare_fixture");

    // Reporting the HOST's footprint for every in-process module would be the
    // easy wrong answer: it is a real number, it moves, and every module would
    // show the same one.
    EXPECT_LE(measured.memoryBytes, moduleImage.mappedBytes);
    EXPECT_NE(measured.memoryBytes, hostImage.mappedBytes);

    container.terminateAll();
}

TEST_F(BareModuleGlueTest, ChargesItsWorkerThreadsCpuToTheModule)
{
    // A dispatch runs on the glue's OWN thread (see the threading note in
    // bare_module_glue.h), which is what makes per-module CPU measurable in a
    // process shared by every module: the thread is the module's.
    EXPECT_FALSE(m_glue->dispatchCpuSeconds().has_value())
        << "there is no worker thread yet, so there is nothing to charge";

    CompletionCollector completions;
    m_glue->setEventListener(completions.listener());
    ensureQtApp();
    m_glue->enableDeferredDispatch();

    const std::optional<double> idle = m_glue->dispatchCpuSeconds();
    ASSERT_TRUE(idle.has_value()) << "a started worker thread must be measurable";

    // burn() WORKS, where stall() sleeps: a sleeping dispatch costs no CPU and
    // could not tell a working thread from an idle one. Its argument is a count
    // of iterations rather than a duration, so the CPU it costs does not depend
    // on what share of a core this machine is giving the test -- see the note
    // in the fixture.
    QString callId;
    ASSERT_TRUE(logos::isPendingCallSentinel(
        m_glue->callMethod(QStringLiteral("burn"), QVariantList{100}), &callId));
    ASSERT_TRUE(completions.waitFor(1)) << "the burn never completed";

    const std::optional<double> charged = m_glue->dispatchCpuSeconds();
    ASSERT_TRUE(charged.has_value());
    // A hundred million volatile additions. The floor is a small fraction of
    // what that actually costs: the claim under test is that the work landed on
    // THIS thread's account, not that the machine is a particular speed.
    EXPECT_GE(*charged - *idle, 0.02)
        << "the work done inside the module was not charged to its thread";

    ASSERT_TRUE(m_glue->stopDispatch(2000));
    EXPECT_FALSE(m_glue->dispatchCpuSeconds().has_value())
        << "the thread is gone; its handle must not be read again";
}

TEST(InProcModuleUsageTest, ACompositeLoaderPassesTheMeasurementThrough)
{
    // ModuleManager reaches every container through ModuleLoader, so a
    // measurement a container can make is worth nothing until that interface
    // carries it.
    ASSERT_FALSE(bareModulePath().empty()) << "TEST_BARE_MODULE is not set";

    auto container = std::make_shared<LogosCore::InProcContainer>();
    LogosCore::CompositeModuleLoader loader(container,
                                            std::make_shared<LogosCore::BareModuleFormatLoader>());

    LogosCore::LoadedModuleHandle handle;
    ASSERT_TRUE(loader.load(bareDescriptor(), nullptr, handle));

    const auto usage = loader.getAllResourceUsage();
    ASSERT_EQ(usage.count("bare_fixture"), 1u);
    EXPECT_GT(usage.at("bare_fixture").memoryBytes, 0u);

    loader.terminateAll();
}

// ── an image a module has RUN in is never unmapped (#96) ────────────────────
//
// The container's careful teardown — ask, stop the dispatch thread, unpublish,
// close — accounts for every thread the HOST made. It cannot account for the
// threads the MODULE made: a Bare module is a language core behind a C ABI, and
// neither the ABI nor the manifest has a word for "my runtime still has threads
// up". chat_module's tokio runtime and delivery_module's nim scheduler both do,
// and both answer logos_module_about_to_unload with "already quiescent",
// because from inside the module that is true.
//
// So an unmap after a correct unload is a use-after-free of CODE. It reads as a
// process that simply stops: on the Samsung it took the Shell down with no
// tombstone, no `logcat -b crash` line and no am_kill (#96). Whether it is seen
// at all is up to the loader — bionic really unmaps, glibc unmaps, dyld unmaps
// a bundle and keeps a framework — which is why this was an Android-only
// symptom of a portable defect.

namespace {

std::string threadedModulePath()
{
    const char* p = std::getenv("TEST_BARE_MODULE_THREADED");
    return p ? p : std::string();
}

// What the threaded fixture beats before its thread ends, from kBeats in
// bare_threaded_fixture_module.cpp. Only the drain at the end of the test needs
// it, and only to know when waiting further is pointless: a fixture that beat
// fewer times than this would still be caught by the assertions above.
constexpr int kThreadedFixtureBeats = 120;

// The threaded fixture's heartbeat file is append-only, one line per beat, so a
// line count IS a beat count — and a half-written final line cannot be counted
// as a beat that happened.
int beatsIn(const std::string& path)
{
    std::ifstream in(path);
    int beats = 0;
    for (std::string line; std::getline(in, line); )
        if (!line.empty())
            ++beats;
    return beats;
}

// Ask the loader whether `path` is still in this process, WITHOUT mapping it if
// it is not: that is exactly what RTLD_NOLOAD is for. The reference it hands
// back has to be given up again, or the probe would pin the very thing it came
// to measure.
bool imageIsStillMapped(const std::string& path)
{
#if defined(_WIN32)
    return true;
#else
    void* handle = ::dlopen(path.c_str(), RTLD_NOLOAD | RTLD_LAZY);
    if (handle)
        ::dlclose(handle);
    return handle != nullptr;
#endif
}

} // namespace

TEST(BareImageRetentionTest, AnImageAModuleHasRunInStaysMappedAfterTheUnload)
{
    ASSERT_FALSE(bareModulePath().empty()) << "TEST_BARE_MODULE is not set";
    LogosCore::InProcContainer container;
    LogosCore::LoadedModuleHandle handle;

    ASSERT_TRUE(container.launch(bareDescriptor(), "", {}, nullptr, handle));
    container.terminate("bare_fixture");

    EXPECT_TRUE(imageIsStillMapped(bareModulePath()))
        << "the unload gave the module's image back to the loader; anything the "
           "module left running is now executing unmapped pages (#96)";
    EXPECT_GE(LogosCore::retainedBareImageCount(), 1u)
        << "the host is not counting what it is keeping mapped";
}

TEST(BareImageRetentionTest, AModuleWhoseOwnThreadOutlivesTheUnloadDoesNotKillTheHost)
{
    ASSERT_FALSE(threadedModulePath().empty()) << "TEST_BARE_MODULE_THREADED is not set";

    const std::string heartbeat =
        (std::filesystem::temp_directory_path() /
         ("logos_bare_heartbeat_" +
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
          ".log")).string();
    std::filesystem::remove(heartbeat);

    LogosCore::InProcContainer container;
    LogosCore::LoadedModuleHandle handle;
    LogosCore::ModuleDescriptor desc = bareDescriptor("bare_threaded");
    desc.path = threadedModulePath();
    // The context is what starts the fixture's thread, and the path it names is
    // where that thread reports from — the one channel a module's own thread
    // has that outlives the module's memory.
    desc.instancePersistencePath = heartbeat;

    ASSERT_TRUE(container.launch(desc, "", {}, nullptr, handle));

    // Beating BEFORE the unload, or this test would pass just as well against a
    // fixture that never started a thread at all.
    int before = 0;
    for (int i = 0; i < 400 && before == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        before = beatsIn(heartbeat);
    }
    ASSERT_GT(before, 0) << "the fixture never started its thread";

    container.terminate("bare_threaded");

    // ...and beating AFTER it. On the old behaviour this line is not reached on
    // any loader that honours dlclose: the detached thread's next call lands in
    // an unmapped page and the whole process goes, which is what the Shell did
    // on the phone.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const int after = beatsIn(heartbeat);
    EXPECT_GT(after, before)
        << "the module's thread stopped at the unload, so this run never "
           "exercised a thread outliving its image";

    EXPECT_TRUE(imageIsStillMapped(threadedModulePath()))
        << "the image that thread is executing in was handed back to the loader";

    // Let the detached thread run out before the file goes: it writes straight
    // through, so removing the file under it only leaves a new one.
    for (int i = 0; i < 200 && beatsIn(heartbeat) < kThreadedFixtureBeats; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::filesystem::remove(heartbeat);
}
