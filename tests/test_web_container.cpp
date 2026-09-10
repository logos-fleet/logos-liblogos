// The Web container, driven against a real web-transport peer.
//
// The far end of every case here is a PAGE STAND-IN: one endpoint of a real
// logos::web in-memory channel pair, with messages decoded and encoded by the
// transport's own codec (encodeWebMessage / decodeWebMessage). So what is
// exercised is the wire a browser speaks — the same JSON envelopes the browser
// JS SDK's WebPeer reads and writes — rather than a C++ double of it. Swap the
// pair for a webview's message bridge and nothing in the container changes;
// that swap is WebModuleView, and FakeView below is the smallest thing that
// satisfies it.
//
// No LogosAPI is handed to the container in these cases, exactly as in
// test_inproc_container: with no host API the container opens the view, relays
// to it and tears it down without publishing, so everything below runs with no
// core, no capability_module and no transport of its own. Publication is
// covered by the load path in test_module_manager.

#include <gtest/gtest.h>

#include "web_container.h"
#include "web_module_glue.h"
#include "web_module_loader.h"
#include "web_module_view.h"
#include "composite_module_loader.h"
#include "module_manager.h"

#include "module_registry.h"

#include <in_memory_channel.h>
#include <message_channel.h>
#include <web_message_codec.h>
#include <rpc_message.h>
#include <rpc_value.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QVariant>
#include <QVariantList>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using logos::plain::AnyMessage;
using logos::plain::CallMessage;
using logos::plain::EventMessage;
using logos::plain::MethodMetadata;
using logos::plain::MethodsMessage;
using logos::plain::MethodsResultMessage;
using logos::plain::ResultMessage;
using logos::plain::RpcValue;
using logos::plain::SubscribeMessage;
using logos::plain::TokenMessage;
using logos::plain::UnsubscribeMessage;

namespace {

// A whole number out of an RpcValue, whichever alternative the codec chose.
// A JSON `2` decodes as int64_t here and as a double after a round trip through
// a producer that wrote it that way, and a page must not care which.
int64_t asInteger(const RpcValue& v)
{
    if (v.isInt()) return v.asInt();
    if (v.isUInt()) return static_cast<int64_t>(v.asUInt());
    if (v.isDouble()) return static_cast<int64_t>(v.asDouble());
    return 0;
}

bool waitFor(const std::function<bool()>& pred, int budgetMs)
{
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

// The page: a `js_counter` provider answering on the wire, with nothing of the
// container's on this side of the channel.
//
// `serving` is what a page that has not finished loading looks like — it
// answers a Methods query with an empty contract, which is precisely the
// distinction awaitLoad exists to make.
class FakePage {
public:
    explicit FakePage(logos::web::MessageChannelPtr channel,
                      std::string object = "js_counter")
        : m_channel(std::move(channel)), m_object(std::move(object))
    {
        m_channel->setReceiver([this](const std::string& text) { onMessage(text); });
    }

    ~FakePage() { if (m_channel) m_channel->setReceiver(nullptr); }

    void setServing(bool serving) { m_serving = serving; }

    // Push an event at whoever subscribed, as a page's provider does when its
    // module emits.
    void emitEvent(const std::string& eventName, std::vector<RpcValue> data)
    {
        EventMessage ev;
        ev.object = m_object;
        ev.eventName = eventName;
        ev.data = std::move(data);
        m_channel->send(logos::web::encodeWebMessage(AnyMessage{ev}));
    }

    int calls() const { return m_calls.load(); }
    int subscribes() const { return m_subscribes.load(); }
    int tokenPushes() const { return m_tokenPushes.load(); }

    std::string lastTokenModule() const
    {
        std::lock_guard<std::mutex> g(m_mu);
        return m_lastTokenModule;
    }
    std::string lastToken() const
    {
        std::lock_guard<std::mutex> g(m_mu);
        return m_lastToken;
    }
    std::string lastCallAuthToken() const
    {
        std::lock_guard<std::mutex> g(m_mu);
        return m_lastCallAuthToken;
    }

private:
    void onMessage(const std::string& text)
    {
        AnyMessage msg;
        try {
            msg = logos::web::decodeWebMessage(text);
        } catch (...) {
            return;
        }

        if (auto* call = std::get_if<CallMessage>(&msg)) {
            ++m_calls;
            {
                std::lock_guard<std::mutex> g(m_mu);
                m_lastCallAuthToken = call->authToken;
            }
            ResultMessage res;
            res.id = call->id;
            if (call->method == "add" && call->args.size() == 2) {
                res.ok = true;
                res.value = RpcValue(asInteger(call->args[0]) + asInteger(call->args[1]));
            } else if (call->method == "echo" && !call->args.empty()) {
                res.ok = true;
                res.value = call->args[0];
            } else {
                res.ok = false;
                res.err = "no such method: " + call->method;
                res.errCode = "METHOD_NOT_FOUND";
            }
            m_channel->send(logos::web::encodeWebMessage(AnyMessage{res}));
            return;
        }

        if (auto* q = std::get_if<MethodsMessage>(&msg)) {
            MethodsResultMessage res;
            res.id = q->id;
            res.ok = true;
            if (m_serving) {
                MethodMetadata add;
                add.name = "add";
                add.signature = "add(int,int)";
                add.returnType = "int";
                MethodMetadata counted;
                counted.name = "counted";
                counted.signature = "counted(int)";
                counted.returnType = "void";
                res.methods = { add, counted };
            }
            m_channel->send(logos::web::encodeWebMessage(AnyMessage{res}));
            return;
        }

        if (std::get_if<SubscribeMessage>(&msg)) { ++m_subscribes; return; }
        if (std::get_if<UnsubscribeMessage>(&msg)) { ++m_unsubscribes; return; }

        if (auto* tok = std::get_if<TokenMessage>(&msg)) {
            ++m_tokenPushes;
            std::lock_guard<std::mutex> g(m_mu);
            m_lastTokenModule = tok->moduleName;
            m_lastToken = tok->token;
            return;
        }
    }

    logos::web::MessageChannelPtr m_channel;
    std::string m_object;
    std::atomic<bool> m_serving{true};
    std::atomic<int> m_calls{0};
    std::atomic<int> m_subscribes{0};
    std::atomic<int> m_unsubscribes{0};
    std::atomic<int> m_tokenPushes{0};
    mutable std::mutex m_mu;
    std::string m_lastTokenModule;
    std::string m_lastToken;
    std::string m_lastCallAuthToken;
};

// A view over one endpoint of the pair. Killing it is what killing a renderer
// looks like from the container's side: the channel closes and the death
// callback fires.
class FakeView : public LogosCore::WebModuleView {
public:
    FakeView(logos::web::MessageChannelPtr channel, std::optional<int64_t> pid)
        : m_channel(std::move(channel)), m_pid(pid) {}

    ~FakeView() override { m_channel->close(); }

    logos::web::MessageChannelPtr channel() const override { return m_channel; }
    std::optional<int64_t> pid() const override { return m_pid; }
    void setOnDied(std::function<void()> cb) override { m_onDied = std::move(cb); }
    bool isAlive() const override { return m_alive && m_channel->isOpen(); }

    // The renderer went away.
    void kill()
    {
        m_alive = false;
        m_channel->close();
        if (m_onDied) m_onDied();
    }

private:
    logos::web::MessageChannelPtr m_channel;
    std::optional<int64_t> m_pid;
    std::function<void()> m_onDied;
    std::atomic<bool> m_alive{true};
};

// Installs a view factory for the life of the test and takes it away again:
// the factory is process-wide, so a leaked one makes later cases
// order-dependent.
class ViewBackend {
public:
    ViewBackend()
    {
        LogosCore::setWebModuleViewFactory(
            [this](const LogosCore::WebModuleViewRequest& req)
                -> std::unique_ptr<LogosCore::WebModuleView> {
                auto pair = logos::web::makeInMemoryChannelPair();
                {
                    std::lock_guard<std::mutex> g(m_mu);
                    m_lastRequest = req;
                    m_pages.push_back(std::make_unique<FakePage>(pair.second, req.moduleName));
                    auto view = std::make_unique<FakeView>(pair.first, m_pid);
                    m_views.push_back(view.get());
                    return view;
                }
            });
    }

    ~ViewBackend() { LogosCore::setWebModuleViewFactory({}); }

    void setPid(std::optional<int64_t> pid) { m_pid = pid; }

    FakePage& page(size_t i = 0)
    {
        std::lock_guard<std::mutex> g(m_mu);
        return *m_pages.at(i);
    }
    FakeView& view(size_t i = 0)
    {
        std::lock_guard<std::mutex> g(m_mu);
        return *m_views.at(i);
    }
    LogosCore::WebModuleViewRequest lastRequest() const
    {
        std::lock_guard<std::mutex> g(m_mu);
        return m_lastRequest;
    }
    size_t viewCount() const
    {
        std::lock_guard<std::mutex> g(m_mu);
        return m_views.size();
    }

private:
    mutable std::mutex m_mu;
    std::vector<std::unique_ptr<FakePage>> m_pages;
    std::vector<FakeView*> m_views;   // owned by the container
    LogosCore::WebModuleViewRequest m_lastRequest;
    std::optional<int64_t> m_pid;
};

LogosCore::ModuleDescriptor webDescriptor(const std::string& name = "js_counter")
{
    LogosCore::ModuleDescriptor desc;
    desc.name = name;
    desc.path = "/modules/" + name + "/index.html";
    desc.format = "web";
    return desc;
}

} // namespace

// ── the format loader ───────────────────────────────────────────────────────

TEST(WebModuleFormatLoaderTest, ClaimsOnlyWebAndNeedsNoArguments)
{
    LogosCore::WebModuleFormatLoader loader;
    EXPECT_EQ(loader.id(), "web");

    EXPECT_TRUE(loader.canHandle(webDescriptor()));

    LogosCore::ModuleDescriptor qt = webDescriptor();
    qt.format = "qt-plugin";
    EXPECT_FALSE(loader.canHandle(qt));

    // The EMPTY format is the Qt loader's, and claiming it here would make the
    // answer depend on registration order.
    LogosCore::ModuleDescriptor unstamped = webDescriptor();
    unstamped.format.clear();
    EXPECT_FALSE(loader.canHandle(unstamped));

    LogosCore::ModuleDescriptor bare = webDescriptor();
    bare.format = "bare";
    EXPECT_FALSE(loader.canHandle(bare));

    EXPECT_EQ(loader.resolveHostBinary(webDescriptor()), webDescriptor().path);
    EXPECT_TRUE(loader.buildArguments(webDescriptor()).empty());
}

// ── the container's claim ───────────────────────────────────────────────────

TEST(WebContainerTest, ClaimsOnlyWebModules)
{
    LogosCore::WebContainer container;
    EXPECT_EQ(container.id(), "web");
    EXPECT_TRUE(container.canHandle(webDescriptor()));

    LogosCore::ModuleDescriptor bare = webDescriptor();
    bare.format = "bare";
    EXPECT_FALSE(container.canHandle(bare));

    LogosCore::ModuleDescriptor qt = webDescriptor();
    qt.format = "qt-plugin";
    EXPECT_FALSE(container.canHandle(qt));
}

TEST(WebContainerTest, WithNoBackendInstalledTheLoadIsAnErrorNotACrash)
{
    LogosCore::setWebModuleViewFactory({});
    LogosCore::WebContainer container;
    LogosCore::LoadedModuleHandle handle;
    EXPECT_FALSE(container.launch(webDescriptor(), "", {}, {}, handle));
    EXPECT_FALSE(container.hasModule("js_counter"));
}

// ── the relay ───────────────────────────────────────────────────────────────

TEST(WebContainerTest, RelaysACallToThePageAndAnswersItsResult)
{
    ViewBackend backend;
    LogosCore::WebContainer container;
    LogosCore::LoadedModuleHandle handle;
    ASSERT_TRUE(container.launch(webDescriptor(), "", {}, {}, handle));

    auto* glue = container.glueFor("js_counter");
    ASSERT_NE(glue, nullptr);

    // The acceptance criterion, at the seam it is decided: add(1,2) is 3, and
    // the 3 came back over the web transport from the page.
    const QVariant sum = glue->callMethod(QStringLiteral("add"), { 1, 2 });
    EXPECT_EQ(sum.toInt(), 3);
    EXPECT_EQ(backend.page().calls(), 1);

    // The view was asked for the module's own entry document and directory.
    EXPECT_EQ(backend.lastRequest().moduleName, "js_counter");
    EXPECT_EQ(backend.lastRequest().entryPath, "/modules/js_counter/index.html");
    EXPECT_EQ(backend.lastRequest().moduleDir, "/modules/js_counter");

    container.terminateAll();
}

TEST(WebContainerTest, AMethodThePageRefusesIsAnInvalidVariantNotACrash)
{
    ViewBackend backend;
    LogosCore::WebContainer container;
    LogosCore::LoadedModuleHandle handle;
    ASSERT_TRUE(container.launch(webDescriptor(), "", {}, {}, handle));

    const QVariant result =
        container.glueFor("js_counter")->callMethod(QStringLiteral("nope"), {});
    EXPECT_FALSE(result.isValid());

    container.terminateAll();
}

TEST(WebContainerTest, TheContractIsReadFromThePageNotGenerated)
{
    ViewBackend backend;
    LogosCore::WebContainer container;
    LogosCore::LoadedModuleHandle handle;
    ASSERT_TRUE(container.launch(webDescriptor(), "", {}, {}, handle));

    const QJsonArray methods = container.glueFor("js_counter")->getMethods();
    ASSERT_EQ(methods.size(), 2);
    EXPECT_EQ(methods[0].toObject().value("name").toString(), QStringLiteral("add"));
    EXPECT_EQ(methods[1].toObject().value("name").toString(), QStringLiteral("counted"));

    container.terminateAll();
}

TEST(WebContainerTest, AnEventEmittedByThePageReachesANativeListener)
{
    ViewBackend backend;
    LogosCore::WebContainer container;
    LogosCore::LoadedModuleHandle handle;
    ASSERT_TRUE(container.launch(webDescriptor(), "", {}, {}, handle));

    std::mutex mu;
    std::vector<std::pair<QString, QVariantList>> seen;
    container.glueFor("js_counter")->setEventListener(
        [&](const QString& name, const QVariantList& data) {
            std::lock_guard<std::mutex> g(mu);
            seen.emplace_back(name, data);
        });

    // The subscription has to be ON THE WIRE before the page emits, or the
    // emission has nowhere to go — the same ordering every event test has.
    ASSERT_TRUE(waitFor([&] { return backend.page().subscribes() > 0; }, 2000));

    backend.page().emitEvent("counted", { RpcValue(static_cast<int64_t>(7)) });

    ASSERT_TRUE(waitFor([&] {
        std::lock_guard<std::mutex> g(mu);
        return !seen.empty();
    }, 2000));

    std::lock_guard<std::mutex> g(mu);
    EXPECT_EQ(seen[0].first, QStringLiteral("counted"));
    ASSERT_EQ(seen[0].second.size(), 1);
    EXPECT_EQ(seen[0].second[0].toInt(), 7);

    container.terminateAll();
}

TEST(WebContainerTest, ATokenPushIsRelayedToThePage)
{
    ViewBackend backend;
    LogosCore::WebContainer container;
    LogosCore::LoadedModuleHandle handle;
    ASSERT_TRUE(container.launch(webDescriptor(), "", {}, {}, handle));

    // The core's own delivery path: sendToken is what ModuleManager calls right
    // after a successful load, and for a page it has to cross the channel or
    // the module never learns its own credential.
    EXPECT_TRUE(container.sendToken("js_counter", "root-credential"));
    ASSERT_TRUE(waitFor([&] { return backend.page().tokenPushes() > 0; }, 2000));
    EXPECT_EQ(backend.page().lastTokenModule(), "js_counter");
    EXPECT_EQ(backend.page().lastToken(), "root-credential");

    // And it is the credential the relay presents from then on, so the page's
    // own ModuleProxy can validate an inbound call with transport tag "web".
    container.glueFor("js_counter")->callMethod(QStringLiteral("add"), { 1, 1 });
    EXPECT_EQ(backend.page().lastCallAuthToken(), "root-credential");

    container.terminateAll();
}

// ── the load verdict ────────────────────────────────────────────────────────

TEST(WebContainerTest, AwaitLoadAsksThePageWhetherItPublishedItsModule)
{
    ViewBackend backend;
    LogosCore::WebContainer container;
    LogosCore::LoadedModuleHandle handle;
    ASSERT_TRUE(container.launch(webDescriptor(), "", {}, {}, handle));

    const LogosCore::LoadOutcome ok =
        container.awaitLoad("js_counter", std::chrono::milliseconds(2000));
    EXPECT_EQ(ok.verdict, LogosCore::LoadVerdict::Loaded);

    container.terminateAll();
}

TEST(WebContainerTest, APageThatNeverPublishesIsAFailedLoadNotALoadedOne)
{
    ViewBackend backend;
    LogosCore::WebContainer container;
    LogosCore::LoadedModuleHandle handle;
    ASSERT_TRUE(container.launch(webDescriptor(), "", {}, {}, handle));
    backend.page().setServing(false);

    const LogosCore::LoadOutcome outcome =
        container.awaitLoad("js_counter", std::chrono::milliseconds(300));
    EXPECT_EQ(outcome.verdict, LogosCore::LoadVerdict::Failed);
    EXPECT_FALSE(outcome.reason.empty());

    container.terminateAll();
}

// ── lifecycle ───────────────────────────────────────────────────────────────

TEST(WebContainerTest, ReportsTheViewsPidSoAKilledRendererIsNameable)
{
    ViewBackend backend;
    backend.setPid(4242);
    LogosCore::WebContainer container;
    LogosCore::LoadedModuleHandle handle;
    ASSERT_TRUE(container.launch(webDescriptor(), "", {}, {}, handle));

    EXPECT_EQ(handle.pid, 4242);
    ASSERT_TRUE(container.pid("js_counter").has_value());
    EXPECT_EQ(*container.pid("js_counter"), 4242);
    EXPECT_EQ(container.getAllPids().at("js_counter"), 4242);

    container.terminateAll();
}

TEST(WebContainerTest, AViewWithNoProcessOfItsOwnReportsTheSentinel)
{
    ViewBackend backend;   // no pid
    LogosCore::WebContainer container;
    LogosCore::LoadedModuleHandle handle;
    ASSERT_TRUE(container.launch(webDescriptor(), "", {}, {}, handle));
    EXPECT_EQ(handle.pid, LogosCore::WebContainer::kNoPid);
    container.terminateAll();
}

TEST(WebContainerTest, KillingThePageAnnouncesTerminationExactlyOnce)
{
    ViewBackend backend;
    LogosCore::WebContainer container;
    LogosCore::LoadedModuleHandle handle;

    std::atomic<int> announced{0};
    ASSERT_TRUE(container.launch(webDescriptor(), "", {},
                                 [&](const std::string&) { ++announced; }, handle));

    backend.view().kill();
    ASSERT_TRUE(waitFor([&] { return announced.load() > 0; }, 2000));
    EXPECT_EQ(announced.load(), 1);
    EXPECT_FALSE(container.hasModule("js_counter"));

    // And the deliberate teardown that follows does not announce it a second
    // time — the module is already gone.
    container.terminateAll();
    EXPECT_EQ(announced.load(), 1);
}

TEST(WebContainerTest, KillingOnePageLeavesTheOtherModuleAlone)
{
    ViewBackend backend;
    LogosCore::WebContainer container;
    LogosCore::LoadedModuleHandle a, b;
    ASSERT_TRUE(container.launch(webDescriptor("js_counter"), "", {}, {}, a));
    ASSERT_TRUE(container.launch(webDescriptor("js_other"), "", {}, {}, b));
    ASSERT_EQ(backend.viewCount(), 2u);

    backend.view(0).kill();
    ASSERT_TRUE(waitFor([&] { return !container.hasModule("js_counter"); }, 2000));

    EXPECT_TRUE(container.hasModule("js_other"));
    EXPECT_EQ(container.glueFor("js_other")
                  ->callMethod(QStringLiteral("add"), { 2, 3 }).toInt(), 5);

    container.terminateAll();
}

TEST(WebContainerTest, TheSameModuleCannotBeLaunchedTwice)
{
    ViewBackend backend;
    LogosCore::WebContainer container;
    LogosCore::LoadedModuleHandle first, second;
    ASSERT_TRUE(container.launch(webDescriptor(), "", {}, {}, first));
    EXPECT_FALSE(container.launch(webDescriptor(), "", {}, {}, second));
    container.terminateAll();
}

TEST(WebContainerTest, UnloadClosesTheViewSoAReloadGetsAFreshPage)
{
    ViewBackend backend;
    LogosCore::WebContainer container;
    LogosCore::LoadedModuleHandle handle;
    ASSERT_TRUE(container.launch(webDescriptor(), "", {}, {}, handle));
    container.terminate("js_counter");
    EXPECT_FALSE(container.hasModule("js_counter"));

    ASSERT_TRUE(container.launch(webDescriptor(), "", {}, {}, handle));
    EXPECT_EQ(backend.viewCount(), 2u);
    EXPECT_EQ(container.glueFor("js_counter")
                  ->callMethod(QStringLiteral("add"), { 4, 5 }).toInt(), 9);
    container.terminateAll();
}

TEST(WebContainerTest, TerminateAllStopsEveryModule)
{
    ViewBackend backend;
    LogosCore::WebContainer container;
    LogosCore::LoadedModuleHandle a, b;
    ASSERT_TRUE(container.launch(webDescriptor("js_counter"), "", {}, {}, a));
    ASSERT_TRUE(container.launch(webDescriptor("js_other"), "", {}, {}, b));

    container.terminateAll();
    EXPECT_FALSE(container.hasModule("js_counter"));
    EXPECT_FALSE(container.hasModule("js_other"));
    EXPECT_TRUE(container.getAllPids().empty());
}

// ── the pair the registry understands ───────────────────────────────────────

TEST(WebContainerTest, PairsIntoAModuleLoaderTheRegistryUnderstands)
{
    ViewBackend backend;
    auto container = std::make_shared<LogosCore::WebContainer>();
    auto loader = std::make_shared<LogosCore::WebModuleFormatLoader>();
    LogosCore::CompositeModuleLoader composite(container, loader);

    EXPECT_TRUE(composite.canHandle(webDescriptor()));

    LogosCore::ModuleDescriptor bare = webDescriptor();
    bare.format = "bare";
    EXPECT_FALSE(composite.canHandle(bare));

    LogosCore::LoadedModuleHandle handle;
    ASSERT_TRUE(composite.load(webDescriptor(), {}, handle));
    EXPECT_TRUE(composite.hasModule("js_counter"));
    EXPECT_EQ(handle.endpoint, "web://js_counter");
    composite.terminateAll();
}

// ── the operator's assertion ────────────────────────────────────────────────
//
// `--container web` is an ASSERTION, so the vocabulary it accepts is part of
// the contract: a value the runtime silently ignored would make the flag mean
// "web if you happen to have built it that way".

TEST(WebContainerPolicyTest, WebIsAContainerTheOperatorCanAssert)
{
    const std::string saved = ModuleManager::containerPolicy();

    EXPECT_TRUE(ModuleManager::setContainerPolicy("web"));
    EXPECT_EQ(ModuleManager::containerPolicy(), "web");

    // An unrecognised value is refused and leaves the previous policy in
    // place — never silently downgraded to "auto", which would be a wide-open
    // runtime that looks like a constrained one.
    EXPECT_FALSE(ModuleManager::setContainerPolicy("webview"));
    EXPECT_EQ(ModuleManager::containerPolicy(), "web");

    ModuleManager::setContainerPolicy(saved);
}

// ── discovery: what makes a package a `web` module in the first place ───────
//
// The container above answers for a descriptor that is ALREADY stamped
// `format = "web"`. Nothing stamps it but ModuleRegistry::discoverInstalledModules,
// and it does so on the filename alone — so the rule that decides which of the
// three arms a scanned package takes has to be asserted on real directories,
// not on a hand-built descriptor.

namespace {

// A package directory the package manager will scan: manifest.json declaring a
// plain-string `main`, and that file, written where the manifest says. The same
// shape createFakeModule builds in test_module_manager.cpp, kept local because
// this file drives ModuleRegistry directly rather than through the C API.
void plantPackage(const std::filesystem::path& modulesDir,
                  const std::string& name,
                  const std::string& mainFile)
{
    const std::filesystem::path dir = modulesDir / name;
    std::filesystem::create_directories(dir);

    const nlohmann::json manifest{
        {"name", name},
        {"version", "1.0.0"},
        {"type", "core"},
        {"main", mainFile},
        {"description", "a planted package"},
    };
    std::ofstream(dir / "manifest.json") << manifest.dump();
    std::ofstream(dir / mainFile) << "not read by discovery";
}

// A scratch modules directory that removes itself.
class ScopedModulesDir {
public:
    ScopedModulesDir()
        : m_path(std::filesystem::temp_directory_path()
                 / ("logos-web-discovery-" + std::to_string(::getpid()) + "-"
                    + std::to_string(s_counter++)))
    {
        std::filesystem::create_directories(m_path);
    }
    ~ScopedModulesDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    const std::filesystem::path& path() const { return m_path; }
    std::string str() const { return m_path.string(); }

private:
    std::filesystem::path m_path;
    static std::atomic<int> s_counter;
};

std::atomic<int> ScopedModulesDir::s_counter{0};

// The `format` discovery stamped on `name`, or "<absent>" when the scan did not
// register it at all.
std::string discoveredFormat(const ScopedModulesDir& dir, const std::string& name)
{
    ModuleRegistry registry;
    registry.setModulesDir(dir.str());
    registry.discoverInstalledModules();

    for (const auto& entry : registry.allModulesInfo()) {
        if (entry.value("name", std::string()) == name)
            return entry.value("format", std::string());
    }
    return "<absent>";
}

} // namespace

TEST(WebDiscoveryTest, AnHtmlEntryDocumentIsStampedWeb)
{
    ScopedModulesDir dir;
    plantPackage(dir.path(), "js_counter", "index.html");

    // The whole reason the container ever sees `format == "web"`.
    EXPECT_EQ(discoveredFormat(dir, "js_counter"), "web");
}

TEST(WebDiscoveryTest, TheExtensionIsMatchedCaseInsensitivelyAndHtmCounts)
{
    // `.htm` because it is a page, and case-folded because a filename's case is
    // not the author's statement about anything — on a case-insensitive
    // filesystem it is not even stable.
    ScopedModulesDir upper;
    plantPackage(upper.path(), "shouty_page", "INDEX.HTML");
    EXPECT_EQ(discoveredFormat(upper, "shouty_page"), "web");

    ScopedModulesDir htm;
    plantPackage(htm.path(), "short_page", "index.htm");
    EXPECT_EQ(discoveredFormat(htm, "short_page"), "web");

    ScopedModulesDir mixed;
    plantPackage(mixed.path(), "mixed_page", "Index.Htm");
    EXPECT_EQ(discoveredFormat(mixed, "mixed_page"), "web");
}

TEST(WebDiscoveryTest, ABareImageIsStillBareAndAPluginIsStillNeitherArm)
{
    // The three arms are mutually exclusive, and this is the assertion that
    // adding the `web` arm did not move anything that used to take another one.
    ScopedModulesDir bare;
    plantPackage(bare.path(), "counter_module", "counter_module_bare.dylib");
    EXPECT_EQ(discoveredFormat(bare, "counter_module"), "bare");

    // A Qt plugin takes the third arm, which OPENS the file — and this one is
    // a text file, so it is refused rather than registered. That refusal is the
    // point: a non-page, non-Bare artifact never reaches the Web container by
    // accident, it goes to the loader that inspects it.
    ScopedModulesDir plugin;
    plantPackage(plugin.path(), "plugin_module", "plugin_module_plugin.dylib");
    EXPECT_EQ(discoveredFormat(plugin, "plugin_module"), "<absent>");
}

TEST(WebDiscoveryTest, AnHtmlNamedLikeABareImageIsBare)
{
    // Both gates are filename rules and this document satisfies both. The Bare
    // gate is asked FIRST, so it wins — recorded here because the answer is
    // arbitrary-looking and a reader deserves to find it asserted rather than
    // inferred from the order of two `if`s.
    ScopedModulesDir dir;
    plantPackage(dir.path(), "ambiguous_module", "ambiguous_module_bare.html");
    EXPECT_EQ(discoveredFormat(dir, "ambiguous_module"), "bare");
}
