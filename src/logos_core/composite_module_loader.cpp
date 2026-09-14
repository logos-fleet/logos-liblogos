#include "composite_module_loader.h"

namespace LogosCore {

CompositeModuleLoader::CompositeModuleLoader(std::shared_ptr<ModuleContainer> container,
                                             std::shared_ptr<ModuleFormatLoader> loader)
    : container_(std::move(container))
    , loader_(std::move(loader))
{}

std::string CompositeModuleLoader::id() const
{
    return loader_->id() + "+" + container_->id();
}

bool CompositeModuleLoader::canHandle(const ModuleDescriptor& desc) const
{
    return loader_->canHandle(desc) && container_->canHandle(desc);
}

bool CompositeModuleLoader::load(const ModuleDescriptor& desc,
                                 std::function<void(const std::string& name)> onTerminated,
                                 LoadedModuleHandle& out)
{
    std::string host = loader_->resolveHostBinary(desc);
    if (host.empty())
        return false;

    auto args = loader_->buildArguments(desc);
    return container_->launch(desc, host, args, std::move(onTerminated), out);
}

bool CompositeModuleLoader::sendToken(const std::string& name, const std::string& token)
{
    return container_->sendToken(name, token);
}

LoadOutcome CompositeModuleLoader::awaitLoad(const std::string& name,
                                             std::chrono::milliseconds timeout)
{
    return container_->awaitLoad(name, timeout);
}

void CompositeModuleLoader::terminate(const std::string& name)
{
    container_->terminate(name);
}

void CompositeModuleLoader::terminateAll()
{
    container_->terminateAll();
}

bool CompositeModuleLoader::hasModule(const std::string& name) const
{
    return container_->hasModule(name);
}

std::optional<int64_t> CompositeModuleLoader::pid(const std::string& name) const
{
    return container_->pid(name);
}

std::unordered_map<std::string, int64_t> CompositeModuleLoader::getAllPids() const
{
    return container_->getAllPids();
}

std::unordered_map<std::string, ModuleResourceUsage>
CompositeModuleLoader::getAllResourceUsage() const
{
    // Asked for, not required. ModuleContainer is the logos-container contract
    // package's interface and is implemented outside this repo too; measuring a
    // module without a pid is something only a container that runs modules
    // in-process can do at all. One that cannot answers nothing, which leaves
    // its modules to process-stats exactly as before.
    if (const auto* measurable = dynamic_cast<const ResourceMeasurable*>(container_.get()))
        return measurable->getAllResourceUsage();
    return {};
}

} // namespace LogosCore
