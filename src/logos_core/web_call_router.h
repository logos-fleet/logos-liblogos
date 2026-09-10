#ifndef WEB_CALL_ROUTER_H
#define WEB_CALL_ROUTER_H

#include <incoming_call_handler.h>

#include <QJsonArray>
#include <QString>
#include <QVariant>
#include <QVariantList>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

class LogosAPI;

namespace LogosCore {

// THE HOST, AS A PAGE MAY REACH IT.
//
// Three verbs, because three is what the web transport's inbound message set
// asks for: call something, ask what it can do, watch what it emits. It is an
// interface rather than a LogosAPI* so the router can be driven with no core
// running — the same reason WebModuleView is an interface rather than a
// QWebEngineView.
class WebHostRoutes {
public:
    virtual ~WebHostRoutes() = default;

    // Two outcomes, and the page has to be able to tell them apart: a method
    // that returned nothing and a call that never happened are both an empty
    // QVariant, and only one of them is a bug in the page.
    struct CallOutcome {
        bool ok = false;
        QVariant value;
        std::string error;
        std::string errorCode;
    };

    virtual CallOutcome call(const std::string& target, const std::string& method,
                             const QVariantList& args) = 0;

    virtual QJsonArray methods(const std::string& target) = 0;

    using EventSink = std::function<void(const QString& event, const QVariantList& data)>;

    // An empty `eventName` subscribes to everything the target emits. MUST NOT
    // BLOCK on the target being loaded: a page subscribes during its own
    // startup, which is exactly when the module it wants is most likely to be
    // still coming up.
    virtual bool subscribe(const std::string& target, const std::string& eventName,
                           EventSink sink) = 0;

    virtual void unsubscribe(const std::string& target, const std::string& eventName) = 0;
};

// The routes a module's OWN LogosAPI opens: the same door a native module
// calls other modules through, entered as this module.
//
// That is what closes the capability loop. LogosAPIClient::invokeRemoteMethod
// runs the requestModule handshake against capability_module on the first call
// to a target and caches the token it mints, so a page calling a native module
// is authorized by exactly the machinery a native caller is — the container
// adds no second policy and grants nothing of its own.
class LogosApiRoutes : public WebHostRoutes {
public:
    // `api` is the per-identity LogosAPI the container built for this module;
    // it is borrowed and must outlive the routes.
    LogosApiRoutes(LogosAPI* api, std::string moduleName);
    ~LogosApiRoutes() override;

    CallOutcome call(const std::string& target, const std::string& method,
                     const QVariantList& args) override;
    QJsonArray methods(const std::string& target) override;
    bool subscribe(const std::string& target, const std::string& eventName,
                   EventSink sink) override;
    void unsubscribe(const std::string& target, const std::string& eventName) override;

private:
    LogosAPI* m_api = nullptr;
    std::string m_moduleName;

    // Subscription ids handed out by LogosAPIClient::onEventWhenAvailable, so
    // an Unsubscribe from the page can cancel the right one. Keyed by the pair
    // the page named, because that pair is all an Unsubscribe carries.
    mutable std::mutex m_mutex;
    std::map<std::pair<std::string, std::string>, quint64> m_subscriptions;
};

// THE PAGE'S OUTBOUND HALF.
//
// WebModuleGlue relays the host's calls INTO a page; this relays the page's
// calls OUT. Together they are the whole of what "a Web module is an ordinary
// participant" means: a page that can only be called is a library, and a module
// that cannot reach capability_module cannot be granted anything.
//
// It rides the SAME channel the glue does — one connection, one page, both
// directions — because a second connection over that channel would install the
// channel's single receiver and silently take every message from the first.
//
// NOTHING IS ROUTED ON THE THREAD IT ARRIVES ON, and that is the load-bearing
// decision here. These callbacks arrive on the CHANNEL's delivery thread, which
// is also the thread carrying the Results the host is blocked waiting for.
// Routing inline would deadlock the obvious way round: the core serves on the
// Qt main thread, an outbound call marshals onto it and blocks, the Result the
// main thread is waiting for is stuck behind that call on the delivery thread,
// and neither ever moves. So the work is POSTED TO THE QT MAIN THREAD — where
// the core already is, so the marshal inside LogosAPI is a no-op — and the
// delivery thread returns at once. A page can then call out while the host is
// calling in, which is exactly the shape `capability_module.requestModule`
// arrives in.
//
// Run inline when there is no QCoreApplication or when this already IS its
// thread — the same rule, for the same reason, as
// WebContainer::announceTermination.
//
// IT AUTHORIZES NOTHING, and needs to authorize nothing: identity here is
// structural (ADR 0005). A message on this channel came from this page, because
// nothing outside the container can write this channel, and the call it becomes
// is made as this module and validated by the target's own ModuleProxy against
// a token capability_module minted. The credential the page presents is
// therefore not re-checked — it is the one this container handed it.
class WebCallRouter : public logos::plain::IncomingCallHandler {
public:
    // `routes` is borrowed and may be null, which is what a container with no
    // host API has: the page is then told the host is unreachable rather than
    // being left to time out.
    WebCallRouter(std::string moduleName, WebHostRoutes* routes);
    ~WebCallRouter() override;

    // STOP ROUTING, BUT STAY ALIVE, and the second half is why this is not just
    // the destructor. Tearing a web module down has a dependency cycle in it:
    // the connection's peer holds this router as its inbound handler and calls
    // onConnectionClosed() while it stops, so the router must outlive the
    // connection — while the router's own work runs against the module's
    // LogosAPI, so it must stop before the API is deleted. stop() is the seam
    // that separates the two: it withdraws what the page subscribed to (while
    // the routes are still there to withdraw from), waits out anything already
    // running, and makes every later message a no-op. Idempotent.
    void stop();

    void onCall(const logos::plain::CallMessage& req, CallReply reply) override;
    void onMethods(const logos::plain::MethodsMessage& req, MethodsReply reply) override;
    void onSubscribe(const logos::plain::SubscribeMessage& req, EventSink sink,
                     const void* connectionId) override;
    void onUnsubscribe(const logos::plain::UnsubscribeMessage& req,
                       const void* connectionId) override;
    void onConnectionClosed(const void* connectionId) override;
    void onToken(const logos::plain::TokenMessage& req) override;

    // How many of the page's subscriptions are live. For tests and
    // diagnostics: a subscription that crossed the channel is otherwise
    // unobservable from outside.
    std::size_t subscriptionCount() const;

private:
    // THE ONE THING THAT MAKES POSTING SAFE. A posted unit of work outlives the
    // post, so by the time it runs the router may be gone — a page whose
    // renderer died is retired from the Qt main thread, which is the very
    // thread the work is queued on. The gate is shared with every piece of work
    // this router hands out: the destructor takes its mutex and clears `alive`,
    // so work either runs with the router guaranteed to be there or does not
    // run at all, and the destructor waits out anything already inside.
    struct Gate {
        std::mutex mutex;
        bool alive = true;
    };

    // Queue `work` for the Qt main thread, or run it here when there is no
    // event loop to queue on. Either way it runs under the gate.
    static void dispatch(const std::shared_ptr<Gate>& gate, std::function<void()> work);

    std::string m_moduleName;
    // Atomic because stop() clears it from the teardown thread while a message
    // arriving on the channel's delivery thread is reading it.
    std::atomic<WebHostRoutes*> m_routes;
    std::shared_ptr<Gate> m_gate;

    // The page's live subscriptions, so onConnectionClosed can withdraw them:
    // a page that navigates away never sends Unsubscribe, and a sink left in
    // the core keeps writing into a channel nobody reads.
    mutable std::mutex m_mutex;
    std::map<std::pair<std::string, std::string>, EventSink> m_sinks;
};

} // namespace LogosCore

#endif // WEB_CALL_ROUTER_H
