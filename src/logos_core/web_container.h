#ifndef WEB_CONTAINER_H
#define WEB_CONTAINER_H

#include "web_module_view.h"

#include <logos_container/module_container.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

class LogosAPI;

namespace LogosCore {

class WebModuleGlue;

// THE WEB CONTAINER.
//
// A ModuleContainer that runs a module INSIDE A WEBVIEW: one view per module,
// hidden for a headless module, with the web transport's frames crossing the
// view's message bridge. The page is an ordinary provider — it answers Call,
// Methods, Subscribe and Token exactly as a subprocess module does over TCP —
// and this container is what makes it reachable by name: it publishes a
// WebModuleGlue under the module's own identity on the host's transport, so
// every consumer already in the system reaches a Web module unchanged.
//
// It exists because the App Store and Google Play both forbid downloading
// executable code but permit an interpreter running downloaded scripts (ADR
// 0003), so a Downloaded module on a Store shell is a web variant or it is
// nothing. Desktop gets it too, and gets it FIRST: `logoscore --container web`
// is how a module developer exercises the phone's container without a phone.
//
// WHAT THIS CONTAINER BUYS THAT THE NATIVE ONE CANNOT is crash containment. A
// Bare module that segfaults takes the host down with it; a page that dies
// takes down a renderer the host outlives, and the container reports it. That
// is the trade the two containers make against each other and it is why both
// exist.
//
// IDENTITY IS STRUCTURAL. A frame is attributed to the CHANNEL it arrived on,
// and a channel belongs to exactly one view, so a token stolen inside one page
// cannot be used to speak as another module (ADR 0005). Nothing in this class
// trusts a name a page asserts about itself.
class WebContainer : public ModuleContainer {
public:
    WebContainer();
    ~WebContainer() override;

    std::string id() const override { return "web"; }

    // A `web` module, and only ever explicitly. The format is stamped by the
    // core when discovery finds a page where a plugin would be; this container
    // never claims a Qt plugin or a Bare image, so registering it alongside
    // them cannot change what an existing module does.
    bool canHandle(const ModuleDescriptor& desc) const override;

    bool launch(const ModuleDescriptor& desc,
                const std::string& hostBinary,
                const std::vector<std::string>& args,
                std::function<void(const std::string& name)> onTerminated,
                LoadedModuleHandle& out) override;

    bool sendToken(const std::string& name, const std::string& token) override;

    // LAUNCH IS NOT THE VERDICT HERE, and that is the interesting difference
    // from the Native container. Opening a view proves the backend made one; it
    // says nothing about whether the page finished loading and published its
    // module, which is a fact only the page has. So this asks it — one
    // introspection round trip, retried until the caller's deadline — exactly
    // as the subprocess path waits for the child's load-status line.
    LoadOutcome awaitLoad(const std::string& name,
                          std::chrono::milliseconds timeout) override;

    void terminate(const std::string& name) override;
    void terminateAll() override;
    bool hasModule(const std::string& name) const override;

    std::optional<int64_t> pid(const std::string& name) const override;
    std::unordered_map<std::string, int64_t> getAllPids() const override;

    // What a module whose view names no process reports. Same sentinel, and
    // the same contract, as InProcContainer::kInProcPid: -1 is "no process of
    // its own", never "unknown".
    static constexpr int64_t kNoPid = -1;

    // How long a view is given to load its page and publish its module before
    // awaitLoad gives up, when the caller names no shorter deadline.
    static constexpr int kPageReadyTimeoutMs = 30000;

    // The host's LogosAPI, used as the trusted channel that publishes each web
    // module under its own identity. Injected rather than constructed here so
    // tests can drive the container with no core running: with no host API the
    // container still opens the view and relays to it, it just does not
    // publish. Mirrors InProcContainer::setHostApi.
    void setHostApi(LogosAPI* hostApi);

    // The glue for a loaded module, or nullptr. Exists so a test can drive the
    // relay without a core, and so the container's own awaitLoad has one place
    // to ask the page whether it is serving.
    WebModuleGlue* glueFor(const std::string& name) const;

private:
    struct Instance;

    // Announce a module's death exactly once, whatever killed it.
    void announceTermination(const std::string& name);

    // Lift a module out of m_modules, or nullptr when it is not there. Both
    // teardown paths go through it, which is what makes them exactly-once.
    std::unique_ptr<Instance> takeInstance(const std::string& name);

    // Unpublish, drop the relay, stop the peer, close the page. Called with
    // m_mutex released — see the definition.
    static void tearDown(Instance& instance);

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::unique_ptr<Instance>> m_modules;
    LogosAPI* m_hostApi = nullptr;
};

} // namespace LogosCore

#endif // WEB_CONTAINER_H
