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
#include <vector>

class LogosAPI;

namespace LogosCore {

class WebCallRouter;
class WebHostRoutes;
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

    // How long a view is given to load its page and publish its module.
    //
    // A FLOOR, NOT A DEFAULT: awaitLoad takes the larger of this and what the
    // caller asked for. ModuleManager asks for 10 s, which is calibrated for a
    // subprocess that dlopens a plugin and prints a line; a page is a browser
    // cold-starting a WebAssembly image, and a `ui_qml` `web` variant brings up
    // the app's 26 MB bundled QML runtime AND its own backend image before its
    // SDK can publish anything. On a machine with no GPU that is tens of
    // seconds with nothing wrong.
    //
    // AND IT IS A MULTIPLE OF THE CONTRACT QUERY'S OWN TIMEOUT, which is the
    // part that is not obvious. awaitLoad polls by ASKING the page for its
    // interface, and a page that is not serving yet does not answer "no" — it
    // does not answer at all, so the query costs WebModuleGlue::kCallTimeoutMs
    // before the poll loop gets its turn back. A budget of one such timeout
    // therefore buys exactly ONE attempt, made before the page could possibly
    // be ready. This buys four.
    static constexpr int kPageReadyTimeoutMs = 120000;

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

    // WHAT A PAGE MAY REACH, for the module loaded next.
    //
    // The container builds routes over each module's own LogosAPI, which is the
    // right answer for every real host and no answer at all for a test driving
    // the container with no core. Set this and the container hands the routes
    // to the router instead of building its own; the pointer is borrowed and
    // must outlive every module loaded after it is set.
    //
    // Deliberately NOT a per-module argument: the descriptor is what the loader
    // produced and the routes are what the host provides, and folding one into
    // the other would make every caller of launch() name something only the
    // container knows how to build.
    void setHostRoutes(WebHostRoutes* routes);

private:
    struct Instance;

    // Why a module stopped, which is the only thing the two retirement paths
    // disagree about: an operator unloaded it, or its page went away.
    enum class Retirement { Unloaded, PageLost };

    // Take a module out of m_modules, tear it down and announce it — EXACTLY
    // ONCE, whichever path arrives first. The single retirement path there is:
    // a second death notification for the same page finds nothing to take and
    // says nothing.
    void retire(const std::string& name, Retirement why);

    // Announce a module's death exactly once, whatever killed it. Hops to the
    // Qt main thread first (runOnQtMainThread) when it is not already on it --
    // see the definition, where the reason is a segfault rather than a style
    // preference.
    void announceTermination(const std::string& name);

    // Lift a module out of m_modules, or nullptr when it is not there. The one
    // place a module stops being loaded, which is what makes retire()
    // exactly-once.
    std::unique_ptr<Instance> takeInstance(const std::string& name);

    // Unpublish, drop the relay, stop the peer, close the page. Called with
    // m_mutex released — see the definition.
    static void tearDown(Instance& instance);

    // Park a LOST page's parts for destruction later. A lost page is noticed
    // from wherever the backend happened to be — including from inside the
    // module's own QtRO dispatch, three Qt frames down. See the definition,
    // where the alternative is a segfault in the host.
    void deferTearDown(std::unique_ptr<Instance> instance);

    // Destroy every parked module that is no longer being waited on, and come
    // back for the rest.
    void sweepRetired();

    // Ask to be called back on the next turn of the Qt main thread's event
    // loop, which is where sweepRetired runs.
    void armSweep();

    // Everything still parked, waited on or not. The destructor's, because the
    // container is going away and there is no later.
    void tearDownAllRetired();

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::unique_ptr<Instance>> m_modules;
    // Pages that died while a call into them was waiting, still whole, until
    // that wait has unwound.
    std::vector<std::unique_ptr<Instance>> m_retired;
    // Expires with this container, so a sweep that is already armed can tell
    // whether there is still anything to sweep for.
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
    LogosAPI* m_hostApi = nullptr;
    WebHostRoutes* m_hostRoutes = nullptr;
};

} // namespace LogosCore

#endif // WEB_CONTAINER_H
