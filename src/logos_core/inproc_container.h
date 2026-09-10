#ifndef INPROC_CONTAINER_H
#define INPROC_CONTAINER_H

#include "bare_module_abi.h"

#include <logos_container/module_container.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class LogosAPI;

namespace LogosCore {

class BareModuleGlue;

// THE NATIVE CONTAINER.
//
// A ModuleContainer that runs a module INSIDE the host process: it dlopens a
// Bare module privately, drives it through the one generic host glue
// (BareModuleGlue), and publishes it under the module's name on the host's own
// transport, so every existing consumer — core_service, capability_module,
// another module, the CLI — reaches it exactly as it reaches a subprocess
// module. Nothing above the container learns that there is no process.
//
// It exists because iOS has no fork/exec a store will accept and Google Play
// forbids downloading a .so, so the Bundled set has to run in the app image
// (ADR 0003, ADR 0006). Desktop gets it too: `logoscore --container inproc` is
// how a module developer exercises the phone's container without a phone.
//
// pid IS -1, AND THAT IS THE CONTRACT, not a missing value.
// LoadedModuleHandle documents -1 as the not-process-based sentinel, and every
// consumer that reports pids (logos_core_get_modules_info, the stats snapshot)
// already treats it as one. An in-process module has no process of its own to
// name, and answering with the HOST's pid would be a lie a supervisor could act
// on — it would make killing "the module" kill the app.
//
// WHAT THIS CONTAINER DOES NOT GIVE YOU is isolation. A subprocess module that
// segfaults takes down a process the host can notice and restart; a Bare module
// that segfaults takes down the host. That is inherent to the store rules this
// container exists to satisfy and is stated here so nobody has to discover it:
// the Native container buys reachability on a phone, not crash containment.
// Crash containment on a Store shell is the Web container's job (ADR 0005).
class InProcContainer : public ModuleContainer {
public:
    InProcContainer();
    ~InProcContainer() override;

    std::string id() const override { return "inproc"; }

    // A Bare module, and only ever explicitly. The format is stamped by the
    // core when the module's own manifest says `"format": "bare"` — this
    // container never claims a Qt plugin, so registering it alongside the
    // subprocess loader cannot change what an existing module does.
    bool canHandle(const ModuleDescriptor& desc) const override;

    bool launch(const ModuleDescriptor& desc,
                const std::string& hostBinary,
                const std::vector<std::string>& args,
                std::function<void(const std::string& name)> onTerminated,
                LoadedModuleHandle& out) override;

    bool sendToken(const std::string& name, const std::string& token) override;

    // For the Native container, launch() IS the verdict — and that is the one
    // interesting difference from a subprocess. A subprocess launch proves only
    // that the OS made a process; whether the plugin behind it loaded is a fact
    // only the child has, which is what the load-status line exists to carry.
    // Here the image is opened, its ABI resolved and its provider published
    // before launch() returns, so there is nothing left to wait for and no
    // deadline that can expire. Answering Unknown instead would make every
    // in-process load log the "never reported whether its plugin loaded"
    // warning that exists for hosts predating the status line.
    LoadOutcome awaitLoad(const std::string& name,
                          std::chrono::milliseconds timeout) override;

    void terminate(const std::string& name) override;
    void terminateAll() override;
    bool hasModule(const std::string& name) const override;

    std::optional<int64_t> pid(const std::string& name) const override;
    std::unordered_map<std::string, int64_t> getAllPids() const override;

    // The pid every in-process module reports. Named rather than spelled -1 at
    // each site so the sentinel has one definition to grep for.
    static constexpr int64_t kInProcPid = -1;

    // How long terminate() waits for a module that asked for a grace period.
    static constexpr int kUnloadGraceMs = 2000;

    // The host's LogosAPI, used as the trusted channel that admits each
    // in-process module as a consumer (logos::admitConsumer). Injected rather
    // than constructed here so tests can drive the container with no core
    // running: with no host API the container still loads and drives a module,
    // it just does not publish it.
    void setHostApi(LogosAPI* hostApi);

private:
    struct Instance;

    // Guards m_modules. Held across launch/terminate, which are already
    // serialised one-per-module by ModuleManager but may interleave for
    // different modules.
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::unique_ptr<Instance>> m_modules;

    // Instances whose dispatch thread would not stop. Kept alive for the life of
    // the process on purpose: the thread is still executing code in the image
    // and still holds a pointer to the glue, so destroying either is a
    // use-after-free and dlclose is an unmap of running code. See terminate().
    std::vector<std::unique_ptr<Instance>> m_abandoned;

    LogosAPI* m_hostApi = nullptr;
};

} // namespace LogosCore

#endif // INPROC_CONTAINER_H
