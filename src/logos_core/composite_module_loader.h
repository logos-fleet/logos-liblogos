#ifndef COMPOSITE_MODULE_LOADER_H
#define COMPOSITE_MODULE_LOADER_H

#include "module_loader.h"
#include <logos_container/module_container.h>
#include <logos_module_loader/module_format_loader.h>
#include <memory>

namespace LogosCore {

// Pairs a ModuleContainer (where/how to run) with a ModuleFormatLoader (what to
// load) and presents the combined result as a single ModuleLoader — the
// interface that ModuleLoaderRegistry and ModuleManager already understand.
class CompositeModuleLoader : public ModuleLoader {
public:
    CompositeModuleLoader(std::shared_ptr<ModuleContainer> container,
                          std::shared_ptr<ModuleFormatLoader> loader);

    std::string id() const override;
    bool canHandle(const ModuleDescriptor& desc) const override;

    bool load(const ModuleDescriptor& desc,
              std::function<void(const std::string& name)> onTerminated,
              LoadedModuleHandle& out) override;

    bool sendToken(const std::string& name, const std::string& token) override;
    LoadOutcome awaitLoad(const std::string& name,
                          std::chrono::milliseconds timeout) override;
    void terminate(const std::string& name) override;
    void terminateAll() override;
    bool hasModule(const std::string& name) const override;
    std::optional<int64_t> pid(const std::string& name) const override;
    std::unordered_map<std::string, int64_t> getAllPids() const override;
    std::unordered_map<std::string, ModuleResourceUsage> getAllResourceUsage() const override;

    ModuleContainer& container() { return *container_; }
    const ModuleContainer& container() const { return *container_; }

private:
    std::shared_ptr<ModuleContainer> container_;
    std::shared_ptr<ModuleFormatLoader> loader_;
};

} // namespace LogosCore

#endif // COMPOSITE_MODULE_LOADER_H
