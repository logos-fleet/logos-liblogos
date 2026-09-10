#include "web_container.h"
#include "web_module_glue.h"

#include <logos_api.h>
#include <logos_api_provider.h>
#include <token_manager.h>
#include <logos_object.h>
#include <logos_transport_config_json.h>

#include <web_transport_connection.h>

#include <QCoreApplication>
#include <QMetaObject>
#include <QString>
#include <QThread>
#include <spdlog/spdlog.h>

#include <filesystem>
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

// How often awaitLoad re-asks a page that is not serving yet.
constexpr int kPageReadyPollMs = 50;

} // namespace

WebContainer::WebContainer() = default;

WebContainer::~WebContainer()
{
    terminateAll();
}

void WebContainer::setHostApi(LogosAPI* hostApi)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hostApi = hostApi;
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

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_modules.count(desc.name)) {
            spdlog::warn("Web module already running: {}", desc.name);
            return false;
        }
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

    // ── the conversation ──────────────────────────────────────────────────
    //
    // Constructed over THIS view's channel rather than reached through
    // logos::web::setMessageChannelFactory, and that is not a shortcut: the
    // process-wide factory takes no argument, so with two web modules loaded it
    // could not tell which page a connection is for — and "which page" IS a Web
    // module's identity (ADR 0005).
    auto connection = std::make_unique<logos::web::WebTransportConnection>(view->channel());
    if (!connection->connectToHost()) {
        spdlog::error("Failed to start the web transport for module {}", desc.name);
        return false;
    }

    LogosObject* page = connection->requestObject(QString::fromStdString(desc.name), 0);
    if (!page) {
        spdlog::error("Failed to obtain a handle on web module {}", desc.name);
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
    instance->connection = std::move(connection);
    instance->glue = std::make_unique<WebModuleGlue>(desc.name, moduleVersion, page);
    instance->onTerminated = std::move(onTerminated);

    // ── publish it ────────────────────────────────────────────────────────
    //
    // PER-IDENTITY, exactly as the Native container does and for the same
    // reason: a web module shares this process's image, so "its" token store
    // has to be asked for or every inbound call authorizes as the HOST. The
    // credential itself is minted and registered by the core's load path —
    // sendToken below adopts it — so this takes only the store-selection half.
    if (m_hostApi) {
        const QString qname = QString::fromStdString(desc.name);

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
        if (!instance->api->getProvider()->registerObject(qname, instance->glue.get())) {
            spdlog::error("Failed to publish web module {}", desc.name);
            tearDown(*instance);
            return false;
        }
        instance->glue->init(instance->api);
    } else {
        spdlog::debug("Web module {} loaded without a host API; not published", desc.name);
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

    const auto budget = timeout.count() > 0
        ? timeout
        : std::chrono::milliseconds(kPageReadyTimeoutMs);
    const auto deadline = std::chrono::steady_clock::now() + budget;

    // Polled rather than pushed, because the seam a page reports through IS the
    // contract query: there is no load-status line to wait on, and the page's
    // own SDK publishes its module whenever its script finishes. A page that is
    // already serving answers on the first attempt and this costs one round
    // trip.
    do {
        if (!hasModule(name))
            return { LoadVerdict::Failed, "the page went away while it was loading" };
        if (glue->pageIsServing())
            return { LoadVerdict::Loaded, {} };
        std::this_thread::sleep_for(std::chrono::milliseconds(kPageReadyPollMs));
    } while (std::chrono::steady_clock::now() < deadline);

    return { LoadVerdict::Failed,
             "the page never published a module: it answered the contract query "
             "with nothing before the deadline" };
}

// Lift a module out of the map, or nullptr when it is not there.
//
// The single place a module stops being loaded, which is what makes both
// teardown paths EXACTLY ONCE: a second death notification for the same page —
// a renderer crash the backend reports twice, or a kill racing a deliberate
// unload — finds nothing and says nothing.
std::unique_ptr<WebContainer::Instance> WebContainer::takeInstance(const std::string& name)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_modules.find(name);
    if (it == m_modules.end()) return nullptr;
    std::unique_ptr<Instance> instance = std::move(it->second);
    m_modules.erase(it);
    return instance;
}

void WebContainer::terminate(const std::string& name)
{
    std::unique_ptr<Instance> instance = takeInstance(name);
    if (!instance) return;

    auto onTerminated = instance->onTerminated;
    tearDown(*instance);
    spdlog::info("Web module stopped: {}", name);

    // Announced last, matching the Native and subprocess containers: the
    // callback is the signal that the module is gone, so everything that makes
    // it gone runs first. ModuleManager's expected-exit mark is what stops this
    // being reported as a crash.
    if (onTerminated) onTerminated(name);
}

// Unpublish, drop the relay, stop the peer, close the page — in that order,
// because each step removes a way back into the next one's memory. Called with
// m_mutex RELEASED: closing a channel waits for an in-flight delivery, and that
// delivery may be inside the relay, which would re-enter this container.
void WebContainer::tearDown(Instance& instance)
{
    // Deleting the LogosAPI destroys its provider, and ~LogosAPIProvider
    // unpublishes the registered object from every transport it bound. That is
    // the only unpublish path there is, so the API object's lifetime IS the
    // module's publication — and it goes FIRST, while the glue it published is
    // still alive.
    delete instance.api;
    instance.api = nullptr;
    instance.glue.reset();        // drops the subscriptions, then the handle
    instance.connection.reset();  // stops the peer
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
// POSTED, not blocking. A blocking hand-off would deadlock the pair: the main
// thread's teardown closes the view, which joins the pump thread, which would
// be sitting here waiting for the main thread. Posting lets the pump thread
// finish and be joined.
//
// Run inline when there is no QCoreApplication or when this IS its thread --
// the second case is every gtest here, which drives the whole container from
// the test thread and pumps no event loop.
void WebContainer::announceTermination(const std::string& name)
{
    QCoreApplication* app = QCoreApplication::instance();
    if (app && QThread::currentThread() != app->thread()) {
        // `app` as the context object, so a shutdown that outruns this event
        // drops it rather than running teardown against a half-gone process.
        QMetaObject::invokeMethod(app, [this, name] { announceTerminationHere(name); },
                                  Qt::QueuedConnection);
        return;
    }
    announceTerminationHere(name);
}

void WebContainer::announceTerminationHere(const std::string& name)
{
    std::unique_ptr<Instance> instance = takeInstance(name);
    if (!instance) return;

    auto onTerminated = instance->onTerminated;
    tearDown(*instance);
    spdlog::warn("Web module {} lost its page", name);
    if (onTerminated) onTerminated(name);
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
