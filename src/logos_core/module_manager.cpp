#include "module_manager.h"
#include "module_registry.h"
#include "access_policy.h"
#include "dependency_resolver.h"
#include "module_loader_registry.h"
#include "composite_module_loader.h"
#include "inproc_container.h"
#include "web_container.h"
#include "web_module_loader.h"
#include "bare_module_loader.h"
#include "module_state_observer.h"
#include "module_supervisor.h"
#include <logos_container/container_factory.h>
#include <logos_module_loader/format_loader_factory.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <QCoreApplication>
#include <QMetaObject>
#include <QString>
#include <QThread>
#include <QVariant>
#include <QVariantList>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <cassert>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include "logos_api.h"
#include "logos_thread_marshal.h"
#include "logos_api_client.h"
#include "logos_module.h"
#include "logos_protocol.h"
#include "dependency_gate.h"
#include "protocol_gate.h"
#include "logos_transport_config_json.h"
#include "token_manager.h"
#include "instance_persistence.h"

namespace {
    ModuleRegistry& registryInstance() {
        static ModuleRegistry instance;
        return instance;
    }

    // Load locks, in the order they must be taken: fleet -> module ->
    // spawn -> config. One lock over the whole path made every load queue
    // behind every other one whatever module it named.
    //
    //   fleet          shared per-module; exclusive for whatever moves the
    //                  whole loaded set (clear, terminateAll, unload cascade)
    //   module         one per name, held for that module's whole load/unload
    //   spawn          process creation, bounded rather than made safe -- see
    //                  spawnMutex
    //   config         the transport map and access policy a load reads
    //
    // Not here: ModuleRegistry has its own; inFlight/expectedExit are taken on
    // the asio thread and must never wait behind a load; and the observer is
    // never dispatched under any of them (RULE 1 in module_state_observer.h).
    std::shared_mutex& fleetMutex() {
        static std::shared_mutex m;
        return m;
    }

    std::shared_mutex& configMutex() {
        static std::shared_mutex m;
        return m;
    }

    // RE-ENTRANCY, on one thread, and it is not hypothetical: requestObject and
    // informModuleToken spin nested Qt event loops, so a load a frontend posted
    // with a queued connection can be delivered INSIDE one already running.
    // Proceeding would take fleetMutex shared recursively — undefined behaviour,
    // and a deadlock against a queued writer. The old global lock deadlocked
    // outright here, so refusing loses nothing that ever worked.
    bool& threadIsInsideLoad() {
        static thread_local bool inside = false;
        return inside;
    }

    struct ScopedLoadEntry {
        const bool reentrant;
        ScopedLoadEntry() : reentrant(threadIsInsideLoad()) {
            if (!reentrant) threadIsInsideLoad() = true;
        }
        ~ScopedLoadEntry() {
            if (!reentrant) threadIsInsideLoad() = false;
        }
    };

    // "Already loaded" stays a truthful yes even here — the registry has its own
    // lock, so answering it needs none of ours.
    bool refuseReentrantLoad(const std::string& name) {
        if (registryInstance().isLoaded(name))
            return true;
        spdlog::error("Refusing a re-entrant load of {}: this thread is already "
                      "inside a load or unload", name);
        return false;
    }

    // SPAWNING is serialized even though loads are not. What this DOES buy is
    // an upper bound on concurrent process creation from this path; what it
    // does NOT buy is fork safety, and the earlier version of this comment
    // claimed otherwise. ModuleContainer does not promise a container spawns
    // without forking, and a fork here is unsafe because this process is
    // multithreaded: the container's own io thread is a second thread and can
    // hold an asio-internal lock at fork time. One spawner is enough to
    // deadlock — reproduced 3/3 with spawning fully serialized.
    //
    // The real fix belongs to the container and landed there
    // (logos-container-subprocess#7, posix_spawn). This stays because liblogos
    // cannot verify it: the launcher is a choice made by a dependency of a
    // dependency that liblogos neither pins nor can see, and the failure mode
    // is a multi-hour hang rather than a red test. Removing it wants a test
    // HERE that would catch a forking loader first.
    //
    // It costs almost nothing. Spawning is ~2 ms; the wait for the child's
    // verdict is ~34 ms warm and hundreds cold, and THAT is what this change
    // exists to overlap. It stays outside this lock.
    std::mutex& spawnMutex() {
        static std::mutex m;
        return m;
    }

    // Grows only: erasing an entry would race with whoever is holding it.
    std::mutex& moduleMutex(const std::string& name) {
        static std::mutex mapMutex;
        static std::unordered_map<std::string, std::unique_ptr<std::mutex>> locks;
        std::lock_guard<std::mutex> g(mapMutex);
        auto& slot = locks[name];
        if (!slot) slot = std::make_unique<std::mutex>();
        return *slot;
    }

    // Per-module transport set, keyed by module name. Set by the
    // daemon before the corresponding module loads (capability_module
    // before logos_core_start; user modules before loadModule). Empty
    // = inherit the global default. See module_manager.h for details.
    // Guarded by configMutex().
    std::unordered_map<std::string, std::string>& moduleTransportsMap() {
        static std::unordered_map<std::string, std::string> m;
        return m;
    }

    std::string& persistenceBasePath() {
        static std::string path;
        return path;
    }

    // ── Orderly teardown vs. death ───────────────────────────────────────────
    //
    // onTerminated fires for BOTH an unload we asked for and a module that
    // exited on its own, and cannot tell them apart from its arguments — but
    // they are different states (`stopping -> unloaded` vs `loaded -> error`).
    // So teardown announces intent here before terminate(); a name NOT in this
    // set died without being asked to.
    //
    // Its own mutex, not any of the load locks: the callback runs on the
    // container's background asio thread and must never wait behind a load.
    std::mutex& expectedExitMutex() {
        static std::mutex m;
        return m;
    }

    std::unordered_set<std::string>& expectedExits() {
        static std::unordered_set<std::string> s;
        return s;
    }

    // Defined below loadModuleInternal, which is where the death that reaches
    // it is described; declared here because the callback that calls it is
    // built inside that function.
    void superviseUnexpectedExit(const std::string& name);

    void markExitExpected(const std::string& name) {
        std::lock_guard<std::mutex> g(expectedExitMutex());
        expectedExits().insert(name);
    }

    // Consumes the mark: returns true exactly once per announced teardown, so a
    // module that is unloaded, reloaded and then CRASHES is reported as a crash
    // rather than inheriting the earlier orderly exit.
    bool consumeExpectedExit(const std::string& name) {
        std::lock_guard<std::mutex> g(expectedExitMutex());
        return expectedExits().erase(name) > 0;
    }

    // ── A load attempt, and a death that lands inside one ────────────────────
    //
    // A child that dies mid-load is reported by the load path, which has the
    // child's reason, instead of by onTerminated from another thread. And
    // because markUnloaded() does nothing to a module not yet marked loaded,
    // such a death used to be erased by the markLoaded behind it — leaving a
    // module that is not there marked loaded for good. commitLoad() settles
    // both under one lock.
    std::mutex& inFlightMutex() {
        static std::mutex m;
        return m;
    }

    // name -> "a termination arrived during this attempt".
    std::unordered_map<std::string, bool>& inFlightLoads() {
        static std::unordered_map<std::string, bool> m;
        return m;
    }

    void beginLoadAttempt(const std::string& name) {
        std::lock_guard<std::mutex> g(inFlightMutex());
        inFlightLoads()[name] = false;
    }

    void abandonLoadAttempt(const std::string& name) {
        std::lock_guard<std::mutex> g(inFlightMutex());
        inFlightLoads().erase(name);
    }

    // Called from the container's asio thread. Returns true when the load path
    // owns this termination and onTerminated should stay quiet about it.
    bool recordTerminationDuringLoad(const std::string& name) {
        std::lock_guard<std::mutex> g(inFlightMutex());
        registryInstance().markUnloaded(name);
        auto it = inFlightLoads().find(name);
        if (it == inFlightLoads().end()) return false;
        it->second = true;
        return true;
    }

    // Close the attempt and mark the module loaded as one step, so a
    // termination is either counted by the attempt or lands after the registry
    // says loaded — never in the gap between, where it would be lost.
    // Returns false when the module died before this could commit.
    bool commitLoad(const std::string& name,
                    std::shared_ptr<LogosCore::ModuleLoader> loader,
                    LogosCore::LoadedModuleHandle handle) {
        std::lock_guard<std::mutex> g(inFlightMutex());
        auto it = inFlightLoads().find(name);
        const bool died = it != inFlightLoads().end() && it->second;
        if (it != inFlightLoads().end()) inFlightLoads().erase(it);
        if (died) return false;
        registryInstance().markLoaded(name, std::move(loader), std::move(handle));
        return true;
    }

    // How long a load waits for the child's own verdict. A host that reports its
    // status answers in the time it takes to start, so only one that never
    // reports reaches this.
    constexpr std::chrono::milliseconds kLoadVerdictTimeout{10000};

    // What it shrinks to once a host has been seen to stay silent through a
    // whole load. The same host binary loads every module here, so one that
    // predates the status line costs the deadline once, not once per module —
    // and this still catches a child that dies starting up (tens of ms).
    constexpr std::chrono::milliseconds kSilentHostGrace{1000};

    std::atomic<bool>& hostStaysSilent() {
        static std::atomic<bool> silent{false};
        return silent;
    }

    // Host shutdown tears down EVERY loaded module at once, and each one
    // triggers onTerminated. Without announcing them first, a clean shutdown
    // reports the whole fleet as having crashed — `loaded -> error`, "module
    // exited without being asked to", once per module — which is both wrong and
    // the single most alarming thing this feed can say.
    //
    // Callers hold fleetMutex() EXCLUSIVELY, so the loaded set cannot move
    // underneath: every load and unload holds it shared for its whole span.
    void markAllLoadedExitsExpected() {
        for (const std::string& n : registryInstance().loadedModuleNames())
            markExitExpected(n);
    }

    // The counterpart, and it is not optional. A container drops the callback
    // for a teardown it performed itself, so nothing consumes the marks above:
    // each one survives to describe that module's NEXT death as an orderly
    // exit, turning a real crash into `stopping -> unloaded` on the feed. Called
    // once the teardown is done, when by construction no announced teardown is
    // still outstanding.
    void clearExpectedExits() {
        std::lock_guard<std::mutex> g(expectedExitMutex());
        expectedExits().clear();
    }

    // The operator's container assertion. Guarded by loadMutex()'s successor
    // (fleetMutex) on read, and written before any load runs.
    std::string& containerPolicyValue() {
        static std::string p = "auto";
        return p;
    }

    // Does `policy` forbid running the module `name` out of a `format`
    // artifact? Answers the reason it does, or nullptr when the load may
    // proceed. See logos_core_set_container_policy for what each policy means.
    //
    // The runtime's OWN modules are exempt, and this is not a loophole.
    // capability_module and modules_state are loaded by the core itself,
    // unconditionally, before any operator module — a policy that refused them
    // would turn `--container inproc` into "start a core with no
    // capability_module", i.e. a core in which every cross-module call is
    // refused, which is not what anyone asking for the Native container means.
    //
    // EXEMPT IS NOT THE SAME AS SUBPROCESS, and the difference is now live for
    // capability_module: it has a Bare artifact (logos-capability-module builds
    // a `bare` output), and where the modules directory supplies that artifact
    // it loads into the Native container like any other Bare module — the
    // ARTIFACT decides the container, and this exemption only decides whether a
    // disagreement with the policy is fatal. What the exemption still buys is
    // the case where the artifact on disk is the Qt plugin (logoscore's own
    // bundle ships one): the core comes up with its trust root in a subprocess
    // rather than not at all.
    const char* containerPolicyMismatch(const std::string& policy,
                                        const std::string& name,
                                        const std::string& format) {
        static const std::string kPolicyExempt[] =
            {"capability_module", "modules_state", "core", "core_service"};
        if (std::find(std::begin(kPolicyExempt), std::end(kPolicyExempt), name)
                != std::end(kPolicyExempt))
            return nullptr;

        // One block per policy, each opening with the artifact it DEMANDS and
        // then refusing the other two in turn. "auto" demands nothing and falls
        // off the end. A Qt plugin is the shape that has no marker of its own —
        // it is what a module is when it is neither of the other two.
        const bool isBare = (format == "bare");
        const bool isWeb  = (format == "web");
        const bool isQtPlugin = !isBare && !isWeb;

        if (policy == "inproc") {
            if (isBare) return nullptr;
            if (isWeb)
                return "the container policy is 'inproc' but this module is a "
                       "web variant, which only the Web container can run";
            return "the container policy is 'inproc' but this module is a Qt "
                   "plugin, which only a subprocess host can run — it ships no "
                   "Bare module artifact";
        }

        if (policy == "subprocess") {
            if (isQtPlugin) return nullptr;
            if (isBare)
                return "the container policy is 'subprocess' but this module is "
                       "a Bare module image, which only the Native container "
                       "can run";
            return "the container policy is 'subprocess' but this module is a "
                   "web variant, which only the Web container can run";
        }

        if (policy == "web") {
            if (isWeb) return nullptr;
            if (isBare)
                return "the container policy is 'web' but this module is a Bare "
                       "module image, which only the Native container can run";
            return "the container policy is 'web' but this module is a Qt "
                   "plugin, which only a subprocess host can run — it ships no "
                   "web variant";
        }

        return nullptr;
    }

    // Both guarded by configMutex(). parsedEnforcePolicy is set only in enforce mode.
    std::string& accessPolicyJson() {
        static std::string s;
        return s;
    }

    std::optional<LogosCore::AccessPolicy>& parsedEnforcePolicy() {
        static std::optional<LogosCore::AccessPolicy> p;
        return p;
    }

    // Always allowed past the dependency check, so they're never locked out.
    const std::vector<std::string> kTrustedCallers = {"core", "core_service"};

    // Never restricted as targets, even if an explicit policy names them.
    // TODO: re-eval this; probably is required to restrict core/core_service
    const std::vector<std::string> kExemptTargets =
        {"capability_module", "core", "core_service"};

    // Built-in default loader, composed from the container + format-loader the
    // build linked in. The concrete implementations are chosen at link time via
    // the contract factory seams (LogosCore::makeContainer / makeFormatLoader);
    // the core names no specific container or loader. Frontends can still
    // register additional loaders via ModuleManager::loaders().registerLoader().
    // The Native container, kept by reference as well as in the registry
    // because the host has to hand it the trusted LogosAPI it admits each
    // in-process module through, and a ModuleLoader has no seam for that.
    std::shared_ptr<LogosCore::InProcContainer>& inProcContainer() {
        static std::shared_ptr<LogosCore::InProcContainer> c;
        return c;
    }

    // The Web container, kept by reference for the same reason: it too has to
    // be handed the trusted LogosAPI it publishes each page under, and a
    // ModuleLoader has no seam for that.
    std::shared_ptr<LogosCore::WebContainer>& webContainer() {
        static std::shared_ptr<LogosCore::WebContainer> c;
        return c;
    }

    LogosCore::ModuleLoaderRegistry& loaderRegistry() {
        static LogosCore::ModuleLoaderRegistry reg;
        static std::once_flag initFlag;
        std::call_once(initFlag, []() {
            auto container = LogosCore::makeContainer();
            auto loader    = LogosCore::makeFormatLoader();
            if (container && loader)
                reg.registerLoader(std::make_shared<LogosCore::CompositeModuleLoader>(container, loader));

            // The NATIVE CONTAINER, registered as a SECOND loader rather than
            // swapped in for the first. Both are always present and the
            // descriptor decides: this pair claims only `format == "bare"`,
            // which the Qt pair never claims and which nothing stamps on a Qt
            // plugin. So a workspace with no Bare module behaves exactly as it
            // did, and a host does not choose a container globally — the
            // artifact does.
            //
            // Not reached through LogosCore::makeContainer(): that seam is
            // link-time and admits exactly one provider, which is the right
            // shape for "which container is the DEFAULT" and the wrong shape
            // for "which containers exist".
            inProcContainer() = std::make_shared<LogosCore::InProcContainer>();
            reg.registerLoader(std::make_shared<LogosCore::CompositeModuleLoader>(
                inProcContainer(),
                std::make_shared<LogosCore::BareModuleFormatLoader>()));

            // The WEB CONTAINER, a THIRD loader on the same rule: it claims
            // only `format == "web"`, which nothing stamps on a Qt plugin or a
            // Bare image. Registered unconditionally even though most processes
            // have no webview backend installed, because "is a web module
            // loadable here" is a question the container answers with a
            // diagnostic naming the missing bridge — and a loader that were
            // absent instead would answer it with "no loader available for this
            // module format", which points at the artifact rather than at the
            // host.
            webContainer() = std::make_shared<LogosCore::WebContainer>();
            reg.registerLoader(std::make_shared<LogosCore::CompositeModuleLoader>(
                webContainer(),
                std::make_shared<LogosCore::WebModuleFormatLoader>()));
        });
        return reg;
    }

    char** toNullTerminatedArray(const std::vector<std::string>& list) {
        int count = static_cast<int>(list.size());
        if (count == 0) {
            char** result = new char*[1];
            result[0] = nullptr;
            return result;
        }

        char** result = new char*[count + 1];
        for (int i = 0; i < count; ++i) {
            result[i] = new char[list[i].size() + 1];
            strcpy(result[i], list[i].c_str());
        }
        result[count] = nullptr;
        return result;
    }

    // THE OWNER THREAD, chosen rather than raced for. LogosAPI is a QObject that
    // binds its provider — and on a Qt-affine transport its node and socket — to
    // the thread that CONSTRUCTS it, and every client it hands out inherits that
    // owner. Left lazy that was whichever thread dialled first, which with loads
    // running off the main thread can be one that never pumps. anchorCoreApi()
    // settles it from logos_core_start(). No marshal in this initializer: a magic
    // static that blocks on another thread deadlocks against that thread waiting
    // on the static's own guard. Leaked on purpose — it outlives its clients.
    LogosAPI& coreApi() {
        static LogosAPI* api = new LogosAPI(std::string("core"));
        return *api;
    }

    // Core's outbound dials, on the owner thread. getClient and
    // invokeRemoteMethod marshal there with a BlockingQueuedConnection, so a
    // dial from a thread holding a load lock WAITS for the owner — while the
    // owner blocks on that same lock during its own load, un-pumped. That edge
    // is the whole deadlock class; posting instead of waiting removes it, and
    // the owner then reaches every dial on its own thread where the marshal is
    // a no-op. Inline when we ARE the owner (every shipped host, and the tests)
    // or when there is no event loop to post to, so the common path is
    // unchanged — same thread, same order, same timing.
    template <typename Fn>
    void runOnOwner(Fn&& fn) {
        if (!QCoreApplication::instance() ||
            QThread::currentThread() == coreApi().thread()) {
            fn();
            return;
        }
        QMetaObject::invokeMethod(&coreApi(), std::forward<Fn>(fn),
                                  Qt::QueuedConnection);
    }

    // Dial `name` from a long-lived "core" LogosAPI. Prefer the operator's first
    // configured transport; fall back to the global default (LocalSocket).
    // Needed because the single-arg getClient() always uses the global default,
    // which hangs against a tcp-only module that never bound a LocalSocket.
    LogosAPIClient* moduleClient(const std::string& name) {
        // Copied out: setModuleTransports can rewrite the entry.
        std::string transportSetJson;
        {
            std::shared_lock<std::shared_mutex> g(configMutex());
            if (auto it = moduleTransportsMap().find(name);
                it != moduleTransportsMap().end())
                transportSetJson = it->second;
        }

        if (!transportSetJson.empty()) {
            const auto ts = logos::transportSetFromJsonString(transportSetJson);
            if (!ts.empty())
                return coreApi().getClient(QString::fromStdString(name), ts.front());
        }
        return coreApi().getClient(name);
    }

    LogosAPIClient* capabilityModuleClient() {
        return moduleClient("capability_module");
    }

    // Token authenticates the call. Best-effort; assumes capability_module loaded.
    void registerRestrictionRpc(const std::string& target,
                                const std::vector<std::string>& callers) {
        nlohmann::json args = nlohmann::json::array();
        args.push_back(TokenManager::instance().getToken(std::string("capability_module")));
        args.push_back(target);
        args.push_back(callers);

        nlohmann::json result = capabilityModuleClient()->invokeRemoteMethod(
            std::string("capability_module"),
            std::string("registerRestriction"),
            args);

        if (!result.is_boolean() || !result.get<bool>())
            spdlog::warn("Failed to register access restriction for target: {}", target);
        else
            spdlog::info("Registered access restriction for target: {} ({} allowed callers)",
                         target, callers.size());
    }

    // Explicit-policy restrictions, including targets not yet loaded (the
    // derived path covers only loaded ones).
    void pushAccessRestrictionsToCapabilityModule() {
        if (!registryInstance().isLoaded("capability_module"))
            return;
        // Copied out: the RPCs below must not run under configMutex.
        std::vector<LogosCore::AccessRestriction> restrictions;
        {
            std::shared_lock<std::shared_mutex> g(configMutex());
            const auto& policy = parsedEnforcePolicy();
            if (!policy)
                return;
            restrictions = policy->restrictions;
        }

        runOnOwner([restrictions]() {
            for (const auto& restriction : restrictions) {
                if (std::find(kExemptTargets.begin(), kExemptTargets.end(),
                              restriction.target) != kExemptTargets.end())
                    continue;
                registerRestrictionRpc(restriction.target, restriction.allowedCallers);
            }
        });
    }

    // A module may only call modules it declared as a dependency, so `target`'s
    // allowed callers are its loaded dependents plus the trusted set. Empty when
    // exempt or no enforce policy (fail-open); explicit policy overrides verbatim.
    std::vector<std::string> derivedAllowedCallersFor(const std::string& target) {
        if (std::find(kExemptTargets.begin(), kExemptTargets.end(), target)
                != kExemptTargets.end())
            return {};

        {
            std::shared_lock<std::shared_mutex> g(configMutex());
            const auto& policy = parsedEnforcePolicy();
            if (!policy)
                return {};

            for (const auto& r : policy->restrictions)
                if (r.target == target)
                    return r.allowedCallers;
        }

        // Deduped; no dependents => trusted only (deny-by-default for peers).
        std::vector<std::string> callers;
        std::unordered_set<std::string> seen;
        auto add = [&](const std::string& c) {
            if (seen.insert(c).second)
                callers.push_back(c);
        };
        for (const auto& d : registryInstance().moduleDependents(target, /*recursive=*/false))
            if (registryInstance().isLoaded(d))
                add(d);
        // Optional dependents are callers too. The DECLARATION is what grants
        // the right to call; whether the loader had to supply the target is a
        // separate question. Omitting them denies a declared call between two
        // loaded modules, and the caller sees a default value, not an error.
        for (const auto& d : registryInstance().moduleOptionalDependents(target))
            if (registryInstance().isLoaded(d))
                add(d);
        for (const auto& t : kTrustedCallers)
            add(t);
        return callers;
    }

    void pushDerivedRestrictionForTarget(const std::string& target) {
        if (!registryInstance().isLoaded("capability_module"))
            return;
        auto callers = derivedAllowedCallersFor(target);
        if (!callers.empty())
            registerRestrictionRpc(target, callers);
    }

    // On load/unload of `name`, re-push the targets whose caller set changed:
    // its declared dependencies, plus `name` itself.
    //
    // Each push reads the loaded set, so the pushes have to be ordered against
    // each other or the last word can come from a reader that ran before the
    // other module committed. The owner thread's queue is that order now — a
    // mutex here would be held across the dial, which is the edge runOnOwner
    // exists to remove.
    void refreshDerivedRestrictionsForDependenciesOf(const std::string& name) {
        if (!registryInstance().isLoaded("capability_module"))
            return;
        runOnOwner([name]() {
            for (const auto& dep : registryInstance().moduleDependencies(name, /*recursive=*/false))
                pushDerivedRestrictionForTarget(dep);
            for (const auto& dep : registryInstance().moduleOptionalDependencies(name))
                pushDerivedRestrictionForTarget(dep);
            pushDerivedRestrictionForTarget(name);
        });
    }

    void notifyCapabilityModule(const std::string& name, const std::string& token) {
        if (!registryInstance().isLoaded("capability_module"))
            return;

        // On the owner thread, which the 3-arg informModuleToken needs for a
        // second reason: it has no marshal of its own at the pinned protocol,
        // and a QtRO replica is thread-AFFINE, not merely non-reentrant.
        runOnOwner([name, token]() {
            const std::string capabilityModuleToken =
                TokenManager::instance().getToken(std::string("capability_module"));

            // INBOUND half of load-time identity: capability stores (name, token)
            // so authorize can name the caller from the presented token rather
            // than from a self-asserted fromModuleName.
            if (!capabilityModuleClient()->informModuleToken(
                    capabilityModuleToken, name, token)) {
                spdlog::warn("Failed to register token with capability module for: {}", name);
            }
        });
    }

    // ── THE modules_state FEED ───────────────────────────────────────────────
    //
    // The consumer end of ModuleStateObserver. Follows the capability_module
    // precedent above — one long-lived "core" LogosAPI, per-module transport
    // honoured — with three differences forced by where it runs:
    //
    //   1. ASYNC. registerRestrictionRpc is synchronous and gets away with it
    //      because it is rare and short. This runs on EVERY load, unload and
    //      crash, from the observer's flush. A synchronous RPC there would put
    //      a 20 s worst case on the load path.
    //   2. CHEAP NO-OP WHEN ABSENT, checked before the client is even fetched.
    //      A synchronous dial to an absent module cost Basecamp ~417 s of
    //      blocked GUI thread once already.
    //   3. THE SINK IS UNINSTALLED when modules_state goes away, so the
    //      observer buffers nothing rather than buffering into a sink that
    //      only drops.

    constexpr const char* kModulesState = "modules_state";

    // An empty optional reaches the wire as JSON null: an invalid QVariant
    // falls through every branch of qvariantToNlohmann and returns nullptr.
    QVariant optToVariant(const std::optional<std::string>& v) {
        return v.has_value() ? QVariant(QString::fromStdString(*v)) : QVariant();
    }

    QVariant optToVariant(const std::optional<int64_t>& v) {
        return v.has_value() ? QVariant(static_cast<qlonglong>(*v)) : QVariant();
    }

    LogosAPIClient* modulesStateClient() {
        return moduleClient(kModulesState);
    }

    // Observe readiness: arm a one-shot watch and return. Never waits -- rule 1
    // forbids blocking or dispatching under a load lock, and the callback lands
    // on this thread's event loop with no lock held.
    //
    // Only armed when a sink is installed; without one nothing consumes the
    // transition and each watch would hold a client and replica for nothing.
    void armReadinessWatch(const std::string& name,
                           std::optional<std::string> instanceId,
                           std::optional<int64_t> pid) {
        auto& observer = logos::ModuleStateObserver::instance();
        if (!observer.hasSink()) return;

        registryInstance().beginPublishWatch(name);
        const uint64_t epoch = registryInstance().loadEpoch(name);

        // The client and its replica are built by this call, so it decides
        // which thread owns them — the owner's, never the loading worker's.
        runOnOwner([name, instanceId, pid, epoch]() {
        moduleClient(name)->whenObjectAvailable(
            QString::fromStdString(name),
            [name, instanceId, pid, epoch](bool ready) {
                if (!ready) return;                                  // abandoned
                if (!registryInstance().markPublished(name, epoch))  // reloaded
                    return;
                auto& o = logos::ModuleStateObserver::instance();
                o.record(name, logos::module_state::kLoaded,
                         logos::module_state::kReady, instanceId, pid);
                o.flush();   // no ScopedModuleStateFlush in scope out here
            });
        });
    }

    // Everything the host knows, as a ModuleListing, for apply_snapshot.
    //
    // THE SEQ RULE, the one thing here easy to get wrong: every record seq AND
    // the listing seq come from the observer's single counter, listing drawn
    // LAST so it is >= every record. modules_state tombstones a pruned record
    // at the LISTING's seq, so a second counter makes that tombstone
    // unreachably high or trivially low.
    nlohmann::json buildSnapshotListing() {
        auto& observer = logos::ModuleStateObserver::instance();
        nlohmann::json records = nlohmann::json::array();

        for (const auto& info : registryInstance().allModulesInfo()) {
            const std::string name = info.value("name", std::string());
            if (name.empty())
                continue;

            const bool loaded = info.value("loaded", false);
            // Readiness must survive the snapshot. Snapshot records draw fresh
            // seqs, so they outrank every earlier transition -- reporting a
            // published module as merely `loaded` would clobber its `ready`
            // permanently, since the watch is one-shot and will not re-fire.
            // `published` is null when no watch is armed, hence the is_boolean
            // check: unknown readiness reports `loaded`, never `ready`.
            bool published = false;
            if (auto p = info.find("published");
                p != info.end() && p->is_boolean())
                published = p->get<bool>();

            // type/version are the module's OWN claims, read off the
            // metadata.json embedded in its plugin. Every step degrades to ""
            // rather than throwing: allModulesInfo reports a missing or
            // unparseable blob as null, and even a well-formed object
            // guarantees nothing about these two keys' types -- a snapshot
            // must not fail to build over a module that mis-declares itself.
            std::string type;
            std::string version;
            if (auto m = info.find("metadata"); m != info.end() && m->is_object()) {
                if (auto t = m->find("type"); t != m->end() && t->is_string())
                    type = t->get<std::string>();
                if (auto v = m->find("version"); v != m->end() && v->is_string())
                    version = v->get<std::string>();
            }

            nlohmann::json rec = nlohmann::json::object();
            rec["module"]       = name;
            rec["state"]        = !loaded ? logos::module_state::kUnloaded
                                : published ? logos::module_state::kReady
                                            : logos::module_state::kLoaded;
            rec["path"]         = info.value("path", std::string());
            rec["type"]         = std::move(type);
            rec["version"]      = std::move(version);
            rec["dependencies"] = info.value("dependencies", nlohmann::json::array());
            rec["dependents"]   = info.value("dependents", nlohmann::json::array());
            rec["loadedAt"]     = info.value("loaded_at", static_cast<int64_t>(0));
            rec["seq"]          = observer.nextSeq();
            // instance/pid/reason are OMITTED rather than nulled, matching the
            // generated encoder; the registry carries none of them.
            records.push_back(std::move(rec));
        }

        nlohmann::json listing = nlohmann::json::object();
        listing["modules"] = std::move(records);
        // FALSE, and it is a claim worth defending: `partial` means the scan
        // SKIPPED something, and discoverInstalledModules drops what it cannot
        // read before it ever enters the registry. Anything missing is not
        // withheld — it is unknown to the host, which is a complete view from
        // modules_state's side.
        listing["partial"] = false;
        listing["seq"]     = observer.nextSeq();
        return listing;
    }

    void pushSnapshot() {
        if (!registryInstance().isLoaded(kModulesState))
            return;
        nlohmann::json args = nlohmann::json::array();
        args.push_back(buildSnapshotListing());
        // Synchronous unlike the deltas, deliberately: this runs from
        // whenObjectAvailable's callback, not the load path, so no lock is held
        // and nothing waits on it. The nlohmann overload has no async twin, and
        // a struct argument is easier to build as JSON than as a QVariantMap.
        const nlohmann::json ok = modulesStateClient()->invokeRemoteMethod(
            std::string(kModulesState), std::string("apply_snapshot"), args);
        if (!ok.is_boolean() || !ok.get<bool>())
            spdlog::warn("modules_state refused the startup snapshot");
        else
            spdlog::info("Pushed module snapshot to modules_state");
    }

    void pushTransitions(const std::vector<logos::ModuleTransition>& batch) {
        // Before the client is fetched: see note 2 above.
        if (!registryInstance().isLoaded(kModulesState))
            return;

        runOnOwner([batch]() {
        LogosAPIClient* client = modulesStateClient();
        for (const logos::ModuleTransition& t : batch) {
            QVariantList args;
            args << QString::fromStdString(t.module)
                 << optToVariant(t.instance)
                 << optToVariant(t.pid)
                 << QString::fromStdString(t.oldState)
                 << QString::fromStdString(t.newState)
                 << optToVariant(t.reason)
                 << QVariant(static_cast<qulonglong>(t.seq));

            // Fire and forget: the next snapshot re-establishes the whole
            // picture, and blocking the load path to find out would be the
            // failure this shape exists to avoid.
            client->invokeRemoteMethodAsync(
                QString::fromUtf8(kModulesState),
                QStringLiteral("note_transition"),
                args,
                [](QVariant) {});
        }
        });
    }

    // Called once modules_state is loaded. The snapshot waits for it to
    // PUBLISH, which is later than "loaded" (~390 ms cold);
    // whenObjectAvailable waits without failing fast or burning the acquire
    // timeout on this thread.
    void enableModulesStateFeed() {
        logos::ModuleStateObserver::instance().setSink(&pushTransitions);
        runOnOwner([]() {
        modulesStateClient()->whenObjectAvailable(
            QString::fromUtf8(kModulesState),
            [](bool ready) {
                if (ready)
                    pushSnapshot();
                else
                    spdlog::warn("modules_state never became available; no snapshot pushed");
            });
        });
    }

    void disableModulesStateFeed() {
        // Clearing the sink is what makes the observer free again: record()
        // early-outs when nothing is installed.
        logos::ModuleStateObserver::instance().setSink({});
    }

    // Callers hold fleetMutex(). Takes `name`'s own lock, so two callers of one
    // module are one load and two callers of different modules are two.
    bool loadModuleInternal(const char* moduleName) {
        std::string name(moduleName);

        if (!registryInstance().isKnown(name)) {
            spdlog::warn("Cannot load unknown module: {}", name);
            return false;
        }

        // Wraps the already-loaded check below too: outside it, two callers
        // racing on one name would both see "not loaded" and both spawn.
        std::lock_guard<std::mutex> moduleGuard(moduleMutex(name));

        // "Already loaded" is a successful no-op, not a failure.
        // Callers (basecamp's PluginLoader::loadCoreDependencies,
        // logoscore-cli, etc.) use loadModule as "ensure loaded";
        // returning false here aborted UI-plugin loads whose core
        // dependency had been pre-loaded at startup (e.g. clicking
        // the package-manager launcher after basecamp pre-loaded
        // `package_manager`).
        if (registryInstance().isLoaded(name)) {
            spdlog::debug("Module already loaded (no-op): {}", name);
            return true;
        }

        std::string modPath = registryInstance().modulePath(name);

        // Build a descriptor for the loader to inspect.
        LogosCore::ModuleDescriptor desc;
        desc.name        = name;
        desc.path        = modPath;
        // WHICH CONTAINER RUNS THIS MODULE is decided here, and by the
        // artifact rather than by a flag: a Bare module image can only run
        // in-process, and a Qt plugin can only run in a subprocess host. The
        // registry recorded the shape at discovery.
        const std::string moduleFormat = registryInstance().moduleFormat(name);
        desc.format      = moduleFormat.empty() ? std::string("qt-plugin") : moduleFormat;

        // ── the container assertion ────────────────────────────────────
        // The operator said which container everything here must run in;
        // the artifact says which one it CAN run in. Where they disagree the
        // load is refused, because the alternative is running the module in
        // the container the operator explicitly said not to and reporting
        // success. See logos_core_set_container_policy.
        if (const char* mismatch =
                containerPolicyMismatch(containerPolicyValue(), name, desc.format)) {
            spdlog::error("Refusing to load module {}: {}", name, mismatch);
            logos::ModuleStateObserver::instance().record(
                name, logos::module_state::kUnloaded, logos::module_state::kError,
                std::nullopt, std::nullopt, mismatch);
            return false;
        }
        desc.dependencies = registryInstance().moduleDependencies(name);
        desc.modulesDirs  = registryInstance().modulesDirs();

        // Hoisted out of the block below so the observer can carry it. The
        // instance id is the host's PERSISTENCE identity and is stable across
        // load/unload cycles (ResolveMode::ReuseOrCreate), which is exactly why
        // a consumer needs the pid too: instance cannot tell you a module died
        // and came back, and pid can.
        std::optional<std::string> instanceId;

        if (!persistenceBasePath().empty()) {
            auto info = ModuleLib::InstancePersistence::resolveInstance(
                persistenceBasePath(), name);
            desc.instancePersistencePath = info.persistencePath;
            if (!info.instanceId.empty())
                instanceId = info.instanceId;
        }

        // The attempt starts here — everything above was a cheap reject that
        // never touched the module. `loading` is the state the drafts fold into
        // `loaded`; we keep it because it is the only way a consumer can tell
        // "being brought up" from "up", and a load that hangs is otherwise
        // indistinguishable from one that never started.
        logos::ModuleStateObserver::instance().record(
            name, logos::module_state::kUnloaded, logos::module_state::kLoading,
            instanceId);

        // Per-module transport set, if the daemon registered one before
        // calling load. The loader threads it through to the child via
        // a CLI argument so the child's LogosAPIProvider binds the right
        // listeners. Modules without an entry inherit the global default.
        {
            std::shared_lock<std::shared_mutex> g(configMutex());
            if (auto it = moduleTransportsMap().find(name);
                it != moduleTransportsMap().end()) {
                desc.transportSetJson = it->second;
            }
        }

        // ── Protocol-version load gate ─────────────────────────────────
        // Read the module's embedded metadata without loading it and apply
        // the one compatibility rule: equal logos-protocol MAJOR loads,
        // different MAJOR is refused, a missing stamp (pre-protocol module)
        // loads permissively with a warning.
        std::string moduleProtocolVersion;
        if (auto meta = ModuleLib::LogosModule::extractMetadata(modPath)) {
            // While we have it, hand the full metadata to the loader.
            desc.rawMetadata = nlohmann::json::parse(
                meta->rawMetadataJson, nullptr, /*allow_exceptions=*/false);
            if (desc.rawMetadata.is_discarded())
                desc.rawMetadata = nlohmann::json::object();
            if (auto it = desc.rawMetadata.find("logos_protocol_version");
                it != desc.rawMetadata.end() && it->is_string())
                moduleProtocolVersion = it->get<std::string>();
        }
        const auto gate = LogosCore::evaluateProtocolGate(
            moduleProtocolVersion, LOGOS_PROTOCOL_VERSION_MAJOR);
        switch (gate.decision) {
        case LogosCore::ProtocolGateDecision::Refuse:
            spdlog::error(
                "Refusing to load module {}: built against logos-protocol {} "
                "(major {}), this host speaks major {} ({}) — incompatible "
                "protocol majors",
                name, moduleProtocolVersion, gate.moduleMajor,
                LOGOS_PROTOCOL_VERSION_MAJOR, LOGOS_PROTOCOL_VERSION_STRING);
            logos::ModuleStateObserver::instance().record(
                name, logos::module_state::kLoading, logos::module_state::kError,
                instanceId, std::nullopt,
                "incompatible logos-protocol major: module " + moduleProtocolVersion);
            return false;
        case LogosCore::ProtocolGateDecision::AllowLegacy:
            // A Bare module is EXPECTED to land here: this stamp is read out of
            // Qt plugin metadata, and a Bare module carries none — that is what
            // "bare" means. It answers the same question over
            // logos_module_get_protocol_version instead, and InProcContainer
            // asks it there, with the same equal-MAJOR rule, before the module
            // runs. Warning here would report a gate that is not missing, only
            // asked elsewhere.
            //
            // A WEB module lands here too, and for a stronger reason: a page is
            // not a binary at all, so there is no image to stamp. Its
            // compatibility is decided by the message set its SDK speaks, which
            // the transport rejects on the wire. Warning here would fire for
            // every web module ever loaded.
            if (desc.format != "bare" && desc.format != "web")
                spdlog::warn(
                    "Module {} carries no usable logos_protocol_version "
                    "(pre-protocol build) — loading permissively",
                    name);
            break;
        case LogosCore::ProtocolGateDecision::Allow:
            spdlog::debug("Module {} protocol version {} compatible with host {}",
                          name, moduleProtocolVersion,
                          LOGOS_PROTOCOL_VERSION_STRING);
            break;
        }

        // ── Dependency version-range gate ──────────────────────────────
        // Direct edges only: every module is gated as it loads, so a chain is
        // covered edge by edge. The signer pin each entry may also carry is
        // NOT checked — lgpm verifies a package's signature at install time
        // (PackageManagerLib::verifyPackageSignature) and keeps no evidence on
        // disk afterwards, so there is nothing here to check it against.
        const auto depGate = ModuleManager::dependencyGateFor(name);
        if (depGate.decision == LogosCore::DependencyGateDecision::Refuse) {
            spdlog::error("Refusing to load module {}: {}", name, depGate.reason);
            logos::ModuleStateObserver::instance().record(
                name, logos::module_state::kLoading, logos::module_state::kError,
                instanceId, std::nullopt, depGate.reason);
            return false;
        }
        if (depGate.decision == LogosCore::DependencyGateDecision::Allow) {
            spdlog::debug("Module {} dependency constraints satisfied", name);
        }

        auto loader = loaderRegistry().select(desc);
        if (!loader) {
            spdlog::warn("No loader available to load module: {}", name);
            logos::ModuleStateObserver::instance().record(
                name, logos::module_state::kLoading, logos::module_state::kError,
                instanceId, std::nullopt, "no loader available for this module format");
            return false;
        }

        // Fires on the container's BACKGROUND asio thread, for both an orderly
        // unload and a module that died. consumeExpectedExit() is what tells
        // them apart; see its definition.
        //
        // This is the one seam that flushes inline: it is not under
        // a load lock, so there is no lock to get out from under, and a crash
        // is the transition a consumer most needs promptly.
        auto onTerminated = [](const std::string& n) {
            // Marks the module unloaded and, if a load is in flight, hands the
            // termination to it: that load reports the failure, with the child's
            // own reason, and its markLoaded is called off.
            if (recordTerminationDuringLoad(n))
                return;

            auto& observer = logos::ModuleStateObserver::instance();
            if (consumeExpectedExit(n)) {
                observer.record(n, logos::module_state::kStopping,
                                logos::module_state::kUnloaded);
                observer.flush();
                return;
            }

            observer.record(n, logos::module_state::kLoaded,
                            logos::module_state::kError, std::nullopt,
                            std::nullopt, "module exited without being asked to");
            // Flushed BEFORE the supervisor is asked, so a consumer watching
            // the feed sees the failure and then whatever the policy does about
            // it, in that order.
            observer.flush();
            superviseUnexpectedExit(n);
        };

        // Past here a child process may exist, so a termination belongs to this
        // attempt rather than to whatever the module was doing before.
        beginLoadAttempt(name);

        LogosCore::LoadedModuleHandle handle;
        bool started;
        {
            std::lock_guard<std::mutex> g(spawnMutex());
            started = loader->load(desc, onTerminated, handle);
        }
        if (!started) {
            abandonLoadAttempt(name);
            logos::ModuleStateObserver::instance().record(
                name, logos::module_state::kLoading, logos::module_state::kError,
                instanceId, std::nullopt, "loader failed to start the module");
            return false;
        }

        // Read before the handle is moved into the registry below.
        const std::optional<int64_t> pid =
            handle.pid >= 0 ? std::optional<int64_t>(handle.pid) : std::nullopt;

        // OUTBOUND half of load-time identity: mint a root token, send it into
        // the child, and register it locally under the module's name.
        std::string authToken = boost::uuids::to_string(boost::uuids::random_generator()());

        if (!loader->sendToken(name, authToken)) {
            // We are about to terminate it deliberately, so announce the intent
            // BEFORE calling terminate() — otherwise onTerminated, which may
            // already be running on the asio thread, reports this as a crash.
            markExitExpected(name);
            loader->terminate(name);
            // Same reason unloadModuleInternal() consumes below: the
            // container drops the callback for a teardown it performed, so this
            // mark has nothing left to consume it. Before the attempt is
            // abandoned, so a termination racing us is still swallowed by it and
            // this load stays the only thing that reports.
            consumeExpectedExit(name);
            abandonLoadAttempt(name);
            logos::ModuleStateObserver::instance().record(
                name, logos::module_state::kLoading, logos::module_state::kError,
                instanceId, pid, "failed to deliver the module's auth token");
            return false;
        }

        // SPAWNED IS NOT LOADED. load() proved only that the OS made a
        // process; whether the plugin behind it loaded is a fact only the child
        // has. Claiming it here without asking is what reported a module whose
        // plugin never loaded as loaded, with the failure surfacing hops away.
        const LogosCore::LoadOutcome outcome = loader->awaitLoad(
            name, hostStaysSilent().load() ? kSilentHostGrace : kLoadVerdictTimeout);

        if (outcome.verdict == LogosCore::LoadVerdict::Failed) {
            spdlog::error("Failed to load module {}: {}", name, outcome.reason);
            loader->terminate(name);   // no-op when the child is already gone
            abandonLoadAttempt(name);
            logos::ModuleStateObserver::instance().record(
                name, logos::module_state::kLoading, logos::module_state::kError,
                instanceId, pid, outcome.reason);
            return false;
        }

        if (outcome.verdict == LogosCore::LoadVerdict::Unknown) {
            hostStaysSilent().store(true);
            spdlog::warn("Module {} never reported whether its plugin loaded; "
                         "treating it as loaded. Its module host predates the "
                         "load-status line, so a failed load here is only "
                         "detectable if the process dies.", name);
        }

        // Settles a death that arrived while we waited together with the
        // registry write — see commitLoad.
        if (!commitLoad(name, loader, std::move(handle))) {
            const char* reason = "the module process exited while it was loading";
            spdlog::error("Failed to load module {}: {}", name, reason);
            logos::ModuleStateObserver::instance().record(
                name, logos::module_state::kLoading, logos::module_state::kError,
                instanceId, pid, reason);
            return false;
        }

        TokenManager::instance().saveToken(name, authToken);

        notifyCapabilityModule(name, authToken);

        refreshDerivedRestrictionsForDependenciesOf(name);

        spdlog::info("Module loaded: {}", name);
        logos::ModuleStateObserver::instance().record(
            name, logos::module_state::kLoading, logos::module_state::kLoaded,
            instanceId, pid);

        // The feed can only exist once its consumer does.
        if (name == kModulesState)
            enableModulesStateFeed();

        // After the feed: modules_state installs the sink as it loads, and
        // armReadinessWatch is a no-op without one, so it must see its own sink.
        armReadinessWatch(name, instanceId, pid);

        return true;
    }

    // Callers hold fleetMutex(): shared for a single unload, exclusive for the
    // cascade, which needs one span so a load cannot interleave between the
    // dependents and the target. Takes `name`'s lock like the load path does.
    // The load a SUPERVISOR asks for, which differs from the operator's
    // ModuleManager::loadModule in exactly one way: it does not clear the
    // module's death history. Clearing it there is what gives an operator who
    // loads a module by hand a fresh budget; doing it here would make every
    // restart the first one and turn the budget into no budget at all.
    bool restartModule(const std::string& name) {
        // BEFORE the lock guard, so it is destroyed after it. See rule 1.
        logos::ScopedModuleStateFlush stateFlusher;
        ScopedLoadEntry entry;
        if (entry.reentrant) return refuseReentrantLoad(name);
        std::shared_lock<std::shared_mutex> fleet(fleetMutex());
        return loadModuleInternal(name.c_str());
    }

    // A module died without being asked to. Ask the policy what to do about it
    // and say, in the log, what was decided and why — a module that came back
    // on its own and a module that stayed down are both surprising to whoever
    // is reading, and neither should have to be inferred.
    void superviseUnexpectedExit(const std::string& name) {
        auto& supervisor = LogosCore::ModuleSupervisor::instance();
        const LogosCore::SupervisionPolicy policy = supervisor.policy();
        const LogosCore::ModuleSupervisor::Verdict verdict =
            supervisor.onUnexpectedExit(name);

        using Decision = LogosCore::ModuleSupervisor::Decision;
        if (verdict.decision == Decision::Disabled) return;

        if (verdict.decision == Decision::BudgetExhausted) {
            spdlog::error("Module {} has died {} times in the last {} ms, past a "
                          "supervision budget of {}. Leaving it down: a module "
                          "that will not stay up needs an operator, not another "
                          "restart.",
                          name, verdict.attempt, policy.window.count(),
                          policy.maxRestarts);
            return;
        }

        spdlog::warn("Module {} exited without being asked to; loading it again "
                     "in {} ms (restart {} of {})",
                     name, verdict.delay.count(), verdict.attempt,
                     policy.maxRestarts);

        // NOT ON THIS THREAD. The death is announced from wherever the
        // container noticed it — an asio thread, a socket pump — and the load
        // path must not run there. See RestartScheduler.
        LogosCore::restartScheduler()(verdict.delay, [name]() {
            if (!registryInstance().isKnown(name)) {
                spdlog::warn("Not restarting {}: it is no longer a known module",
                             name);
                return;
            }
            if (!restartModule(name))
                spdlog::error("Restarting module {} failed", name);
        });
    }

    bool unloadModuleInternal(const std::string& name) {
        std::lock_guard<std::mutex> moduleGuard(moduleMutex(name));

        if (!registryInstance().isLoaded(name)) {
            spdlog::warn("Cannot unload module (not loaded): {}", name);
            return false;
        }

        logos::ModuleStateObserver::instance().record(
            name, logos::module_state::kLoaded, logos::module_state::kStopping);

        auto loader = registryInstance().loaderFor(name);
        if (loader) {
            if (!loader->hasModule(name)) {
                spdlog::warn("No module entry found for module: {}", name);
                // Nothing was torn down, so the module is still where it was.
                // Walk `stopping` back rather than leaving a consumer watching
                // a teardown that never happens.
                logos::ModuleStateObserver::instance().record(
                    name, logos::module_state::kStopping, logos::module_state::kLoaded,
                    std::nullopt, std::nullopt, "no module entry found; teardown not started");
                return false;
            }
            // Announce BEFORE terminate(): onTerminated can fire on the asio
            // thread before terminate() even returns here.
            markExitExpected(name);
            loader->terminate(name);
        } else {
            // Fallback: module was loaded via markLoaded(name) directly (test
            // scenarios or external setup), so no loader was recorded. Ask the
            // registered loaders to terminate it by name — no specific container
            // is named here.
            markExitExpected(name);
            if (!loaderRegistry().terminate(name)) {
                spdlog::warn("No live module entry found for module: {}", name);
                consumeExpectedExit(name);
                logos::ModuleStateObserver::instance().record(
                    name, logos::module_state::kStopping, logos::module_state::kLoaded,
                    std::nullopt, std::nullopt, "no live module entry; teardown not started");
                return false;
            }
        }

        registryInstance().markUnloaded(name);

        // The operator has taken this module out. Whatever it did before that
        // describes a run that is over, and must not spend the budget of the
        // next one.
        LogosCore::ModuleSupervisor::instance().forget(name);

        // markUnloaded keeps the dependency edges, so this still resolves them.
        refreshDerivedRestrictionsForDependenciesOf(name);

        spdlog::info("Module unloaded: {}", name);

        // THE MARK IS THE GUARD: the observer is stateless, so it cannot spot
        // a duplicate `stopping -> unloaded` the way it drops old == new, and
        // emitting twice would reach modules_state as two transitions with
        // different seqs — two identical events for one teardown. If
        // onTerminated already ran it consumed the mark; if the mark is still
        // here no callback fired (loaders that terminate synchronously) and
        // without this the module sits in `stopping` forever.
        if (consumeExpectedExit(name)) {
            logos::ModuleStateObserver::instance().record(
                name, logos::module_state::kStopping, logos::module_state::kUnloaded);
        }
        if (name == kModulesState)
            disableModulesStateFeed();
        return true;
    }
}

namespace ModuleManager {

    ModuleRegistry& registry() {
        return registryInstance();
    }

    void anchorCoreApi() {
        // Single-threaded at startup, so constructing here cannot race the
        // static's guard; the marshal only matters for a host that starts off
        // the Qt main thread.
        logos::runOnQtMainThread([]() {
            LogosAPI& api = coreApi();
            // The Native container admits each in-process module as a consumer
            // over this same trusted channel, so it needs the object rather
            // than a copy of the name. Done HERE, on the owner thread and
            // before any load can run, because admitConsumer's
            // informModuleToken must travel core's channel and the container
            // has no other way to reach it.
            loaderRegistry();   // builds the containers on first touch
            if (auto& c = inProcContainer())
                c->setHostApi(&api);
            // Same hand-off for the Web container: a page is published under
            // its own identity on this same trusted channel.
            if (auto& w = webContainer())
                w->setHostApi(&api);
        });
    }

    LogosCore::ModuleLoaderRegistry& loaders() {
        return loaderRegistry();
    }

    void setModulesDir(const char* modules_dir) {
        assert(modules_dir != nullptr);
        registryInstance().setModulesDir(std::string(modules_dir));
    }

    void addModulesDir(const char* modules_dir) {
        assert(modules_dir != nullptr);
        registryInstance().addModulesDir(std::string(modules_dir));
    }

    void setPersistenceBasePath(const char* path) {
        assert(path != nullptr);
        persistenceBasePath() = std::string(path);
    }

    void setModuleTransports(const std::string& moduleName,
                             const std::string& transportSetJson) {
        // Same mutex as loadModuleInternal's read of the map, and
        // moduleClient()'s. Without this, an operator can race with an
        // in-flight load and the child gets garbled JSON (or sees an empty
        // transport set after the operator overwrote what it was about to
        // read).
        std::unique_lock<std::shared_mutex> g(configMutex());
        if (transportSetJson.empty())
            moduleTransportsMap().erase(moduleName);
        else
            moduleTransportsMap()[moduleName] = transportSetJson;
    }

    // THE deny-by-default switch. `mode: "enforce"` is the whole flag: it is
    // what turns the derived restrictions on (derivedAllowedCallersFor
    // returns {} without it, so core registers nothing and capability_module
    // leaves every target open). Anything else — no policy, empty policy,
    // unparseable policy, a different mode — is OFF, i.e. exactly the behaviour
    // of a host that never calls this at all.
    //
    // Every branch says out loud which side it landed on. Enforcement that
    // silently failed to arm is the dangerous outcome: it looks identical to
    // enforcement that is working and simply has nothing to deny, so an
    // operator who mistyped `"mode":"enforced"` would otherwise get a
    // wide-open runtime and a clean log.
    void setAccessPolicy(const std::string& policyJson) {
        std::unique_lock<std::shared_mutex> g(configMutex());  // guards the read at push time
        accessPolicyJson() = policyJson;
        // Cache the parse only in enforce mode; malformed/non-enforce stays empty.
        parsedEnforcePolicy().reset();

        if (policyJson.empty()) {
            spdlog::info("Inter-module access enforcement is OFF (no access policy set): "
                         "any loaded module may call any other");
            return;
        }

        auto parsed = LogosCore::parseAccessPolicy(policyJson);
        if (!parsed) {
            spdlog::warn("logos_core_set_access_policy: policy is not valid JSON — "
                         "inter-module access enforcement stays OFF");
            return;
        }
        if (!parsed->enforce()) {
            spdlog::warn("logos_core_set_access_policy: mode is \"{}\", not \"enforce\" — "
                         "inter-module access enforcement stays OFF ({} restriction(s) "
                         "parsed but not registered)",
                         parsed->mode, parsed->restrictions.size());
            return;
        }

        spdlog::info("Inter-module access enforcement is ON (mode=enforce): deny-by-default — "
                     "a module may only call the modules it declares as dependencies; "
                     "{} explicit restriction(s) override the derived allow-list",
                     parsed->restrictions.size());
        parsedEnforcePolicy() = std::move(parsed);
    }

    bool setContainerPolicy(const std::string& policy) {
        const std::string wanted = policy.empty() ? std::string("auto") : policy;
        if (wanted != "auto" && wanted != "inproc" && wanted != "subprocess"
            && wanted != "web") {
            spdlog::error("Ignoring unknown container policy '{}' "
                          "(expected auto | inproc | subprocess | web)", policy);
            return false;
        }
        containerPolicyValue() = wanted;
        spdlog::info("Container policy: {}", wanted);
        return true;
    }

    std::string containerPolicy() {
        return containerPolicyValue();
    }

    void discoverInstalledModules() {
        registryInstance().discoverInstalledModules();
    }

    std::string processModule(const std::string& modulePath) {
        return registryInstance().processModule(modulePath);
    }

    char* processModuleCStr(const char* modulePath) {
        std::string path(modulePath);

        std::string moduleName = registryInstance().processModule(path);
        if (moduleName.empty()) {
            spdlog::warn("Failed to process module: {}", path);
            return nullptr;
        }

        char* result = new char[moduleName.size() + 1];
        strcpy(result, moduleName.c_str());
        return result;
    }

    char* addEmbeddedBareModuleCStr(const char* metadataJson, const char* imagePath) {
        std::string moduleName =
            registryInstance().addEmbeddedBareModule(std::string(metadataJson),
                                                     std::string(imagePath));
        if (moduleName.empty())
            return nullptr;

        char* result = new char[moduleName.size() + 1];
        strcpy(result, moduleName.c_str());
        return result;
    }

    bool loadModule(const char* moduleName) {
        // AN OPERATOR LOADING A MODULE IS AN INTERVENTION, and it re-arms
        // supervision: the deaths recorded against this name happened to a run
        // that whoever is calling has decided to replace. (The supervisor's own
        // restart goes through restartModule, which does not do this.)
        if (moduleName) LogosCore::ModuleSupervisor::instance().forget(moduleName);
        // BEFORE the lock guard, so it is destroyed after it. See rule 1.
        logos::ScopedModuleStateFlush stateFlusher;
        ScopedLoadEntry entry;
        if (entry.reentrant) return refuseReentrantLoad(moduleName);
        std::shared_lock<std::shared_mutex> fleet(fleetMutex());
        return loadModuleInternal(moduleName);
    }

    // The default for `optionalLoad` lives on the declaration in
    // module_manager.h; repeating it here would not compile.
    bool loadModuleWithDependencies(const char* moduleName,
                                    DependencyResolver::OptionalLoad optionalLoad) {
        // The same intervention as loadModule's, for the same reason.
        if (moduleName) LogosCore::ModuleSupervisor::instance().forget(moduleName);
        // BEFORE the lock guard, so it is destroyed after it. See rule 1.
        logos::ScopedModuleStateFlush stateFlusher;
        ScopedLoadEntry entry;
        if (entry.reentrant) return refuseReentrantLoad(moduleName);
        // Shared, not exclusive: the chain below takes each module's own lock
        // in turn and releases it before the next, so two chains sharing a
        // dependency make the second one a no-op instead of a second child.
        std::shared_lock<std::shared_mutex> fleet(fleetMutex());

        std::string name(moduleName);

        std::vector<std::string> requested;
        requested.push_back(name);

        auto resolved = DependencyResolver::resolve(
            requested,
            [](const std::string& n) { return registryInstance().isKnown(n); },
            [](const std::string& n) { return registryInstance().moduleDependencies(n); },
            [](const std::string& n) { return registryInstance().moduleOptionalDependencies(n); },
            optionalLoad
        );

        // Treat missing dependencies and cycles as hard failures.
        // The header contract (logos_core.h) promises "returns 0 when
        // dependency resolution fails", so we must not proceed with a
        // partial order that silently dropped unknown deps or cycled.
        if (!resolved.ok()) {
            spdlog::warn("Cannot resolve dependencies for: {}", name);
            return false;
        }

        bool nameFound = false;
        for (const auto& r : resolved.order) {
            if (r == name) { nameFound = true; break; }
        }

        if (resolved.order.empty() || !nameFound) {
            spdlog::warn("Cannot resolve dependencies for: {}", name);
            return false;
        }

        bool allSucceeded = true;
        for (const std::string& moduleName : resolved.order) {
            if (loadModuleInternal(moduleName.c_str()))
                continue;

            // A best-effort optional dependency is in this order because it was
            // INSTALLED, not because anything needs it. Its failure is the case
            // the caller asked to tolerate, so it must not reach the return
            // value — the header's contract is "all REQUIRED modules ended up
            // loaded", and this module is required by nobody.
            if (resolved.isBestEffort(moduleName)) {
                spdlog::warn("Optional dependency failed to load, continuing without it: {}",
                             moduleName);
                continue;
            }

            spdlog::warn("Failed to load module: {}", moduleName);
            allSucceeded = false;
        }

        return allSucceeded;
    }

    // Auto-load modules_state, if it is installed.
    //
    // OPTIONAL BY DESIGN: absent, this returns false and nothing else changes.
    // The observer has no sink, record() early-outs, and liblogos pays nothing.
    //
    // LOADED LAST, AFTER DISCOVERY AND capability_module, AND THAT IS FINE. The
    // membership edges from discoverInstalledModules and capability_module's own
    // load were recorded with no sink installed, so they were dropped. The
    // snapshot pushed when this module publishes carries the whole picture,
    // which is exactly what apply_snapshot exists for -- modules_state cannot
    // be first, so it must not need to be.
    bool initializeModulesState() {
        // BEFORE the lock guard, so it is destroyed after it. See rule 1.
        logos::ScopedModuleStateFlush stateFlusher;
        ScopedLoadEntry entry;
        if (entry.reentrant) return refuseReentrantLoad("modules_state");
        std::shared_lock<std::shared_mutex> fleet(fleetMutex());

        if (!registryInstance().isKnown("modules_state")) {
            spdlog::debug("modules_state is not installed; lifecycle feed stays off");
            return false;
        }

        if (!loadModuleInternal("modules_state")) {
            spdlog::warn("Failed to load modules_state; lifecycle feed stays off");
            return false;
        }
        return true;
    }

    bool initializeCapabilityModule() {
        // BEFORE the lock guard, so it is destroyed after it. See rule 1.
        logos::ScopedModuleStateFlush stateFlusher;
        ScopedLoadEntry entry;
        if (entry.reentrant) return refuseReentrantLoad("capability_module");
        std::shared_lock<std::shared_mutex> fleet(fleetMutex());

        if (!registryInstance().isKnown("capability_module"))
            return false;

        if (!loadModuleInternal("capability_module")) {
            spdlog::warn("Failed to load capability module");
            return false;
        }

        // Register restrictions before any other module can call out: explicit
        // entries, then derived for anything already loaded (usually nothing —
        // only the exempt capability_module is up here).
        pushAccessRestrictionsToCapabilityModule();
        runOnOwner([]() {
            for (const auto& loaded : registryInstance().loadedModuleNames())
                pushDerivedRestrictionForTarget(loaded);
        });

        return true;
    }

    bool unloadModule(const char* moduleName) {
        // BEFORE the lock guard, so it is destroyed after it. See rule 1.
        logos::ScopedModuleStateFlush stateFlusher;
        ScopedLoadEntry entry;
        if (entry.reentrant) {
            spdlog::error("Refusing a re-entrant unload of {}: this thread is "
                          "already inside a load or unload", moduleName);
            return false;
        }
        std::shared_lock<std::shared_mutex> fleet(fleetMutex());
        return unloadModuleInternal(std::string(moduleName));
    }

    bool unloadModuleWithDependents(const char* moduleName) {
        // BEFORE the lock guard, so it is destroyed after it. See rule 1.
        logos::ScopedModuleStateFlush stateFlusher;
        ScopedLoadEntry entry;
        if (entry.reentrant) {
            spdlog::error("Refusing a re-entrant unload cascade of {}: this "
                          "thread is already inside a load or unload", moduleName);
            return false;
        }
        // EXCLUSIVE: the whole cascade is one span, so a load cannot slip in
        // between tearing down the dependents and the target.
        std::unique_lock<std::shared_mutex> fleet(fleetMutex());

        std::string name(moduleName);

        if (!registryInstance().isLoaded(name)) {
            spdlog::warn("Cannot unload module (not loaded): {}", name);
            return false;
        }

        // Build the set of modules that need to come down: the target plus
        // every currently-loaded recursive dependent. Materialise the loaded
        // set into a hash once so the membership check below is O(1).
        std::vector<std::string> loadedNames = registryInstance().loadedModuleNames();
        std::unordered_set<std::string> loaded(loadedNames.begin(), loadedNames.end());

        // Reverse dependency walk against the in-process graph. ModuleRegistry
        // keeps ModuleInfo::dependents in sync with ModuleInfo::dependencies
        // across every discovery pass, so we don't need a disk-backed query.
        std::vector<std::string> dependents = registryInstance().moduleDependents(name, /*recursive=*/true);

        std::vector<std::string> teardownSet;
        std::unordered_set<std::string> teardownSetMembers;
        teardownSet.push_back(name);
        teardownSetMembers.insert(name);
        for (const std::string& d : dependents) {
            if (loaded.count(d) && teardownSetMembers.insert(d).second)
                teardownSet.push_back(d);
        }

        // Order leaves-first: resolve load-order for the teardown set, then
        // reverse. Dependents come down before the modules they depend on.
        // Teardown is best-effort — we use .order and ignore resolution errors
        // (missing deps / cycles) because we need to tear down what we can.
        std::vector<std::string> loadOrder = DependencyResolver::resolve(
            teardownSet,
            [](const std::string& n) { return registryInstance().isKnown(n); },
            [](const std::string& n) { return registryInstance().moduleDependencies(n); }
        ).order;
        std::vector<std::string> teardownOrder;
        std::unordered_set<std::string> teardownOrderMembers;
        for (auto it = loadOrder.rbegin(); it != loadOrder.rend(); ++it) {
            if (teardownSetMembers.count(*it) && teardownOrderMembers.insert(*it).second)
                teardownOrder.push_back(*it);
        }

        // Safety net: any members not seen by the resolver (shouldn't happen,
        // but don't silently skip them) go to the end.
        for (const std::string& n : teardownSet) {
            if (teardownOrderMembers.insert(n).second)
                teardownOrder.push_back(n);
        }

        bool allSucceeded = true;
        for (const std::string& n : teardownOrder) {
            if (!registryInstance().isLoaded(n)) continue;
            if (!unloadModuleInternal(n)) {
                spdlog::warn("Failed to unload module during cascade: {}", n);
                allSucceeded = false;
            }
        }

        return allSucceeded;
    }

    void terminateAll() {
        // BEFORE the lock guard, so it is destroyed after it. See rule 1.
        logos::ScopedModuleStateFlush stateFlusher;
        ScopedLoadEntry entry;
        if (entry.reentrant) {
            spdlog::error("Refusing a re-entrant fleet teardown: this thread is "
                          "already inside a load or unload");
            return;
        }
        // EXCLUSIVE: markAllLoadedExitsExpected needs the loaded set to hold
        // still, and every load and unload holds this shared for its span.
        std::unique_lock<std::shared_mutex> fleet(fleetMutex());
        // Announce before tearing down, or every module reports as a crash.
        markAllLoadedExitsExpected();
        loaderRegistry().terminateAll();
        clearExpectedExits();
        registryInstance().clearLoaded();
    }

    void clear() {
        // BEFORE the lock guard, so it is destroyed after it. See rule 1.
        logos::ScopedModuleStateFlush stateFlusher;
        ScopedLoadEntry entry;
        if (entry.reentrant) {
            spdlog::error("Refusing a re-entrant fleet teardown: this thread is "
                          "already inside a load or unload");
            return;
        }
        // EXCLUSIVE: markAllLoadedExitsExpected needs the loaded set to hold
        // still, and every load and unload holds this shared for its span.
        std::unique_lock<std::shared_mutex> fleet(fleetMutex());
        // Announce before tearing down, or every module reports as a crash.
        markAllLoadedExitsExpected();
        loaderRegistry().terminateAll();
        clearExpectedExits();
        registryInstance().clear();
        // Per-module transport overrides are part of the manager's
        // mutable state — without clearing them here, a daemon
        // restart in the same process (or a unit test that calls
        // clear() between scenarios) would inherit the previous
        // run's transport map and bind unexpected ports.

        {
            std::unique_lock<std::shared_mutex> cfg(configMutex());
            moduleTransportsMap().clear();
            accessPolicyJson().clear();  // same rationale — don't leak across restarts
            parsedEnforcePolicy().reset();
        }
        // Same rationale again: the next run may have a host that does report.
        hostStaysSilent().store(false);
        // ...and the next run's modules have not crashed yet.
        LogosCore::ModuleSupervisor::instance().clear();
    }

    char** getLoadedModulesCStr() {
        return toNullTerminatedArray(registryInstance().loadedModuleNames());
    }

    char** getKnownModulesCStr() {
        std::vector<std::string> known = registryInstance().knownModuleNames();
        if (known.empty()) {
            spdlog::warn("No known modules to return");
        }
        return toNullTerminatedArray(known);
    }

    bool isModuleLoaded(const std::string& name) {
        return registryInstance().isLoaded(name);
    }

    std::unordered_map<std::string, int64_t> getModuleProcessIds() {
        return loaderRegistry().getAllPids();
    }

    LogosCore::DependencyGateResult dependencyGateFor(const std::string& name) {
        return LogosCore::evaluateDependencyGate(
            registryInstance().moduleDependencyEntries(name),
            [](const std::string& dep) { return registryInstance().moduleVersion(dep); });
    }

    std::vector<std::string> resolveDependencies(const std::vector<std::string>& requestedModules) {
        return DependencyResolver::resolve(
            requestedModules,
            [](const std::string& name) { return registryInstance().isKnown(name); },
            [](const std::string& name) { return registryInstance().moduleDependencies(name); },
            [](const std::string& n) { return registryInstance().moduleOptionalDependencies(n); }
        ).order;
    }

    DependencyResolver::ResolveResult resolveDependenciesBestEffort(
        const std::vector<std::string>& requestedModules) {
        return DependencyResolver::resolve(
            requestedModules,
            [](const std::string& name) { return registryInstance().isKnown(name); },
            [](const std::string& name) { return registryInstance().moduleDependencies(name); },
            [](const std::string& n) { return registryInstance().moduleOptionalDependencies(n); },
            DependencyResolver::OptionalLoad::BestEffort
        );
    }

    std::string optionalLoadReportJson(const std::string& moduleName) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& s : resolveDependenciesBestEffort({moduleName}).skippedOptional) {
            nlohmann::json entry;
            entry["module"] = s.module;
            entry["named_by"] = s.namedBy;
            entry["reason"] = s.reason;
            // Present only when there is one to name: "not_installed" is about
            // the optional dependency itself and has no third module to blame.
            if (!s.detail.empty())
                entry["missing"] = s.detail;
            out.push_back(std::move(entry));
        }
        return out.dump();
    }

    char* optionalLoadReportCStr(const char* moduleName) {
        std::string json = optionalLoadReportJson(std::string(moduleName));
        char* result = new char[json.size() + 1];
        strcpy(result, json.c_str());
        return result;
    }

    std::vector<std::string> getDependencies(const std::string& name, bool recursive) {
        std::vector<std::string> deps = registryInstance().moduleDependencies(name, recursive);
        std::vector<std::string> knownDeps;
        knownDeps.reserve(deps.size());
        for (const std::string& dep : deps) {
            if (registryInstance().isKnown(dep))
                knownDeps.push_back(dep);
        }
        return knownDeps;
    }

    std::vector<std::string> getDependents(const std::string& name, bool recursive) {
        return registryInstance().moduleDependents(name, recursive);
    }

    char** getDependenciesCStr(const char* name, bool recursive) {
        return toNullTerminatedArray(
            getDependencies(std::string(name), recursive));
    }

    std::vector<std::string> getOptionalDependencies(const std::string& name) {
        std::vector<std::string> deps = registryInstance().moduleOptionalDependencies(name);
        std::vector<std::string> knownDeps;
        knownDeps.reserve(deps.size());
        for (const std::string& dep : deps) {
            if (registryInstance().isKnown(dep))
                knownDeps.push_back(dep);
        }
        return knownDeps;
    }

    char** getOptionalDependenciesCStr(const char* name) {
        return toNullTerminatedArray(getOptionalDependencies(std::string(name)));
    }

    char** getDependentsCStr(const char* name, bool recursive) {
        return toNullTerminatedArray(
            getDependents(std::string(name), recursive));
    }

    std::string getModulesInfoJson() {
        return registryInstance().allModulesInfo().dump();
    }

    char* getModulesInfoCStr() {
        std::string json = getModulesInfoJson();
        char* result = new char[json.size() + 1];
        strcpy(result, json.c_str());
        return result;
    }

    std::vector<std::string> computeDerivedAllowedCallers(const std::string& target) {
        return derivedAllowedCallersFor(target);
    }

    // No fleet lock: the real caller (pushSnapshot, from whenObjectAvailable)
    // holds no lock either, and allModulesInfo takes the registry's own.
    std::string buildSnapshotListingJson() {
        return buildSnapshotListing().dump();
    }
}
