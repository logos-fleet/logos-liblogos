#include "web_container.h"
#include "web_call_router.h"
#include "web_module_glue.h"
#include "web_qt_dispatch.h"

#include <logos_api.h>
#include <logos_api_provider.h>
#include <token_manager.h>
#include <logos_object.h>
#include <logos_transport_config_json.h>

#include <web_transport_connection.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QString>
#include <QThread>
#include <QTimer>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

namespace fs = std::filesystem;

namespace LogosCore {

// One loaded web module: the view, the conversation over its channel, and the
// LogosAPI it is published on. Destruction order is load-bearing and is why
// this is a struct with an explicit teardown rather than four parallel maps —
// the glue holds a handle over the connection, and the connection's peer is
// serviced by the view's channel.
struct WebContainer::Instance {
    std::string name;
    std::unique_ptr<WebModuleView> view;
    // The page's outbound half, built BEFORE the connection because the
    // connection is what serves it. Only one of the two is ever set: `routes`
    // is what the container builds over this module's own LogosAPI, and a host
    // that injected its own keeps ownership of it.
    std::unique_ptr<WebHostRoutes> routes;
    std::unique_ptr<WebCallRouter> router;
    std::unique_ptr<logos::web::WebTransportConnection> connection;
    std::unique_ptr<WebModuleGlue> glue;
    LogosAPI* api = nullptr;
    std::function<void(const std::string&)> onTerminated;
    int64_t pid = WebContainer::kNoPid;
};

namespace {

// What a module whose manifest declares no version is published as, matching
// the Native container's answer for the same question.
constexpr const char* kDefaultModuleVersion = "1.0.0";

// How often awaitLoad turns the event loop while it is waiting on a page.
constexpr int kPageReadyPollMs = 10;

// What a page that never answered the contract query is reported as.
constexpr const char* kNeverPublished =
    "the page never published a module: it answered the contract query with "
    "nothing before the deadline";

} // namespace

WebContainer::WebContainer() = default;

WebContainer::~WebContainer()
{
    terminateAll();
    // Anything still parked has no later to be swept in. The token goes with
    // this object, so a sweep already armed finds it expired and does nothing.
    tearDownAllRetired();
}

void WebContainer::setHostApi(LogosAPI* hostApi)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hostApi = hostApi;
}

void WebContainer::setHostRoutes(WebHostRoutes* routes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hostRoutes = routes;
}

bool WebContainer::canHandle(const ModuleDescriptor& desc) const
{
    return desc.format == "web";
}

bool WebContainer::launch(const ModuleDescriptor& desc,
                          const std::string& /*hostBinary*/,
                          const std::vector<std::string>& /*args*/,
                          std::function<void(const std::string& name)> onTerminated,
                          LoadedModuleHandle& out)
{
    // A BACKSTOP, and it should find nothing: a lost page is destroyed where it
    // is noticed unless a call into it was waiting, and the sweep that follows
    // such a wait runs long before anyone gets round to loading the module
    // again. It is here because of what a leftover would mean — the old
    // LogosAPI still publishing the name this load is about to take, and a
    // module that answers nothing.
    sweepRetired();

    // The factory is fetched OUTSIDE the container's lock and invoked outside
    // it too: opening a webview is host code that may take locks of its own.
    WebModuleViewFactory factory = webModuleViewFactory();
    if (!factory) {
        spdlog::error("Cannot load web module {}: this process has no webview "
                      "backend installed (see LogosCore::setWebModuleViewFactory). "
                      "A `web` module is a page, and a process with no webview in "
                      "it genuinely cannot run one.", desc.name);
        return false;
    }

    // The two injected pointers are read HERE, under the lock their setters
    // take, and used for the rest of this function: a host installs them once
    // at startup, but reading them unlocked while setHostApi may still be
    // running would be a race with no upside.
    LogosAPI* hostApi = nullptr;
    WebHostRoutes* hostRoutes = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_modules.count(desc.name)) {
            spdlog::warn("Web module already running: {}", desc.name);
            return false;
        }
        hostApi = m_hostApi;
        hostRoutes = m_hostRoutes;
    }

    WebModuleViewRequest request;
    request.moduleName = desc.name;
    request.entryPath = desc.path;
    request.moduleDir = desc.path.empty()
        ? std::string()
        : fs::path(desc.path).parent_path().string();

    std::unique_ptr<WebModuleView> view = factory(request);
    if (!view || !view->channel()) {
        spdlog::error("Failed to open a webview for web module {}", desc.name);
        return false;
    }

    std::string moduleVersion = desc.rawMetadata.is_object()
        ? desc.rawMetadata.value("version", std::string())
        : std::string();
    if (moduleVersion.empty())
        moduleVersion = kDefaultModuleVersion;

    auto instance = std::make_unique<Instance>();
    instance->name = desc.name;
    instance->pid = view->pid().value_or(kNoPid);
    instance->view = std::move(view);
    instance->onTerminated = std::move(onTerminated);

    // ── the module's own identity ─────────────────────────────────────────
    //
    // PER-IDENTITY, exactly as the Native container does and for the same
    // reason: a web module shares this process's image, so "its" token store
    // has to be asked for or every inbound call authorizes as the HOST. The
    // credential itself is minted and registered by the core's load path —
    // sendToken below adopts it — so this takes only the store-selection half.
    //
    // BUILT BEFORE THE CONNECTION, which is the one ordering constraint in this
    // function: the connection serves the page's outbound traffic, that traffic
    // is routed through this API, and a connection cannot be handed a router
    // that does not exist yet.
    const QString qname = QString::fromStdString(desc.name);
    if (hostApi) {
        // The transport set is a CONSTRUCTOR argument because the provider
        // binds its listeners in its own constructor; a set applied afterwards
        // binds nothing. Empty means the process-global default.
        LogosTransportSet transports;
        if (!desc.transportSetJson.empty())
            transports = logos::transportSetFromJsonString(desc.transportSetJson);

        instance->api = LogosAPI::forIdentity(qname, std::move(transports));
        if (!instance->api) {
            spdlog::error("Failed to isolate the token store for web module {}; "
                          "refusing the load — half an identity is worse than none",
                          desc.name);
            tearDown(*instance);
            return false;
        }
    } else {
        spdlog::debug("Web module {} loaded without a host API; not published", desc.name);
    }

    // ── what the page may reach ───────────────────────────────────────────
    //
    // The module's own API, entered as the module: a page calling a native
    // module is authorized by capability_module exactly as a native caller is,
    // and the container grants nothing of its own. A host that injected routes
    // keeps them; with neither, the router still answers — it tells the page
    // there is no host rather than leaving it to time out.
    WebHostRoutes* routes = hostRoutes;
    if (!routes && instance->api) {
        instance->routes = std::make_unique<LogosApiRoutes>(instance->api, desc.name);
        routes = instance->routes.get();
    }
    instance->router = std::make_unique<WebCallRouter>(desc.name, routes);

    // ── the conversation ──────────────────────────────────────────────────
    //
    // Constructed over THIS view's channel rather than reached through
    // logos::web::setMessageChannelFactory, and that is not a shortcut: the
    // process-wide factory takes no argument, so with two web modules loaded it
    // could not tell which page a connection is for — and "which page" IS a Web
    // module's identity (ADR 0005).
    //
    // ONE CONNECTION, BOTH DIRECTIONS. The router is handed to it rather than
    // laid over the channel as a second connection, because a second one would
    // install the channel's single receiver and take every message from the
    // first.
    auto connection = std::make_unique<logos::web::WebTransportConnection>(
        instance->view->channel(), instance->router.get());
    if (!connection->connectToHost()) {
        spdlog::error("Failed to start the web transport for module {}", desc.name);
        tearDown(*instance);
        return false;
    }
    instance->connection = std::move(connection);

    LogosObject* page = instance->connection->requestObject(qname, 0);
    if (!page) {
        spdlog::error("Failed to obtain a handle on web module {}", desc.name);
        tearDown(*instance);
        return false;
    }
    instance->glue = std::make_unique<WebModuleGlue>(desc.name, moduleVersion, page);

    // ── publish it ────────────────────────────────────────────────────────
    if (instance->api) {
        if (!instance->api->getProvider()->registerObject(qname, instance->glue.get())) {
            spdlog::error("Failed to publish web module {}", desc.name);
            tearDown(*instance);
            return false;
        }
        instance->glue->init(instance->api);
    }

    // Installed BEFORE the instance is published to m_modules so a page that
    // dies during startup is still reported. announceTermination is idempotent
    // per module, so the ordering costs nothing.
    instance->view->setOnDied([this, name = desc.name]() { announceTermination(name); });

    out.name = desc.name;
    out.pid = instance->pid;
    out.endpoint = "web://" + desc.name;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_modules.emplace(desc.name, std::move(instance));
    }
    spdlog::info("Web module started: {} (pid {})", desc.name, out.pid);
    return true;
}

bool WebContainer::sendToken(const std::string& name, const std::string& token)
{
    WebModuleGlue* glue = nullptr;
    bool published = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_modules.find(name);
        if (it == m_modules.end()) return false;
        glue = it->second->glue.get();
        published = it->second->api != nullptr;
    }

    const QString qname = QString::fromStdString(name);
    const QString qtoken = QString::fromStdString(token);

    // ── the host side ─────────────────────────────────────────────────────
    // ADOPT the credential the core minted and registered, into the module's
    // OWN isolated store — the same split the Native container documents. It is
    // what an inbound call to this module authorizes against, so without it
    // every such call is refused with "token not recognized".
    if (published && !TokenManager::adoptCredentialFor(qname, qtoken)) {
        spdlog::error("Web module {} could not adopt its credential; every call "
                      "into it would be refused", name);
        return false;
    }

    // ── the page side ─────────────────────────────────────────────────────
    // Unlike a Bare module there IS a second image here, and it is where the
    // module actually lives: the page has its own ModuleProxy and its own token
    // store, so the credential has to cross the channel or the module never
    // learns what to validate an inbound call against.
    return glue->deliverCredential(qtoken);
}

LoadOutcome WebContainer::awaitLoad(const std::string& name,
                                    std::chrono::milliseconds timeout)
{
    WebModuleGlue* glue = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_modules.find(name);
        if (it == m_modules.end())
            return { LoadVerdict::Failed, "the web module is not running" };
        glue = it->second->glue.get();
    }

    // AT LEAST kPageReadyTimeoutMs, whatever the caller asked for.
    //
    // The caller is ModuleManager, whose one deadline (10 s) is calibrated for
    // a subprocess that dlopens a plugin and prints a line. A page is a browser
    // doing a cold start: it fetches its documents, instantiates the app's
    // bundled Qt-wasm QML runtime -- 26 MB of WebAssembly -- and, for a `ui_qml`
    // `web` variant, a second image beside it, all before its SDK can publish
    // anything. On a machine with no GPU that is tens of seconds and nothing is
    // wrong. Failing at 10 s reports "the page never published a module" about
    // a page that was still starting, and then destroys it.
    //
    // The container is the only thing that knows which kind of load this is, so
    // it raises the floor rather than asking every caller to know. A caller
    // that wants LONGER still gets what it asked for.
    const auto budget = std::max(timeout, std::chrono::milliseconds(kPageReadyTimeoutMs));
    const auto deadline = std::chrono::steady_clock::now() + budget;

    // Polled rather than pushed, because the seam a page reports through IS the
    // contract query: there is no load-status line to wait on, and the page's
    // own SDK publishes its module whenever its script finishes. A page that is
    // already serving answers on the first attempt and this costs one round
    // trip.
    QCoreApplication* app = QCoreApplication::instance();
    const bool onQtMainThread = app && QThread::currentThread() == app->thread();

    // OFF THE QT MAIN THREAD the query is an ordinary blocking call and nothing
    // is being starved, so the loop is the obvious one.
    if (!onQtMainThread) {
        do {
            if (!hasModule(name))
                return { LoadVerdict::Failed, "the page went away while it was loading" };
            if (glue->pageIsServing())
                return { LoadVerdict::Loaded, {} };
            std::this_thread::sleep_for(std::chrono::milliseconds(kPageReadyPollMs));
        } while (std::chrono::steady_clock::now() < deadline);
        return { LoadVerdict::Failed, kNeverPublished };
    }

    // ON THE QT MAIN THREAD THE QUERY GOES TO A WORKER, and this thread pumps.
    //
    // THE PAGE RUNS ON THIS THREAD. In a shell the webview is in this process
    // and its renderer talks to it over the Qt event loop; a page cannot fetch
    // a document, instantiate a WebAssembly image or run a line of JavaScript
    // while this thread is inside a blocking call. And the contract query IS a
    // blocking call — a page that is not serving yet does not answer "no", it
    // does not answer at all, so it costs WebModuleGlue::kCallTimeoutMs of
    // silence. Asking it from here was therefore a wait for something this very
    // wait prevented: the page made no progress, every attempt timed out, and
    // the container reported "it never published a module" about a page that
    // had not been allowed to start.
    //
    // So the query runs on a worker (off the Qt thread it is an ordinary
    // blocking call, which is exactly what WebModuleGlue::awaitPage documents)
    // and this thread does what it must: turn the event loop, which is also
    // what lets a page CALL OUT while it starts up — asking capability_module
    // for what it needs before it publishes — since that dispatch lands here.
    //
    // JOINED, NEVER DETACHED, on every exit path. The probe holds the glue, and
    // the glue is destroyed by the teardown that follows a failed verdict; a
    // detached probe would outlive it by up to a full call timeout and write
    // into it.
    while (std::chrono::steady_clock::now() < deadline) {
        if (!hasModule(name))
            return { LoadVerdict::Failed, "the page went away while it was loading" };

        std::atomic<bool> done{false};
        bool serving = false;
        std::thread probe([glue, &done, &serving] {
            serving = glue->pageIsServing();
            done.store(true, std::memory_order_release);
        });

        while (!done.load(std::memory_order_acquire)) {
            QCoreApplication::processEvents(QEventLoop::AllEvents);
            std::this_thread::sleep_for(std::chrono::milliseconds(kPageReadyPollMs));
        }
        probe.join();

        if (serving)
            return { LoadVerdict::Loaded, {} };
    }

    return { LoadVerdict::Failed, kNeverPublished };
}

// Lift a module out of the map, or nullptr when it is not there.
//
// The single place a module stops being loaded, which is what makes retire()
// EXACTLY ONCE: a second death notification for the same page — a renderer
// crash the backend reports twice, or a kill racing a deliberate unload —
// finds nothing and says nothing.
std::unique_ptr<WebContainer::Instance> WebContainer::takeInstance(const std::string& name)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_modules.find(name);
    if (it == m_modules.end()) return nullptr;
    std::unique_ptr<Instance> instance = std::move(it->second);
    m_modules.erase(it);
    return instance;
}

// The one retirement path. A deliberate unload and a lost page differ only in
// the line they log: both take the instance out of the map, tear it down and
// announce it, and whichever arrives first is the one that runs.
void WebContainer::retire(const std::string& name, Retirement why)
{
    std::unique_ptr<Instance> instance = takeInstance(name);
    if (!instance) return;

    auto onTerminated = instance->onTerminated;

    if (why == Retirement::PageLost) {
        spdlog::warn("Web module {} lost its page", name);
        // END ANY WAIT FIRST. A call that was in flight when the page died has
        // no answer coming, and until it gives up nothing about this module can
        // be destroyed — including the LogosAPI that still holds the name a
        // reload needs.
        WebModuleGlue* glue = instance->glue.get();
        if (glue) glue->abandonPendingCalls();

        // TORN DOWN HERE UNLESS THAT WAIT IS STILL UNWINDING, AND THAT MATTERS.
        // Destroying the module's LogosAPI is what unpublishes its name, and a
        // module loaded again takes the same QtRO address — so an old node
        // still standing when a new one binds is a module that answers nothing.
        // Parking it is the one case that cannot be settled here; see
        // deferTearDown.
        if (glue && glue->isAwaitingPage())
            deferTearDown(std::move(instance));
        else
            tearDown(*instance);
    } else {
        // A deliberate unload, and its caller is entitled to assume the module
        // is gone when unloadModule() returns: the load path checks
        // hasModule() on the way back in, and a reload that found the old
        // page's channel still open would publish over it.
        spdlog::info("Web module stopped: {}", name);
        tearDown(*instance);
    }

    // Announced last, matching the Native and subprocess containers: the
    // callback is the signal that the module is gone, so everything that makes
    // it gone runs first. ModuleManager's expected-exit mark is what stops an
    // orderly unload being reported as a crash.
    if (onTerminated) onTerminated(name);
}

// WHY A DEAD PAGE IS SOMETIMES NOT DESTROYED WHERE IT IS NOTICED.
//
// A page can die in the middle of a call INTO it, and a panic inside a `web`
// variant's Wasm host is precisely that. The stack at that moment, in a daemon,
// is three levels of Qt deep:
//
//   QRemoteObjectSourceIo::onServerRead      <- this module's own QtRO node,
//     WebModuleGlue::callMethod                 dispatching the call
//       QEventLoop::exec                      <- waiting for the page's Result
//         ...the page dies, and the backend reports it here...
//
// Destroying the module's LogosAPI from in there frees the QtRO node whose
// onServerRead is still on the stack; when the wait unwinds, that frame returns
// into freed memory. The host segfaults — in a container whose entire reason to
// exist is that a module's crash must not be the host's.
//
// deleteLater() is NOT the answer, and it is worth saying why, because it is
// the obvious one: Qt holds a deferred deletion only until the event loop that
// posted it returns, and the loop that posted it here is the innermost one —
// the very one the glue is spinning inside that onServerRead frame.
//
// So this parks the module and comes back for it. The instance keeps everything
// it had, including the glue whose wait is the reason to wait; a sweep runs on
// each turn of the event loop until that wait has unwound, and only then
// destroys it. Nothing reaches the module in the meantime: the core marked it
// unloaded when it took the announcement, and a call to an unloaded module is
// refused long before it could get this far.
void WebContainer::deferTearDown(std::unique_ptr<Instance> instance)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_retired.push_back(std::move(instance));
    }
    armSweep();
}

// Come back for the parked modules on the next turn of the event loop.
//
// Two hops, because a page's death is noticed on the backend's own thread: the
// queued call puts us on the Qt main thread, and the timer is created THERE so
// it belongs to that thread whatever thread armed it.
//
// The token is what makes a container safe to destroy with a sweep outstanding:
// it expires with the container, and the timer that finds it expired does
// nothing. The parked modules are torn down by ~WebContainer instead.
void WebContainer::armSweep()
{
    QCoreApplication* app = QCoreApplication::instance();
    if (!app) {
        // No event loop to come back on. There is also no nested Qt event loop
        // to be inside — the wait in awaitPage needs one — so there is nothing
        // to wait for.
        sweepRetired();
        return;
    }

    std::weak_ptr<int> token = m_alive;
    QMetaObject::invokeMethod(app, [this, token]() {
        if (token.expired()) return;
        QTimer::singleShot(0, [this, token]() {
            if (token.expired()) return;
            sweepRetired();
        });
    }, Qt::QueuedConnection);
}

// Destroy every parked module that is no longer being waited on, and come back
// for the rest.
//
// The instances are lifted out UNDER the lock and torn down WITHOUT it, for the
// reason tearDown's own comment gives: closing a channel waits for an in-flight
// delivery, and that delivery may be inside the relay, which re-enters this
// container.
void WebContainer::sweepRetired()
{
    std::vector<std::unique_ptr<Instance>> ready;
    bool stillWaiting = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_retired.begin(); it != m_retired.end();) {
            if ((*it)->glue && (*it)->glue->isAwaitingPage()) {
                stillWaiting = true;
                ++it;
                continue;
            }
            ready.push_back(std::move(*it));
            it = m_retired.erase(it);
        }
    }

    for (std::unique_ptr<Instance>& instance : ready) tearDown(*instance);
    if (stillWaiting) armSweep();
}

// Everything still parked, whatever it is doing. The last word, and the only
// caller is the destructor: the container is going away, so there is no later.
void WebContainer::tearDownAllRetired()
{
    std::vector<std::unique_ptr<Instance>> retired;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        retired.swap(m_retired);
    }
    for (std::unique_ptr<Instance>& instance : retired) tearDown(*instance);
}

void WebContainer::terminate(const std::string& name)
{
    retire(name, Retirement::Unloaded);
    sweepRetired();
}

// Stop routing, unpublish, drop the relay, stop the peer, close the page — in
// that order, because each step removes a way back into the next one's memory.
// Called with m_mutex RELEASED: closing a channel waits for an in-flight
// delivery, and that delivery may be inside the relay, which would re-enter
// this container.
//
// THE ORDER HAS A CYCLE IN IT, and stop() is what cuts it. The peer holds the
// router as its inbound handler and calls onConnectionClosed() as it stops, so
// the router object has to OUTLIVE the connection; the router's work runs
// against the LogosAPI, so it has to STOP before the API is deleted. Stopping
// and destroying are therefore two steps, at the two ends of this function.
void WebContainer::tearDown(Instance& instance)
{
    // Deleting the LogosAPI destroys its provider, and ~LogosAPIProvider
    // unpublishes the registered object from every transport it bound. That is
    // the only unpublish path there is, so the API object's lifetime IS the
    // module's publication — and it goes FIRST, while the glue it published is
    // still alive.
    if (instance.router) instance.router->stop();  // and waits out what is running
    delete instance.api;
    instance.api = nullptr;
    instance.glue.reset();        // drops the subscriptions, then the handle
    instance.connection.reset();  // stops the peer — reaches the router, so it
                                  // is still here to be reached
    instance.router.reset();
    instance.routes.reset();
    instance.view.reset();        // closes the page
}

void WebContainer::terminateAll()
{
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        names.reserve(m_modules.size());
        for (const auto& [name, instance] : m_modules) names.push_back(name);
    }
    for (const auto& name : names) terminate(name);
    sweepRetired();
}

// A page's death, reported from WHEREVER THE BACKEND NOTICED IT.
//
// ON THE QT MAIN THREAD, ALWAYS, and that is not defensive tidiness. A backend
// learns its page is gone on a thread of its own -- the desktop one reads the
// end of a socket on a pump thread -- and everything below this point is
// Qt-affine: tearDown deletes the LogosAPI (a QObject tree that unpublishes
// from QtRO sources), and `onTerminated` runs the core's own load-path
// bookkeeping, which calls modules_state over a transport. Doing either from a
// foreign thread is a segfault in the host, which is the exact opposite of what
// a container that exists FOR crash containment may do.
//
// runOnQtMainThread posts rather than blocks, which is what keeps the pump
// thread joinable while the main thread is tearing this view down -- see the
// declaration for the whole of that argument.
void WebContainer::announceTermination(const std::string& name)
{
    runOnQtMainThread([this, name] { retire(name, Retirement::PageLost); });
}

bool WebContainer::hasModule(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_modules.count(name) > 0;
}

std::optional<int64_t> WebContainer::pid(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_modules.find(name);
    if (it == m_modules.end()) return std::nullopt;
    return it->second->pid;
}

std::unordered_map<std::string, int64_t> WebContainer::getAllPids() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::unordered_map<std::string, int64_t> pids;
    pids.reserve(m_modules.size());
    for (const auto& [name, instance] : m_modules) pids.emplace(name, instance->pid);
    return pids;
}

WebModuleGlue* WebContainer::glueFor(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_modules.find(name);
    return it == m_modules.end() ? nullptr : it->second->glue.get();
}

} // namespace LogosCore
