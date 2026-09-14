#include "web_call_router.h"
#include "web_qt_dispatch.h"

#include <logos_api.h>
#include <logos_api_client.h>
#include <logos_object.h>

#include <qvariant_rpc_value.h>

#include <spdlog/spdlog.h>

#include <thread>
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

namespace {

// The broker, by the one name every core registers it under.
const char* const kCapabilityModule = "capability_module";

// The protocol's word for "the target would not take our token". It is what a
// consent refusal looks like from the outside, because an undecided or denied
// pair is minted NO token at all.
const char* const kUnauthorized = "unauthorized";

} // namespace

WebHostRoutes::CallOutcome refusedCallOutcome(const std::string& target,
                                              const std::string& method,
                                              const std::string& errorCode,
                                              const std::string& errorMessage,
                                              const QVariantMap& consentStatus)
{
    WebHostRoutes::CallOutcome outcome;
    outcome.error = errorMessage.empty()
        ? ("call to " + target + "." + method + " failed: " + errorCode)
        : errorMessage;
    outcome.errorCode = errorCode == "object_unavailable" ? "MODULE_NOT_LOADED"
                                                          : "METHOD_FAILED";

    if (errorCode != kUnauthorized)
        return outcome;

    // THE BROKER'S OWN WORDS, AND ONLY ITS OWN. An empty map is a question that
    // was not asked or not answered, and a `granted` / `not-required` pair that
    // still came back unauthorized is a token fault wearing a consent refusal's
    // clothes -- relabelling either would send a developer to a dialog that is
    // not the problem.
    const std::string state =
        consentStatus.value(QStringLiteral("state")).toString().toStdString();
    const QString reason = consentStatus.value(QStringLiteral("reason")).toString();
    if (state != "pending" && state != "unknown" && state != "denied")
        return outcome;

    outcome.errorCode = state == "denied" ? "CONSENT_DENIED" : "CONSENT_REQUIRED";
    if (!reason.isEmpty())
        outcome.error = reason.toStdString();
    return outcome;
}


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
        // A SECOND QUESTION, ONLY ON A REFUSAL THE GATE COULD HAVE CAUSED. The
        // consent verdict costs a round trip, so it is bought exactly when the
        // answer could change the sentence the page reads.
        return refusedCallOutcome(target, method, err.code, err.message,
                                  err.code == kUnauthorized ? consentStatusFor(target)
                                                            : QVariantMap{});
    }

    outcome.ok = true;
    outcome.value = value;
    return outcome;
}

QVariantMap LogosApiRoutes::consentStatusFor(const std::string& target)
{
    // Never through the door that was just shut: a refusal by capability_module
    // ITSELF cannot be explained by asking capability_module.
    if (!m_api || target == kCapabilityModule)
        return {};
    LogosAPIClient* broker = m_api->getClient(QStringLiteral("capability_module"));
    if (!broker)
        return {};

    // The module's own credential for the broker is seeded at load, so this is
    // an ordinary call and not a second handshake. `consentStatus` is a plain
    // query -- it decides nothing and records nothing -- so a module asking
    // about its own pair grants it nothing it did not already have.
    logos::CallError err;
    const QVariant answer = broker->invokeRemoteMethod(
        QStringLiteral("capability_module"), QStringLiteral("consentStatus"),
        QVariantList{ QString::fromStdString(m_moduleName),
                      QString::fromStdString(target) },
        Timeout(), &err);
    if (!err.code.empty()) {
        // A broker that will not answer is not a reason to invent a verdict:
        // the caller keeps the protocol's own refusal.
        spdlog::debug("consentStatus for {} -> {} was refused: {}", m_moduleName, target,
                      err.code);
        return {};
    }
    return answer.toMap();
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
    // Close the gate LAST: everything queued behind finds `alive` false and does
    // nothing, and anything ALREADY inside is waited out here. Only then are
    // the routes dropped, so no work can still be holding the pointer it read
    // on its way in once their owner tears them down.
    //
    // THE WAIT IS WHAT PROVIDES THAT, not the lock. Work runs with the mutex
    // free (see Gate), so holding it here proves nothing about who is inside;
    // `running` does. The upside is that a teardown racing a call blocks here
    // instead of wedging the call.
    std::unique_lock<std::mutex> closing(m_gate->mutex);
    m_gate->alive = false;
    m_gate->idle.wait(closing, [this] { return m_gate->noOtherThreadInside(); });
    m_routes.store(nullptr);
}

void WebCallRouter::runUnderGate(const std::shared_ptr<Gate>& gate,
                                 const std::function<void()>& work)
{
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (!gate->alive) return;
        gate->running.insert(std::this_thread::get_id());
    }
    // THE MUTEX IS NOT HELD ACROSS `work`. It calls into the module's LogosAPI,
    // which for an in-process target spins a nested event loop, which runs this
    // router's next message on this very thread — so a held mutex here is a
    // self-deadlock rather than a slow path (logos-workspace#113).
    //
    // RAII so a throwing route still leaves the count right: a leaked entry
    // would hang every later stop() instead of the current call.
    struct Leave {
        const std::shared_ptr<Gate>& gate;
        ~Leave()
        {
            std::lock_guard<std::mutex> lock(gate->mutex);
            auto it = gate->running.find(std::this_thread::get_id());
            if (it != gate->running.end()) gate->running.erase(it);
            gate->idle.notify_all();
        }
    } leave{gate};

    work();
}

void WebCallRouter::dispatch(const std::shared_ptr<Gate>& gate,
                             std::function<void()> work)
{
    runOnQtMainThread([gate, work = std::move(work)] {
        runUnderGate(gate, work);
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
                // Under the gate, NOT under its mutex: this arrives on the
                // emitting module's own thread, and holding the mutex across
                // the write put every native module's events in a queue behind
                // whatever call the page happened to have in flight.
                runUnderGate(gate, [&] {
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
