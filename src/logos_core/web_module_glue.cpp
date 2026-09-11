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
#include <memory>
#include <mutex>
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

    // WHERE THE REPLY LANDS, AND WHY IT IS NOT THIS STACK FRAME.
    //
    // The reply is delivered asynchronously, on the transport's own thread, and
    // nothing guarantees it arrives before this function returns. The wait ends
    // at its deadline whether or not an answer came — and, since a page that
    // dies is now told to give up (abandonPendingCalls), usually the moment the
    // page goes away. A callback holding references INTO this frame would then
    // write into a dead stack and post to a destroyed QEventLoop, which is a
    // segfault in the host with the page's death nowhere on the stack.
    //
    // So the callback and this frame share a heap record instead, and the loop
    // pointer in it is cleared under the lock before the loop goes out of
    // scope. A reply that arrives before the clear posts to a loop that is
    // still alive (Qt discards posted events for an object destroyed after);
    // one that arrives after finds nullptr and does nothing.
    struct Waiter {
        std::mutex mu;
        QEventLoop* loop = nullptr;
        QVariant result;
        bool done = false;
    };
    auto waiter = std::make_shared<Waiter>();

    QEventLoop loop;
    {
        std::lock_guard<std::mutex> lock(waiter->mu);
        waiter->loop = &loop;
    }

    // FROM HERE TO THE RETURN this glue is inside a nested event loop: the
    // container must not destroy the module while that is true, and it needs a
    // way to end the wait when the page dies. See isAwaitingPage() and
    // abandonPendingCalls().
    struct Awaiting {
        WebModuleGlue& glue;
        QEventLoop* loop;
        Awaiting(WebModuleGlue& g, QEventLoop* l) : glue(g), loop(l)
        {
            ++glue.m_awaiting;
            std::lock_guard<std::mutex> lock(glue.m_waitMutex);
            glue.m_waits.push_back(loop);
        }
        ~Awaiting()
        {
            {
                std::lock_guard<std::mutex> lock(glue.m_waitMutex);
                for (auto it = glue.m_waits.begin(); it != glue.m_waits.end(); ++it) {
                    if (*it == loop) { glue.m_waits.erase(it); break; }
                }
            }
            --glue.m_awaiting;
        }
    } awaiting(*this, &loop);

    m_page->callMethodAsync(authToken(), methodName, args, kCallTimeoutMs,
        [waiter](QVariant value) {
            std::lock_guard<std::mutex> lock(waiter->mu);
            waiter->result = std::move(value);
            waiter->done = true;
            // Queued, because this lands on the transport's delivery thread.
            // A quit posted before exec() begins is NOT lost: it is an event on
            // that thread's queue and the loop below will process it.
            if (waiter->loop) {
                QEventLoop* waking = waiter->loop;
                QMetaObject::invokeMethod(waking, [waking] { waking->quit(); },
                                          Qt::QueuedConnection);
            }
        });

    bool answered = false;
    {
        std::lock_guard<std::mutex> lock(waiter->mu);
        answered = waiter->done;
    }
    if (!answered) {
        // The page's own deadline is the async call's; this one only stops the
        // loop from outliving it if a reply is dropped outright.
        QTimer::singleShot(kCallTimeoutMs + 1000, &loop, [&loop] { loop.quit(); });
        loop.exec();
    }

    std::lock_guard<std::mutex> lock(waiter->mu);
    waiter->loop = nullptr;
    return waiter->result;
}

void WebModuleGlue::abandonPendingCalls()
{
    std::vector<QPointer<QEventLoop>> waits;
    {
        std::lock_guard<std::mutex> lock(m_waitMutex);
        waits = m_waits;
    }
    // Queued and addressed to the loop itself: the caller is the thread that
    // noticed the page die, which is not the thread sitting in exec(), and a
    // loop that has already ended on its own is a receiver Qt discards the
    // event for.
    for (const QPointer<QEventLoop>& loop : waits) {
        if (!loop) continue;
        QMetaObject::invokeMethod(loop.data(), [l = loop]() { if (l) l->quit(); },
                                  Qt::QueuedConnection);
    }
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
