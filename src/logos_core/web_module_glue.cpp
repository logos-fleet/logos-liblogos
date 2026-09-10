#include "web_module_glue.h"

#include <logos_object.h>
#include <token_manager.h>

#include <spdlog/spdlog.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <utility>

namespace LogosCore {

WebModuleGlue::WebModuleGlue(std::string moduleName,
                             std::string moduleVersion,
                             LogosObject* page)
    : m_name(std::move(moduleName))
    , m_version(std::move(moduleVersion))
    , m_page(page)
{
}

WebModuleGlue::~WebModuleGlue()
{
    if (!m_page) return;
    // Order matters and is the one thing a relay can get fatally wrong: drop
    // the subscriptions FIRST, so no event delivery can reach a listener whose
    // owner is being destroyed, and only then release the handle.
    m_page->disconnectEvents();
    {
        std::lock_guard<std::mutex> g(m_eventMutex);
        m_eventCallback = nullptr;
    }
    m_page->release();
    m_page = nullptr;
}

QString WebModuleGlue::authToken() const
{
    std::lock_guard<std::mutex> g(m_credentialMutex);
    if (!m_credential.isEmpty()) return m_credential;
    // Nothing has been delivered yet. Fall back to the core's own copy so a
    // host that mints outside this container is not left presenting nothing;
    // empty is the honest answer when neither exists, and the page refuses it.
    return QString::fromStdString(TokenManager::instance().getToken(m_name));
}

// WAIT FOR THE PAGE WITHOUT STARVING THE THREAD WE ARE ON.
//
// A blocking wait here is a deadlock the moment a page's handler CALLS BACK.
// The core serves on the Qt main thread, so this relay usually runs on it; the
// page's own calls are routed on that same thread (WebCallRouter::dispatch,
// where posting to it is what keeps the channel's delivery thread free). A
// plain blocking wait therefore holds the one thread that could answer the page,
// and both sides sit out their deadlines:
//
//   main thread   -> js_counter.callNative(...)      waits for the page
//   the page      -> core_service.getStatus(...)     waits for the main thread
//
// So on that thread the wait PUMPS. This is not a new idea in this codebase: it
// is exactly what Qt Remote Objects' own blocking call does for every native
// module, which is why a subprocess module has always been able to call back
// while the core waits on it. Off the Qt main thread nothing is being starved
// and the ordinary blocking call is used.
QVariant WebModuleGlue::awaitPage(const QString& methodName, const QVariantList& args)
{
    QCoreApplication* app = QCoreApplication::instance();
    if (!app || QThread::currentThread() != app->thread())
        return m_page->callMethod(authToken(), methodName, args, kCallTimeoutMs);

    QVariant result;
    std::atomic<bool> done{false};
    QEventLoop loop;
    QEventLoop* waiting = &loop;

    m_page->callMethodAsync(authToken(), methodName, args, kCallTimeoutMs,
        [&result, &done, waiting](QVariant value) {
            result = std::move(value);
            done.store(true);
            // Queued, because this lands on the transport's delivery thread.
            // A quit posted before exec() begins is NOT lost: it is an event on
            // this thread's queue and the loop below will process it.
            QMetaObject::invokeMethod(waiting, [waiting] { waiting->quit(); },
                                      Qt::QueuedConnection);
        });

    if (!done.load()) {
        // The page's own deadline is the async call's; this one only stops the
        // loop from outliving it if a reply is dropped outright.
        QTimer::singleShot(kCallTimeoutMs + 1000, &loop, [&loop] { loop.quit(); });
        loop.exec();
    }
    return result;
}

QVariant WebModuleGlue::callMethod(const QString& methodName, const QVariantList& args)
{
    if (!m_page) return QVariant();
    return awaitPage(methodName, args);
}

QJsonArray WebModuleGlue::getMethods()
{
    if (!m_page) return QJsonArray();
    // A ROUND TRIP, not a lookup. Over a transport, introspection is a call
    // like any other — which is also why it is the honest load verdict: a page
    // that has not run its module's registration yet answers with nothing.
    return m_page->getMethods();
}

bool WebModuleGlue::informModuleToken(const QString& moduleName, const QString& token)
{
    // BOTH HALVES, and neither is optional. The base class records the token in
    // this image so the host side of the pair is complete; the relay carries it
    // to the page, which is where the module actually is and where its own
    // ModuleProxy will validate against it.
    const bool localOk = LogosProviderBase::informModuleToken(moduleName, token);
    if (!m_page) return localOk;
    const bool relayed =
        m_page->informModuleToken(authToken(), moduleName, token, kCallTimeoutMs);
    if (!relayed)
        spdlog::warn("Web module {}: the page refused the token for {}", m_name,
                     moduleName.toStdString());
    return localOk && relayed;
}

void WebModuleGlue::setEventListener(EventCallback callback)
{
    {
        std::lock_guard<std::mutex> g(m_eventMutex);
        m_eventCallback = callback;
    }
    LogosProviderBase::setEventListener(std::move(callback));

    if (!m_page || m_subscribed) return;

    // ONE WILDCARD SUBSCRIPTION, not one per event name. The provider above
    // this object is handed every event the module emits and fans them out
    // itself; subscribing per name would need the contract first, which is a
    // second round trip against a page that may not be serving yet.
    m_subscribed = true;
    m_page->onEvent(QString(), [this](const QString& name, const QVariantList& data) {
        if (EventCallback cb = eventListener()) cb(name, data);
    });
}

WebModuleGlue::EventCallback WebModuleGlue::eventListener() const
{
    std::lock_guard<std::mutex> g(m_eventMutex);
    return m_eventCallback;
}

bool WebModuleGlue::deliverCredential(const QString& token)
{
    if (!m_page) return false;
    {
        std::lock_guard<std::mutex> g(m_credentialMutex);
        m_credential = token;
    }
    return m_page->informModuleToken(QString(), QString::fromStdString(m_name),
                                     token, kCallTimeoutMs);
}

bool WebModuleGlue::pageIsServing()
{
    return !getMethods().isEmpty();
}

} // namespace LogosCore
