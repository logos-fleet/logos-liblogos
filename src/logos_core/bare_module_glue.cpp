#include "bare_module_glue.h"

#include <logos_json_convert.h>
#include <logos_types.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
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
    // Clear before the image is closed. logos_module_impl.h: after the clearing
    // call returns, the module must not invoke the old callback — which is what
    // makes this the last moment `this` is reachable from module code.
    if (m_abi.setEmitCallback)
        m_abi.setEmitCallback(nullptr, nullptr);
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
    // cross here.
    const std::string callerJson = currentCallerJson();

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
    m_eventCallback = callback;
    LogosProviderBase::setEventListener(std::move(callback));
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
    if (!m_eventCallback) {
        spdlog::debug("Bare module {} emitted '{}' with no listener attached",
                      m_name, eventName.toStdString());
        return;
    }
    m_eventCallback(eventName, data);
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
