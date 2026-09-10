#include "inproc_container.h"
#include "bare_module_glue.h"

#include <logos_api.h>
#include <logos_api_provider.h>
#include <logos_transport_config_json.h>
#include <token_manager.h>

#include <QString>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <utility>

namespace fs = std::filesystem;

namespace LogosCore {

// One loaded Bare module: the image, the glue driving it, and the LogosAPI it
// is published on. Destruction order is load-bearing and is why this is a
// struct with an explicit teardown rather than four parallel maps.
struct InProcContainer::Instance {
    std::string name;
    BareModuleAbi abi;
    std::unique_ptr<BareModuleGlue> glue;
    // Owned by `api` when there is one (registerObject does not take
    // ownership, but the LogosAPI is parented to nothing and deleted here).
    LogosAPI* api = nullptr;
    std::function<void(const std::string&)> onTerminated;
};

namespace {

// The host-services policy, mirroring the subprocess loader's: which module may
// hold the trust-root services is decided by the HOST and bound to the name the
// registry trusts, never to anything the module asserts about itself.
//
// The same two entries, deliberately duplicated rather than shared: the
// subprocess copy lives in another repo behind a command line, and a table this
// short is clearer stated twice than reached for through a seam. If a third
// container appears, hoist it.
const char* hostServicesJsonFor(const std::string& moduleName)
{
    if (moduleName == "capability_module")
        return R"(["token_registry","token_delivery"])";
    return nullptr;
}

} // namespace

InProcContainer::InProcContainer() = default;

InProcContainer::~InProcContainer()
{
    terminateAll();
}

void InProcContainer::setHostApi(LogosAPI* hostApi)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hostApi = hostApi;
}

bool InProcContainer::canHandle(const ModuleDescriptor& desc) const
{
    return desc.format == "bare";
}

bool InProcContainer::launch(const ModuleDescriptor& desc,
                             const std::string& /*hostBinary*/,
                             const std::vector<std::string>& /*args*/,
                             std::function<void(const std::string& name)> onTerminated,
                             LoadedModuleHandle& out)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_modules.count(desc.name)) {
        spdlog::warn("In-process module already running: {}", desc.name);
        return false;
    }

    // ── open the image ────────────────────────────────────────────────────
    // A failure here is a LOAD ERROR the caller reports, not a crash. That is
    // the whole reason openBareModule takes an error string rather than
    // trusting dlopen's diagnostics to reach anybody.
    BareModuleAbi abi;
    std::string error;
    if (!openBareModule(desc.path, abi, &error)) {
        spdlog::error("Failed to load in-process module {}: {}", desc.name, error);
        return false;
    }

    // ── the runtime protocol handshake ────────────────────────────────────
    // A Bare module carries no Qt plugin metadata, so the build-time stamp the
    // subprocess path gates on does not exist here. logos_module_impl.h puts
    // the same fact behind a symbol for exactly this case.
    const char* version = abi.protocolVersion ? abi.protocolVersion() : nullptr;
    std::string reason;
    if (!bareModuleProtocolCompatible(version ? version : std::string(), &reason)) {
        spdlog::error("Refusing to load in-process module {}: {}", desc.name, reason);
        closeBareModule(abi);
        return false;
    }
    if (!reason.empty())
        spdlog::warn("In-process module {}: {}", desc.name, reason);

    auto instance = std::make_unique<Instance>();
    instance->name = desc.name;
    instance->abi = abi;
    instance->onTerminated = std::move(onTerminated);

    std::string moduleVersion;
    if (desc.rawMetadata.is_object())
        moduleVersion = desc.rawMetadata.value("version", std::string("1.0.0"));
    if (moduleVersion.empty())
        moduleVersion = "1.0.0";

    instance->glue = std::make_unique<BareModuleGlue>(desc.name, moduleVersion, instance->abi);

    // ── publish it ────────────────────────────────────────────────────────
    //
    // PER-IDENTITY, NOT PER-IMAGE. A subprocess module gets its own token store
    // for free: it is its own image, and TokenManager is an image singleton. An
    // in-process module shares the host's image, so "its" store has to be
    // asked for. logos::admitConsumer is that operation — isolate the identity,
    // mint its credential, register it with capability_module, adopt it — and
    // it is what makes an inbound call to this module authorize AS this module
    // rather than as the host.
    //
    // ITS OUTBOUND HALF IS NOT ISOLATED YET, and the limit is exact rather than
    // vague. A Bare module's `lp_*` resolve into THIS image's logos-protocol,
    // and every lp_token_* entry point reads and writes
    // TokenManager::instance() — the host's ambient ring — not
    // forIdentity(name). So a Bare module that CALLS another module presents
    // whatever the ambient ring holds, which is the host's own credentials.
    //
    // A leaf module (the counter, and every module in the Bundled set that
    // depends on nothing) references no lp_invoke at all and is unaffected: the
    // Bare gate in logos-module-builder shows lp_invoke UNDEFINED only for a
    // module with dependencies. Making lp_* identity-aware so a DEPENDENT Bare
    // module presents its own credential is slice 17 (two Bare modules in one
    // process), and it is spelled out here so the limit is read rather than
    // rediscovered.
    //
    // What is NOT deferred is the inbound half, which is what this admission
    // buys today: a call INTO this module authorizes against the module's own
    // store, so it is authorized as this module and not as the host.
    if (m_hostApi) {
        const QString qname = QString::fromStdString(desc.name);
        // ISOLATE ONLY — NOT admitConsumer, and the difference is the whole of
        // this module's identity.
        //
        // admitConsumer does four things: isolate, MINT a credential, REGISTER
        // it with capability_module, adopt. Three of those are already done for
        // a MODULE, by the load path that is about to call sendToken(): it
        // mints the auth token, saves it, and pushes it to capability_module
        // (ModuleManager::notifyCapabilityModule). A second minting here would
        // leave capability_module holding one credential for this name and the
        // module's store holding another, and every inbound call would be
        // refused with "token not recognized (re-exchange failed)" — measured,
        // exactly that, before this was split.
        //
        // So the container takes the store-selection half (LogosAPI::forIdentity)
        // and adopts the credential the CORE minted, which is the case
        // logos::adoptConsumerCredential exists for: "minted and registered
        // ELSEWHERE". See sendToken below.

        // THE TRANSPORT SET IS A CONSTRUCTOR ARGUMENT, not a later setter, and
        // that is why it is resolved before the LogosAPI exists: the provider
        // binds its listeners in ITS constructor, so a set applied afterwards
        // binds nothing.
        //
        // It matters here and nowhere else in the container. A SUBPROCESS
        // module is handed `--transports` on its command line and builds its
        // own LogosAPI from it; an in-process module gets only what this line
        // gives it. Left at the process-global default, a host the operator
        // configured for (say) TCP alone would DIAL this module on a listener
        // it never bound — ModuleManager::moduleClient() picks the first
        // configured transport — and the call would hang at acquire rather than
        // fail. Empty set means the global default, exactly as before.
        //
        // LATENT TODAY, and said so rather than implied: the only caller of
        // logos_core_set_module_transports is the logoscore daemon, and it
        // names exactly one module — capability_module. So no Bare module
        // carries a set yet, and every one of them takes the empty branch. It
        // stops being latent the moment the Bundled-set build (#9) makes
        // capability_module Bare, which is also the moment the failure would
        // be a hang in the core's own trust path.
        LogosTransportSet transports;
        if (!desc.transportSetJson.empty())
            transports = logos::transportSetFromJsonString(desc.transportSetJson);
        if (!transports.empty())
            spdlog::debug("In-process module {} publishes on {} configured transport(s)",
                          desc.name, transports.size());

        instance->api = LogosAPI::forIdentity(qname, std::move(transports));
        if (!instance->api) {
            spdlog::error("Failed to isolate the token store for in-process module {}; "
                          "refusing the load — half an identity is worse than none",
                          desc.name);
            instance->glue.reset();
            closeBareModule(instance->abi);
            return false;
        }

        if (!instance->api->getProvider()->registerObject(qname, instance->glue.get())) {
            spdlog::error("Failed to publish in-process module {}", desc.name);
            instance->glue.reset();
            closeBareModule(instance->abi);
            return false;
        }

        // init() hands the provider its LogosAPI, the same hand-off the
        // subprocess initializer performs through registerObject.
        instance->glue->init(instance->api);
    } else {
        spdlog::debug("In-process module {} loaded without a host API; not published", desc.name);
    }

    // ── stamp identity and context ────────────────────────────────────────
    // Same order as the generated Qt glue's onInit: the grant and the token
    // first (so a context-ready hook can already make authenticated calls),
    // the context last (which is what fires that hook).
    if (const char* services = hostServicesJsonFor(desc.name)) {
        spdlog::info("Granting host services to in-process '{}': {}", desc.name, services);
        instance->glue->grantHostServices(QString::fromUtf8(services));
    }

    const std::string moduleDir = desc.path.empty()
        ? std::string()
        : fs::absolute(fs::path(desc.path)).parent_path().string();
    const std::string instanceId = desc.instancePersistencePath.empty()
        ? std::string()
        : fs::path(desc.instancePersistencePath).filename().string();
    instance->glue->deliverContext(QString::fromStdString(moduleDir),
                                   QString::fromStdString(instanceId),
                                   QString::fromStdString(desc.instancePersistencePath));

    out.name = desc.name;
    out.pid = kInProcPid;
    out.endpoint = "inproc://" + desc.name;

    m_modules.emplace(desc.name, std::move(instance));
    spdlog::info("In-process module started: {} (pid {})", desc.name, kInProcPid);
    return true;
}

bool InProcContainer::sendToken(const std::string& name, const std::string& token)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_modules.find(name);
    if (it == m_modules.end())
        return false;

    Instance& instance = *it->second;

    // ADOPT the credential the core minted and registered — the other half of
    // the split described in launch(). It installs `token` under every
    // bootstrapKeys() key of the module's OWN isolated store, which is both the
    // anchor ModuleProxy::informModuleToken gates on and the value
    // capability_module presents when it pushes a minted pair token here.
    //
    // adoptCredentialFor, NOT logos::adoptConsumerCredential: that verb is for a
    // CO-PROCESS adopting its parent's credential into its own process ring and
    // refuses an isolated store outright, so calling it here writes nothing and
    // says so only on the Qt debug channel. adoptCredentialFor is the primitive
    // under admitConsumer, and its two refusals are exactly the guarantees this
    // path needs: the identity must be isolated (it is, from launch), and the
    // credential must not be the HOST's anchor (it is not — the core minted it
    // per module).
    //
    // Nothing is pushed across the module-impl C ABI. A Bare module has no
    // second image to push into, and the ABI's outbound door
    // (logos_module_accept_token -> lp_token_save) writes the host's AMBIENT
    // ring, where it overwrites the core's own capability_module anchor. See
    // the note on informModuleToken in bare_module_glue.cpp.
    if (instance.api) {
        if (!TokenManager::adoptCredentialFor(QString::fromStdString(name),
                                              QString::fromStdString(token))) {
            spdlog::error("In-process module {} could not adopt its credential; "
                          "every call into it would be refused", name);
            return false;
        }
        return true;
    }
    // No host API (the hermetic test path): there is no store to seed, and the
    // module is not published either, so there is nothing this token could
    // authenticate. Accepting is the honest answer — refusing would make the
    // composite loader tear down a module that loaded fine.
    return true;
}

LoadOutcome InProcContainer::awaitLoad(const std::string& name,
                                       std::chrono::milliseconds /*timeout*/)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_modules.count(name))
        return {LoadVerdict::Loaded, {}};
    return {LoadVerdict::Failed, "the in-process module is not running"};
}

void InProcContainer::terminate(const std::string& name)
{
    std::unique_ptr<Instance> instance;
    std::function<void(const std::string&)> onTerminated;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_modules.find(name);
        if (it == m_modules.end())
            return;
        instance = std::move(it->second);
        m_modules.erase(it);
        onTerminated = instance->onTerminated;
    }

    // Ask, then unpublish, then close — in that order, because each step
    // removes a way back into the next one's memory.
    instance->glue->aboutToUnload(kUnloadGraceMs);

    if (instance->api) {
        // Deleting the LogosAPI destroys its provider, and ~LogosAPIProvider
        // unpublishes the registered object from every transport it bound. That
        // is the only unpublish path there is — there is no unregisterObject —
        // so the API object's lifetime IS the module's publication.
        delete instance->api;
        instance->api = nullptr;
    }

    // The glue clears the module's emit callback in its destructor, so nothing
    // in the image can reach back once this returns — which is what makes the
    // dlclose below safe, and what makes a reload load a fresh image instead of
    // finding the old one still resident.
    instance->glue.reset();
    closeBareModule(instance->abi);

    spdlog::info("In-process module stopped: {}", name);

    // Announced last, matching the subprocess container: the callback is the
    // signal that the module is gone, so everything that makes it gone runs
    // first.
    if (onTerminated)
        onTerminated(name);
}

void InProcContainer::terminateAll()
{
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        names.reserve(m_modules.size());
        for (const auto& [name, _] : m_modules)
            names.push_back(name);
    }
    for (const auto& name : names)
        terminate(name);
}

bool InProcContainer::hasModule(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_modules.count(name) > 0;
}

std::optional<int64_t> InProcContainer::pid(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_modules.count(name))
        return std::nullopt;
    return kInProcPid;
}

std::unordered_map<std::string, int64_t> InProcContainer::getAllPids() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::unordered_map<std::string, int64_t> pids;
    for (const auto& [name, _] : m_modules)
        pids.emplace(name, kInProcPid);
    return pids;
}

} // namespace LogosCore
