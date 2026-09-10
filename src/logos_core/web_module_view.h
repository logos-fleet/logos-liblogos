#ifndef WEB_MODULE_VIEW_H
#define WEB_MODULE_VIEW_H

#include "logos_core.h"   // LOGOS_CORE_EXPORT

#include <message_channel.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace LogosCore {

// What the Web container is handed when it needs somewhere to run a page.
struct WebModuleViewRequest {
    std::string moduleName;
    // The module's entry document, absolute. This is `main` as the manifest
    // resolved it — an .html file for every web variant the builder emits.
    std::string entryPath;
    // The directory the entry lives in. Everything the page may load is under
    // it: the directory IS the package (lgx::resolveMain enforces that at
    // install time), so a backend that serves the page over a custom scheme has
    // its document root here and needs no second policy.
    std::string moduleDir;
};

// ONE WEBVIEW, OWNED BY THE CONTAINER.
//
// This is the whole of what the Web container needs from a webview, and it is
// deliberately smaller than one: no navigation, no DOM, no rendering. A page is
// a message channel that can die. Everything else about it — WebEngine on
// desktop, WKWebView on iOS, Android's WebView — is the backend's business and
// none of the container's.
//
// It is an interface rather than a Qt WebEngine class for the reason ADR 0005
// gives the transport the same treatment: liblogos is linked into a headless
// CLI, a desktop shell and (slice 32) a static iOS core, and exactly one of
// those three can host a Chromium. A container that named QWebEngineView would
// put a browser in all of them.
class WebModuleView {
public:
    virtual ~WebModuleView() = default;

    // The channel the module's frames cross, bound to the page's message
    // bridge (QWebChannel on desktop, a scheme handler on iOS — ADR 0005).
    // Never null while the view is alive.
    virtual logos::web::MessageChannelPtr channel() const = 0;

    // The OS process the page runs in, when the backend has one to name. A
    // Chromium renderer does; an interpreter embedded in this process does not,
    // and answers nullopt rather than the host's pid — see the same rule on
    // InProcContainer::kInProcPid.
    virtual std::optional<int64_t> pid() const { return std::nullopt; }

    // Called at most once, when the page goes away for any reason: the renderer
    // was killed, the bridge closed, the backend tore it down. Installed by the
    // container before it publishes, so a page that dies during startup is
    // still reported.
    virtual void setOnDied(std::function<void()> callback) = 0;

    virtual bool isAlive() const = 0;
};

// The build-selected backend. A `std::function` rather than the link-time
// `makeContainer()` seam because a webview backend is a RUNTIME choice on
// desktop: `logoscore --container web` wants one, and every other logoscore
// invocation must not pay for a Chromium it never opens.
//
// UNSET BY DEFAULT, and the container says so rather than pretending: with no
// backend installed a `web` module reports a load error naming the missing
// bridge, which is the honest answer for a process that has no webview in it.
// Mirrors logos::web::setMessageChannelFactory, deliberately.
using WebModuleViewFactory =
    std::function<std::unique_ptr<WebModuleView>(const WebModuleViewRequest&)>;

// EXPORTED, because the caller is not in this library: a host installs its
// backend from its own binary (logoscore does it when `--container web` is
// asserted), which is also why this header is one of the two liblogos installs.
LOGOS_CORE_EXPORT void setWebModuleViewFactory(WebModuleViewFactory factory);
LOGOS_CORE_EXPORT WebModuleViewFactory webModuleViewFactory();

} // namespace LogosCore

#endif // WEB_MODULE_VIEW_H
