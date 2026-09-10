#ifndef BARE_MODULE_LOADER_H
#define BARE_MODULE_LOADER_H

#include <logos_module_loader/module_format_loader.h>

namespace LogosCore {

// The format half of the Native container's pair: "this module is a Bare
// module image".
//
// A ModuleFormatLoader's job in the subprocess world is to name a HOST BINARY
// and build its command line. An in-process module has neither: the host binary
// IS the running image and there is no command line to cross. So this loader
// answers the two questions the seam asks in the only way that is true for a
// Bare module — the image itself is what gets "run", and it needs no arguments
// — and every bit of the actual work happens in InProcContainer, which is where
// it belongs.
//
// It is a separate class rather than a special case inside the container
// because CompositeModuleLoader is what ModuleLoaderRegistry understands, and
// it wants a pair. Pairing this with a DIFFERENT container is a real
// possibility (a Bare module driven by a Wasm host, slice 26), which is the
// second reason it is not folded in.
class BareModuleFormatLoader : public ModuleFormatLoader {
public:
    std::string id() const override { return "bare"; }

    bool canHandle(const ModuleDescriptor& desc) const override;

    // The module's own path. CompositeModuleLoader treats an empty host binary
    // as "cannot load", so this cannot answer "" — and of the non-empty answers
    // available, the image being loaded is the only honest one. The container
    // ignores the value and reads desc.path.
    std::string resolveHostBinary(const ModuleDescriptor& desc) const override;

    // Empty, always. There is no process to configure: everything the
    // subprocess loader passes over argv (the transport set, the persistence
    // path, the host-services grant) is already in the descriptor the container
    // reads directly.
    std::vector<std::string> buildArguments(const ModuleDescriptor& desc) const override;
};

} // namespace LogosCore

#endif // BARE_MODULE_LOADER_H
