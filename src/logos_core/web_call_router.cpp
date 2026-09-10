#include "web_call_router.h"
#include "web_qt_dispatch.h"

#include <logos_api.h>
#include <logos_api_client.h>
#include <logos_object.h>

#include <qvariant_rpc_value.h>

#include <spdlog/spdlog.h>

#include <utility>

namespace LogosCore {

using logos::plain::CallMessage;
using logos::plain::EventMessage;
using logos::plain::MethodsMessage;
using logos::plain::MethodsResultMessage;
using logos::plain::ResultMessage;
using logos::plain::SubscribeMessage;
using logos::plain::TokenMessage;
using logos::plain::UnsubscribeMessage;

// ── the routes ──────────────────────────────────────────────────────────────

LogosApiRoutes::LogosApiRoutes(LogosAPI* api, std::string moduleName)
    : m_api(api)
    , m_moduleName(std::move(moduleName))
{
}

LogosApiRoutes::~LogosApiRoutes()
{
    std::map<std::pair<std::string, std::string>, quint64> subs;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        subs.swap(m_subscriptions);
    }
    if (!m_api) return;
    for (const auto& [key, id] : subs) {
        if (auto* client = m_api->getClient(QString::fromStdString(key.first)))
            client->cancelEventSubscription(id);
    }
}

WebHostRoutes::CallOutcome LogosApiRoutes::call(const std::string& target,
                                                const std::string& method,
                                                const QVariantList& args)
{
    CallOutcome outcome;
    if (!m_api) {
        outcome.error = "this process has no host API: the web module was loaded "
                        "without one and can call nothing";
        outcome.errorCode = "MODULE_NOT_LOADED";
        return outcome;
    }

    LogosAPIClient* client = m_api->getClient(QString::fromStdString(target));
    if (!client) {
        outcome.error = "no client for " + target;
        outcome.errorCode = "MODULE_NOT_LOADED";
        return outcome;
    }

    // THE CAPABILITY HANDSHAKE IS IN HERE, and that is the whole point of
    // routing through the client rather than reaching into the registry: on the
    // first call to a target invokeRemoteMethod dials capability_module for a
    // token, caches it, and presents it. A page therefore gets exactly the
    // authority a native caller gets, granted by the same module, with nothing
    // the container decided on its own.
    logos::CallError err;
    const QVariant value = client->invokeRemoteMethod(
        QString::fromStdString(target), QString::fromStdString(method), args,
        Timeout(), &err);

    if (!err.code.empty()) {
        outcome.error = err.message.empty()
            ? ("call to " + target + "." + method + " failed: " + err.code)
            : err.message;
        outcome.errorCode = err.code == "object_unavailable" ? "MODULE_NOT_LOADED"
                                                             : "METHOD_FAILED";
        return outcome;
    }

    outcome.ok = true;
    outcome.value = value;
    return outcome;
}

QJsonArray LogosApiRoutes::methods(const std::string& target)
{
    if (!m_api) return QJsonArray();
    LogosAPIClient* client = m_api->getClient(QString::fromStdString(target));
    if (!client) return QJsonArray();
    LogosObject* obj = client->requestObject(QString::fromStdString(target));
    if (!obj) return QJsonArray();
    const QJsonArray methods = obj->getMethods();
    obj->release();
    return methods;
}

bool LogosApiRoutes::subscribe(const std::string& target,
                               const std::string& eventName,
                               EventSink sink)
{
    if (!m_api) return false;
    LogosAPIClient* client = m_api->getClient(QString::fromStdString(target));
    if (!client) return false;

    // onEventWhenAvailable, NOT requestObject + onEvent: a page subscribes
    // during its own startup, which is exactly when the module it wants is most
    // likely to be still coming up, and the acquiring form would either block
    // this thread on the transport's acquire timeout or lose the subscription
    // outright.
    // The sink is handed over as-is: WebHostRoutes::EventSink already has
    // onEventWhenAvailable's callback signature, so wrapping it would only add
    // an indirection.
    const quint64 id = client->onEventWhenAvailable(
        QString::fromStdString(target), QString::fromStdString(eventName),
        std::move(sink));
    if (id == 0) return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_subscriptions[{target, eventName}] = id;
    return true;
}

void LogosApiRoutes::unsubscribe(const std::string& target,
                                 const std::string& eventName)
{
    quint64 id = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_subscriptions.find({target, eventName});
        if (it == m_subscriptions.end()) return;
        id = it->second;
        m_subscriptions.erase(it);
    }
    if (!m_api) return;
    if (auto* client = m_api->getClient(QString::fromStdString(target)))
        client->cancelEventSubscription(id);
}

// ── the router ──────────────────────────────────────────────────────────────

WebCallRouter::WebCallRouter(std::string moduleName, WebHostRoutes* routes)
    : m_moduleName(std::move(moduleName))
    , m_routes(routes)
    , m_gate(std::make_shared<Gate>())
{
}

WebCallRouter::~WebCallRouter()
{
    stop();
}

void WebCallRouter::stop()
{
    std::map<std::pair<std::string, std::string>, EventSink> sinks;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        sinks.swap(m_sinks);
    }
    // WITHDRAWN HERE, not left to whoever owns the routes. A page's death is
    // the common case and it never sends Unsubscribe, so a subscription this
    // router armed on the page's behalf outlives the page unless this takes it
    // back — and every later emission then writes into a channel that is gone.
    // Directly rather than dispatched: a posted withdrawal would run after the
    // gate closed and be dropped, which is the leak it was meant to prevent.
    if (WebHostRoutes* routes = m_routes.load()) {
        for (const auto& [key, sink] : sinks)
            routes->unsubscribe(key.first, key.second);
    }
    // Close the gate LAST and under its own mutex: work already running holds
    // that mutex, so this waits it out, and everything queued behind finds
    // `alive` false and does nothing. The routes are dropped under the same
    // lock, so nothing can reach them after their owner tears them down.
    std::lock_guard<std::mutex> closing(m_gate->mutex);
    m_gate->alive = false;
    m_routes.store(nullptr);
}

void WebCallRouter::dispatch(const std::shared_ptr<Gate>& gate,
                             std::function<void()> work)
{
    runOnQtMainThread([gate, work = std::move(work)] {
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (!gate->alive) return;
        work();
    });
}

void WebCallRouter::onCall(const CallMessage& req, CallReply reply)
{
    const std::string target = req.object;
    const std::string method = req.method;
    const QVariantList args = logos::plain::rpcListToQVariantList(req.args);
    const uint64_t id = req.id;
    WebHostRoutes* routes = m_routes.load();
    const std::string moduleName = m_moduleName;

    dispatch(m_gate, [routes, target, method, args, id, reply, moduleName] {
        ResultMessage res;
        res.id = id;
        if (!routes) {
            res.ok = false;
            res.err = "web module " + moduleName + " has no host to call";
            res.errCode = "MODULE_NOT_LOADED";
            reply(std::move(res));
            return;
        }
        const WebHostRoutes::CallOutcome outcome = routes->call(target, method, args);
        res.ok = outcome.ok;
        if (outcome.ok) {
            res.value = logos::plain::qvariantToRpcValue(outcome.value);
        } else {
            res.err = outcome.error;
            res.errCode = outcome.errorCode;
        }
        reply(std::move(res));
    });
}

void WebCallRouter::onMethods(const MethodsMessage& req, MethodsReply reply)
{
    const std::string target = req.object;
    const uint64_t id = req.id;
    WebHostRoutes* routes = m_routes.load();

    dispatch(m_gate, [routes, target, id, reply] {
        MethodsResultMessage res;
        res.id = id;
        if (!routes) {
            res.ok = false;
            res.err = "no host to introspect";
            reply(std::move(res));
            return;
        }
        res.ok = true;
        res.methods = logos::plain::methodsFromJsonArray(routes->methods(target));
        reply(std::move(res));
    });
}

void WebCallRouter::onSubscribe(const SubscribeMessage& req, EventSink sink,
                                const void* /*connectionId*/)
{
    const std::string target = req.object;
    const std::string eventName = req.eventName;
    WebHostRoutes* routes = m_routes.load();
    const std::string moduleName = m_moduleName;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Recorded before the core is asked, and a pair the page already has is
        // NOT subscribed twice: one sink per (object, event, connection) is the
        // transport's contract, and a repeat Subscribe is an idempotent
        // re-assertion rather than a second subscriber.
        const bool known = m_sinks.count({target, eventName}) > 0;
        m_sinks[{target, eventName}] = std::move(sink);
        if (known) return;
    }

    auto gate = m_gate;
    dispatch(gate, [this, gate, routes, target, eventName, moduleName] {
        if (!routes) {
            spdlog::debug("Web module {} subscribed to {}/{} with no host to "
                          "subscribe on", moduleName, target, eventName);
            return;
        }
        // The sink is looked up again AT DELIVERY TIME rather than captured
        // once, and under the same gate: a page that went away stops being
        // written to because the router's teardown empties the table, and an
        // emission arriving after the router itself is gone finds a closed
        // gate instead of a dangling `this`.
        const bool armed = routes->subscribe(target, eventName,
            [this, gate, target, eventName](const QString& name, const QVariantList& data) {
                std::lock_guard<std::mutex> live(gate->mutex);
                if (!gate->alive) return;
                EventSink sink;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    auto it = m_sinks.find({target, eventName});
                    if (it == m_sinks.end()) return;
                    sink = it->second;
                }
                EventMessage msg;
                msg.object = target;
                msg.eventName = name.toStdString();
                msg.data = logos::plain::qvariantListToRpcList(data);
                sink(std::move(msg));
            });
        if (!armed)
            spdlog::warn("Web module {}: could not subscribe to {}/{}", moduleName,
                         target, eventName);
    });
}

void WebCallRouter::onUnsubscribe(const UnsubscribeMessage& req,
                                  const void* /*connectionId*/)
{
    const std::string target = req.object;
    const std::string eventName = req.eventName;
    WebHostRoutes* routes = m_routes.load();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_sinks.erase({target, eventName}) == 0) return;
    }
    dispatch(m_gate, [routes, target, eventName] {
        if (routes) routes->unsubscribe(target, eventName);
    });
}

void WebCallRouter::onConnectionClosed(const void* /*connectionId*/)
{
    std::map<std::pair<std::string, std::string>, EventSink> sinks;
    WebHostRoutes* routes = m_routes.load();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        sinks.swap(m_sinks);
    }
    // A page that navigates away never sends Unsubscribe, so without this every
    // emission from every module it watched keeps writing into a channel that
    // is gone.
    for (const auto& [key, sink] : sinks) {
        dispatch(m_gate, [routes, key] {
            if (routes) routes->unsubscribe(key.first, key.second);
        });
    }
}

void WebCallRouter::onToken(const TokenMessage& req)
{
    // A page pushing a token AT the host is not a thing the core has a door
    // for: informModuleToken is how a capability grant reaches a MODULE, and
    // the module here is the page. Logged rather than silently dropped, because
    // a page that sends one is doing something its SDK cannot have meant.
    spdlog::debug("Web module {}: ignoring a token push for {} from the page",
                  m_moduleName, req.moduleName);
}

std::size_t WebCallRouter::subscriptionCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sinks.size();
}

} // namespace LogosCore
