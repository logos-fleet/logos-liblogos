#include "bare_module_glue.h"

#include <logos_async_dispatch.h>
#include <logos_json_convert.h>
#include <logos_types.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaObject>
#include <QThread>
#include <QVariantMap>
#include <spdlog/spdlog.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <nlohmann/json.hpp>

namespace LogosCore {

namespace {

// The two return shapes the generated Qt glue hardcodes as QSets, spelled as
// they appear in the published contract. `returnType` carries the LIDL text of
// the declared return (lidlTypeToPublishedName), so `void` and `result` are the
// contract's own words rather than a C++ or Qt spelling.
constexpr const char* kVoidReturn   = "void";
constexpr const char* kResultReturn = "result";

// What the destructor waits for a worker it should never have to wait for: the
// container stops the thread explicitly and refuses to destroy the glue if that
// fails, so this is the belt to that braces.
constexpr int kDestructorGraceMs = 2000;

// Free a char* the module heap-allocated, through the module's OWN free — never
// this image's. Two images, two allocators; logos_module_impl.h makes the
// module export the deallocator precisely so the domains never mix.
struct ModuleString {
    ModuleString(const BareModuleAbi& abi, char* s) : m_abi(abi), m_s(s) {}
    ~ModuleString() { if (m_s && m_abi.stringFree) m_abi.stringFree(m_s); }
    ModuleString(const ModuleString&) = delete;
    ModuleString& operator=(const ModuleString&) = delete;

    explicit operator bool() const { return m_s != nullptr; }
    const char* get() const { return m_s; }

    const BareModuleAbi& m_abi;
    char* m_s;
};

} // namespace

BareModuleGlue::BareModuleGlue(std::string moduleName, std::string moduleVersion,
                               const BareModuleAbi& abi)
    : m_name(std::move(moduleName))
    , m_version(std::move(moduleVersion))
    , m_abi(abi)
{
    readContract();
    // Installed here rather than in setEventListener: a module may emit from a
    // context-ready hook that fires the moment the context lands, which is
    // before any consumer has subscribed. onModuleEvent drops what nobody is
    // listening for; not being installed at all would drop it silently one
    // layer deeper, where there is nothing to log.
    if (m_abi.setEmitCallback)
        m_abi.setEmitCallback(&BareModuleGlue::emitTrampoline, this);
}

BareModuleGlue::~BareModuleGlue()
{
    // The worker first: it is the one thing that can still be INSIDE the module
    // when this runs, and it reaches back through m_eventCallback on the way
    // out. Clearing the emit callback with a dispatch in flight would leave the
    // completion with nowhere to go.
    //
    // The container calls stopDispatch() itself and refuses to destroy a glue
    // whose worker did not stop, so reaching here with one running means some
    // other owner skipped that step. Say so and wait anyway — a short wait is
    // still better than returning into a `delete this` the worker is about to
    // dereference.
    if (!stopDispatch(kDestructorGraceMs))
        spdlog::critical("Bare module {} is being destroyed with a dispatch still "
                         "inside its image", m_name);

    // Clear before the image is closed. logos_module_impl.h: after the clearing
    // call returns, the module must not invoke the old callback — which is what
    // makes this the last moment `this` is reachable from module code.
    if (m_abi.setEmitCallback)
        m_abi.setEmitCallback(nullptr, nullptr);
}

void BareModuleGlue::enableDeferredDispatch()
{
    std::lock_guard<std::mutex> lock(m_dispatchMutex);
    // Already deferring, or retired — and a retired glue never defers again:
    // the container's very next steps are to unpublish it and close the image.
    if (m_dispatch != Dispatch::Inline)
        return;

    // A plain QThread, whose default run() is exec(): the worker needs a Qt
    // event dispatcher of its own, because a handler that calls another module
    // spins nested QEventLoops to acquire the replica and await the reply. A
    // std::thread cannot pump those and such a call would hang — the same
    // reason the generated `multi` glue insists on QThread::create.
    m_workerThread = new QThread();
    m_workerThread->setObjectName(QString::fromStdString("logos-inproc-" + m_name));
    m_workerContext = new QObject();
    m_workerContext->moveToThread(m_workerThread);
    m_workerThread->start();
    m_dispatch = Dispatch::Deferred;
}

bool BareModuleGlue::stopDispatch(int graceMs)
{
    QThread* thread = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_dispatchMutex);
        // RETIRED BEFORE THE LOOP IS ASKED TO STOP, so the set of calls this
        // owes a completion to can only shrink from here: a caller either
        // registered its call under the lock already, or arrives after this and
        // is refused outright rather than posted onto a stopping loop.
        m_dispatch = Dispatch::Retired;
        thread = m_workerThread;
    }
    if (!thread)
        return true;

    thread->quit();
    const bool stopped = thread->wait(graceMs);

    // WHAT quit() LEAVES BEHIND. Asking a QThread to quit interrupts its event
    // dispatcher, so the dispatches already posted behind the one in flight are
    // never delivered — measured at 8 of 9 for a burst queued behind a call in
    // flight. Each of those callers holds a pending-call sentinel, which is a
    // promise of a completion over the event channel, and the teardown this is
    // part of is about to take that channel away. So they are answered HERE,
    // while the listener is still attached, instead of timing out.
    //
    // Done whether or not the wait succeeded: on the failure path the worker is
    // still running and may yet reach one of them, and claimQueuedCall is what
    // keeps exactly one of the two answering each call.
    for (const QueuedCall& call : takeQueuedCalls())
        deliverCompletion(call.callId, call.methodName, unloadingAnswer(call.methodName));

    if (!stopped) {
        // Bounded on purpose. The thread this runs on is the one a blocked
        // handler is waiting for, so an unbounded wait here turns one stuck
        // module into a hung host. Everything stays alive and is reported to
        // the caller, which is the only party that can decide what to abandon.
        spdlog::error("Bare module {} did not leave its image within {}ms; "
                      "its dispatch thread is still running", m_name, graceMs);
        return false;
    }

    std::lock_guard<std::mutex> lock(m_dispatchMutex);
    // Deleting the context drops the posted calls the loop never delivered —
    // which is safe only because they were answered above.
    delete m_workerContext;
    m_workerContext = nullptr;
    delete m_workerThread;
    m_workerThread = nullptr;
    return true;
}

void BareModuleGlue::readContract()
{
    if (!m_abi.getMethods)
        return;

    ModuleString json(m_abi, m_abi.getMethods());
    if (!json) {
        spdlog::warn("Bare module {} answered no contract from logos_module_get_methods()", m_name);
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(json.get()));
    if (!doc.isArray()) {
        spdlog::warn("Bare module {} published a contract that is not a JSON array", m_name);
        return;
    }
    m_methods = doc.array();

    for (const QJsonValue& entry : m_methods) {
        if (!entry.isObject())
            continue;
        const QJsonObject obj = entry.toObject();
        // Events ride inside getMethods() tagged "event"; they have no return
        // shape to classify. An entry with no "type" is a method, which is what
        // keeps a pre-events module reading cleanly.
        if (obj.value("type").toString() == QStringLiteral("event"))
            continue;
        const QString name = obj.value("name").toString();
        if (name.isEmpty())
            continue;
        const QString ret = obj.value("returnType").toString();
        if (ret == QLatin1String(kVoidReturn))
            m_voidMethods.insert(name);
        else if (ret == QLatin1String(kResultReturn))
            m_resultMethods.insert(name);
    }

    spdlog::debug("Bare module {} published {} contract entries ({} void, {} result)",
                  m_name, m_methods.size(), m_voidMethods.size(), m_resultMethods.size());
}

QJsonArray BareModuleGlue::getMethods()
{
    return m_methods;
}

QVariant BareModuleGlue::callMethod(const QString& methodName, const QVariantList& args)
{
    if (!m_abi.dispatch)
        return QVariant();

    // THE PULL, before anything can hand this call to another thread: the
    // caller document lives in a thread-local ModuleProxy opened on the
    // delivering thread. Unlike the generated glue this runs in the SAME image
    // as ModuleProxy, so the thread-local it reads is the one CallerScope wrote
    // — the invokeMethod indirection the generated glue needs has nothing to
    // cross here. A worker has no scope and never will, so a pull made there
    // would answer Unknown for every caller.
    const std::string callerJson = currentCallerJson();

    QString callId;
    {
        std::lock_guard<std::mutex> lock(m_dispatchMutex);
        switch (m_dispatch) {
        case Dispatch::Inline:
            // Dispatched below, once the lock is released: the dispatch enters
            // the module image and the image may call straight back into this
            // glue. An empty `callId` is what says the call took this branch.
            break;

        case Dispatch::Retired:
            // The worker is gone and aboutToUnload has already run, so the
            // module has been told it is going away. Dispatching inline now
            // would walk into a quiesced image on the very thread unloading it;
            // an immediate no is the answer the caller can actually use.
            return unloadingAnswer(methodName);

        case Dispatch::Deferred:
            // Hand the dispatch to the worker and answer the pending-call
            // sentinel. The consumer transport keys on `callId` and waits for
            // the completion event; see the threading note in the header for
            // why the delivering thread must be released rather than blocked.
            callId = QStringLiteral("bc-%1").arg(
                static_cast<qulonglong>(m_callCounter.fetch_add(1, std::memory_order_relaxed)));

            // REGISTERED BEFORE IT IS POSTED, under the lock stopDispatch takes
            // to retire: from here the call belongs to exactly one of the
            // worker and the stop, and neither can fail to see it.
            m_queued.push_back(QueuedCall{callId, methodName});

            QMetaObject::invokeMethod(
                m_workerContext,
                [this, methodName, args, callerJson, callId]() {
                    if (!claimQueuedCall(callId))
                        return;    // stopDispatch answered this one already
                    deliverCompletion(callId, methodName,
                                      dispatchOnThisThread(methodName, args, callerJson));
                },
                Qt::QueuedConnection);
            break;
        }
    }

    if (callId.isEmpty())
        return dispatchOnThisThread(methodName, args, callerJson);

    QVariantMap pending;
    pending[logos::pendingCallKey()] = callId;
    return pending;
}

bool BareModuleGlue::claimQueuedCall(const QString& callId)
{
    std::lock_guard<std::mutex> lock(m_dispatchMutex);
    for (auto it = m_queued.begin(); it != m_queued.end(); ++it) {
        if (it->callId == callId) {
            m_queued.erase(it);
            return true;
        }
    }
    return false;
}

std::vector<BareModuleGlue::QueuedCall> BareModuleGlue::takeQueuedCalls()
{
    std::lock_guard<std::mutex> lock(m_dispatchMutex);
    std::vector<QueuedCall> taken;
    taken.swap(m_queued);
    return taken;
}

void BareModuleGlue::deliverCompletion(const QString& callId, const QString& methodName,
                                       const QVariant& value)
{
    const EventCallback cb = eventListener();
    if (!cb) {
        spdlog::warn("Bare module {}: dispatch of '{}' completed with no event "
                     "listener attached; the caller will time out",
                     m_name, methodName.toStdString());
        return;
    }
    cb(logos::callCompleteEvent(), QVariantList{ callId, value });
}

QVariant BareModuleGlue::unloadingAnswer(const QString& methodName) const
{
    if (m_resultMethods.contains(methodName)) {
        LogosResult lr;
        lr.success = false;
        lr.error = QString::fromStdString("module " + m_name + " is unloading");
        return QVariant::fromValue(lr);
    }
    // Every other return shape has exactly one failure token in this slot and
    // it is the invalid QVariant — what a dispatch the image refused answers.
    return QVariant();
}

QVariant BareModuleGlue::dispatchOnThisThread(const QString& methodName,
                                              const QVariantList& args,
                                              const std::string& callerJson)
{
    nlohmann::json jArgs = nlohmann::json::array();
    for (const QVariant& a : args)
        jArgs.push_back(logos::qvariantToNlohmann(a));
    const std::string dumped = jArgs.dump();

    if (m_abi.setCallCaller)
        m_abi.setCallCaller(callerJson.c_str());

    ModuleString result(m_abi, m_abi.dispatch(methodName.toUtf8().constData(), dumped.c_str()));

    if (m_abi.setCallCaller)
        m_abi.setCallCaller(nullptr);   // NULL pops the innermost push

    if (!result)
        return QVariant();

    nlohmann::json jResult = nlohmann::json::parse(result.get(), nullptr, false);
    if (jResult.is_discarded())
        return QVariant();

    // A `void` method answers QVariant(true) whatever crossed the C ABI: an
    // invalid QVariant is this slot's failure token, so a void method needs
    // SOME value to mean "it ran".
    if (m_voidMethods.contains(methodName))
        return QVariant(true);

    // A `result` method re-materialises the Qt LogosResult its callers expect;
    // {success, value, error} is what crossed as JSON.
    if (m_resultMethods.contains(methodName) && jResult.is_object()) {
        LogosResult lr;
        lr.success = jResult.value("success", false);
        lr.value = logos::nlohmannToQVariant(jResult.value("value", nlohmann::json()));
        lr.error = jResult.contains("error") && jResult["error"].is_string()
            ? QVariant(QString::fromStdString(jResult["error"].get<std::string>()))
            : QVariant();
        return QVariant::fromValue(lr);
    }

    return logos::nlohmannToQVariant(jResult);
}

bool BareModuleGlue::informModuleToken(const QString& moduleName, const QString& token)
{
    // ONE SAVE, not two — and this is the one place the in-process glue must
    // NOT copy the generated one.
    //
    // The generated Qt glue saves twice because there are two images: the
    // host's TokenManager, which ModuleProxy validates inbound calls against,
    // and the cdylib's own statically-linked copy, reached over
    // logos_module_accept_inbound_token. A Bare module has no copy of its own —
    // its `lp_*` resolve UPWARD into this image — so the ABI door and this save
    // would land in the same process, and the door lands in the WRONG PLACE:
    // lp_token_save_inbound writes TokenManager::instance(), the host's ambient
    // ring, while the save below writes the module's own isolated store. The
    // second write would add an ambient entry keyed by the caller's name, where
    // it can collide with the HOST's own record for that caller.
    //
    // Measured, on the door's outbound sibling: forwarding
    // logos_module_accept_token in-process overwrote the core's ambient
    // `capability_module` anchor with the module's auth token, after which
    // core's next informModuleToken was refused with "caller is not the trusted
    // core/capability_module channel" and no module could be registered again.
    return LogosProviderBase::informModuleToken(moduleName, token);
}

void BareModuleGlue::setEventListener(EventCallback callback)
{
    {
        std::lock_guard<std::mutex> lock(m_eventMutex);
        m_eventCallback = callback;
    }
    LogosProviderBase::setEventListener(std::move(callback));
}

BareModuleGlue::EventCallback BareModuleGlue::eventListener() const
{
    std::lock_guard<std::mutex> lock(m_eventMutex);
    return m_eventCallback;
}

void BareModuleGlue::emitTrampoline(const char* eventName, const char* dataJson, void* userData)
{
    auto* self = static_cast<BareModuleGlue*>(userData);
    if (!self || !eventName)
        return;
    nlohmann::json payload = dataJson
        ? nlohmann::json::parse(dataJson, nullptr, false)
        : nlohmann::json::array();
    if (payload.is_discarded() || !payload.is_array())
        payload = nlohmann::json::array();
    self->onModuleEvent(QString::fromUtf8(eventName),
                        logos::nlohmannArgsToQVariantList(payload));
}

void BareModuleGlue::onModuleEvent(const QString& eventName, const QVariantList& data)
{
    const EventCallback cb = eventListener();
    if (!cb) {
        spdlog::debug("Bare module {} emitted '{}' with no listener attached",
                      m_name, eventName.toStdString());
        return;
    }
    cb(eventName, data);
}

void BareModuleGlue::onInit(LogosAPI* api)
{
    // Nothing to forward across an image boundary here — there is no boundary.
    // The container stamps the context and the auth token explicitly, in the
    // order logos_module_impl.h documents, once registration has produced the
    // provider the module will answer on.
    (void)api;
}

void BareModuleGlue::grantHostServices(const QString& servicesJson)
{
    // The one door that IS still worth crossing in-process. lp_grant_host_services
    // opens gates in the image whose `lp_*` the module actually calls, and
    // in-process that image is the host — which is the correct place for a
    // module the host's policy designates as the trust root, because in-process
    // there is only the one image to designate.
    if (servicesJson.isEmpty() || !m_abi.grantHostServices)
        return;
    if (m_abi.grantHostServices(servicesJson.toUtf8().constData()) != 0)
        spdlog::warn("Bare module {} refused the host-services grant {}",
                     m_name, servicesJson.toStdString());
}

void BareModuleGlue::deliverContext(const QString& modulePath,
                                    const QString& instanceId,
                                    const QString& instancePersistencePath)
{
    if (!m_abi.setContext)
        return;
    m_abi.setContext(modulePath.toUtf8().constData(),
                     instanceId.toUtf8().constData(),
                     instancePersistencePath.toUtf8().constData());
}

void BareModuleGlue::aboutToUnload(int graceMs)
{
    if (!m_abi.aboutToUnload)
        return;

    // The whole rendezvous in one object, so the module's callback carries a
    // single void* rather than three pointers into this frame.
    struct Waiter {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
    } waiter;

    if (m_abi.setUnloadDoneCallback) {
        m_abi.setUnloadDoneCallback([](void* ud) {
            auto* w = static_cast<Waiter*>(ud);
            {
                std::lock_guard<std::mutex> lock(w->mutex);
                w->done = true;
            }
            w->cv.notify_all();
        }, &waiter);
    }

    const bool pending = m_abi.aboutToUnload() == 1;
    if (pending && m_abi.setUnloadDoneCallback) {
        std::unique_lock<std::mutex> lock(waiter.mutex);
        // BOUNDED. Returning 1 buys a grace period, not a veto: a module that
        // never signals delays this teardown by `graceMs` and is torn down
        // anyway.
        if (!waiter.cv.wait_for(lock, std::chrono::milliseconds(graceMs),
                                [&] { return waiter.done; }))
            spdlog::warn("Bare module {} did not finish its teardown within {}ms; "
                         "unloading anyway", m_name, graceMs);
    }

    // Detach before the waiter leaves scope, whatever happened above.
    if (m_abi.setUnloadDoneCallback)
        m_abi.setUnloadDoneCallback(nullptr, nullptr);
}

} // namespace LogosCore
