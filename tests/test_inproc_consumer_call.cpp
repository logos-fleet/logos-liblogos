// A CONSUMER CALLING A PUBLISHED BARE MODULE, which is the one step
// test_inproc_container.cpp deliberately leaves out: everything there runs with
// no host API, so the glue is never published and never defers.
//
// Publication is what makes the call path interesting. A published glue answers
// the PENDING SENTINEL and completes later over the event channel
// (bare_module_glue.h), and the consumer transport is what turns that back into
// a value. In the Native container the consumer is in the SAME process, so that
// transport is qt_local — the one arm that did not implement the wait.
//
// Measured on the iPhone 16 Pro simulator (logos-workspace#53): the bundled
// bare_counter's `add(1, 2)` came back EMPTY with CallError::ok() true, and
// __logos_call_complete__ was logged after the caller had already failed. That
// is why the assertion here is on the VALUE and not on ok(): ok() was true
// throughout the bug.
//
// LOCAL MODE, explicitly, because that is the deployment this covers. A phone
// host runs the whole fleet in one process; Remote mode would publish this
// module on QtRO and exercise a transport that already had the wait.

#include <gtest/gtest.h>

#include "bare_module_abi.h"
#include "bare_module_fixture.h"
#include "inproc_container.h"
#include "module_registry.h"

#include <logos_api.h>
#include <logos_api_client.h>
#include <logos_async_dispatch.h>
#include <logos_call_error.h>
#include <logos_mode.h>
#include <token_manager.h>

#include <QString>
#include <QVariant>
#include <QVariantList>

#include <memory>
#include <string>

namespace {

using LogosTests::bareDescriptor;
using LogosTests::bareModulePath;
using LogosTests::ensureQtApp;

// The mode is process-global, so it is restored on the way out: every other
// test in this binary expects Remote.
class LocalModeGuard {
public:
    LocalModeGuard() { LogosModeConfig::setMode(LogosMode::Local); }
    ~LocalModeGuard() { LogosModeConfig::setMode(LogosMode::Remote); }
};

// One published Bare module plus the credential a caller has to present.
//
// sendToken is the core's own step — it adopts the credential the core minted
// as the module identity's anchor — so presenting that same value is what any
// caller does before capability_module has issued it a pair token. The
// consumer's side of it is an ordinary outbound entry, filed exactly where
// LogosAPIClient looks for one after a requestModule.
class PublishedBareModule {
public:
    PublishedBareModule(const char* moduleName, const char* callerName)
        : m_name(moduleName)
        , m_caller(QString::fromLatin1(callerName))
        , m_hostApi(QStringLiteral("core"))
    {
        m_container.setHostApi(&m_hostApi);
    }

    ~PublishedBareModule() { m_container.terminateAll(); }

    // Not in the constructor: ASSERT_* only works in a void function.
    void bringUp()
    {
        LogosCore::LoadedModuleHandle handle;
        ASSERT_TRUE(m_container.launch(bareDescriptor(m_name), "", {}, nullptr, handle))
            << "the container refused to launch the Bare fixture";
        ASSERT_TRUE(m_container.sendToken(m_name, credential().toStdString()));
        TokenManager::forIdentity(m_caller).saveToken(qname(), credential());
    }

    QString qname() const { return QString::fromStdString(m_name); }
    QString credential() const { return QStringLiteral("inproc-credential-for-") + qname(); }

    LogosAPIClient* client()
    {
        if (!m_consumerApi)
            m_consumerApi = std::make_unique<LogosAPI>(m_caller);
        return m_consumerApi->getClient(qname());
    }

private:
    std::string m_name;
    QString m_caller;
    LogosAPI m_hostApi;
    LogosCore::InProcContainer m_container;
    std::unique_ptr<LogosAPI> m_consumerApi;
};

} // namespace

// THE ACCEPTANCE CRITERION OF #53, at the seam the simulator failed at.
TEST(InProcConsumerCallTest, ASynchronousCallReturnsTheModulesOwnValue)
{
    ASSERT_FALSE(bareModulePath().empty()) << "TEST_BARE_MODULE is not set";
    ensureQtApp();
    const LocalModeGuard local;

    PublishedBareModule mod("bare_sync_call_target", "inproc_sync_caller");
    ASSERT_NO_FATAL_FAILURE(mod.bringUp());

    LogosAPIClient* client = mod.client();
    ASSERT_NE(client, nullptr);

    logos::CallError err;
    const QVariant sum = client->invokeRemoteMethod(
        mod.qname(), QStringLiteral("add"),
        QVariantList{ QVariant(1), QVariant(2) }, Timeout(10000), &err);

    EXPECT_TRUE(err.ok()) << err.code << ": " << err.message;

    // THE VALUE, not ok(). The pending sentinel reaching the caller is the exact
    // shape of the bug, and it converts to an empty/zero of every scalar type —
    // so an assertion on ok() alone, or on "not invalid", passes on it.
    QString leaked;
    ASSERT_FALSE(logos::isPendingCallSentinel(sum, &leaked))
        << "the pending-call sentinel reached the caller (call id "
        << leaked.toStdString() << "): the consumer did not await the completion";
    bool converted = false;
    const qlonglong value = sum.toLongLong(&converted);
    ASSERT_TRUE(converted) << "add(1, 2) answered a non-numeric " << sum.typeName();
    EXPECT_EQ(value, 3);
}

// A `void` method and a second call on the same handle: the completion channel
// has to survive the first call, and each call has to get its OWN answer.
TEST(InProcConsumerCallTest, EveryCallOnTheSameHandleGetsItsOwnAnswer)
{
    ASSERT_FALSE(bareModulePath().empty()) << "TEST_BARE_MODULE is not set";
    ensureQtApp();
    const LocalModeGuard local;

    PublishedBareModule mod("bare_repeat_call_target", "inproc_repeat_caller");
    ASSERT_NO_FATAL_FAILURE(mod.bringUp());

    LogosAPIClient* client = mod.client();
    ASSERT_NE(client, nullptr);

    for (int i = 0; i < 5; ++i) {
        logos::CallError err;
        const QVariant sum = client->invokeRemoteMethod(
            mod.qname(), QStringLiteral("add"),
            QVariantList{ QVariant(i), QVariant(100) }, Timeout(10000), &err);
        EXPECT_TRUE(err.ok()) << "round " << i << ": " << err.message;
        EXPECT_EQ(sum.toLongLong(), 100 + i) << "round " << i;
    }

    // The fixture's counter, driven through the same handle: `bump` is a void
    // method, so the glue's own answer is QVariant(true) rather than a number,
    // and `total` reads the state the bumps left behind. Both are deferred like
    // any other published dispatch.
    for (int i = 0; i < 3; ++i) {
        logos::CallError err;
        client->invokeRemoteMethod(mod.qname(), QStringLiteral("bump"),
                                   QVariantList{ QVariant(2) }, Timeout(10000), &err);
        EXPECT_TRUE(err.ok()) << "bump " << i << ": " << err.message;
    }

    logos::CallError err;
    const QVariant total = client->invokeRemoteMethod(
        mod.qname(), QStringLiteral("total"), QVariantList{}, Timeout(10000), &err);
    EXPECT_TRUE(err.ok()) << err.message;
    EXPECT_EQ(total.toLongLong(), 6)
        << "the three deferred bumps did not all reach the module";
}
