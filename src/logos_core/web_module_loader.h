#ifndef WEB_MODULE_LOADER_H
#define WEB_MODULE_LOADER_H

#include <logos_module_loader/module_format_loader.h>

namespace LogosCore {

// The format half of the Web container's pair: "this module is a page".
//
// Same shape, and same reasoning, as BareModuleFormatLoader. A format loader's
// job in the subprocess world is to name a HOST BINARY and build its command
// line; a page has neither. The host binary IS the webview the container opens,
// and there is no command line to cross — everything a subprocess module is
// handed on argv (the transport set, the persistence path, the host-services
// grant) is in the descriptor the container reads directly.
//
// A separate class rather than a special case inside the container because
// CompositeModuleLoader is what ModuleLoaderRegistry understands and it wants a
// pair — and because pairing a `web` artifact with a DIFFERENT container is a
// real possibility: the Wasm host (slice 26) runs the same variant in a Worker
// with no webview around it.
class WebModuleFormatLoader : public ModuleFormatLoader {
public:
    std::string id() const override { return "web"; }

    bool canHandle(const ModuleDescriptor& desc) const override;

    // The module's own entry document. CompositeModuleLoader treats an empty
    // host binary as "cannot load", so this cannot answer ""; of the non-empty
    // answers available, the page being opened is the only honest one. The
    // container ignores the value and reads desc.path.
    std::string resolveHostBinary(const ModuleDescriptor& desc) const override;

    // Empty, always. There is no process to configure.
    std::vector<std::string> buildArguments(const ModuleDescriptor& desc) const override;
};

} // namespace LogosCore

#endif // WEB_MODULE_LOADER_H
