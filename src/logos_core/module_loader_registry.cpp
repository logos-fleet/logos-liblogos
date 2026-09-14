#include "module_loader_registry.h"

namespace LogosCore {

void ModuleLoaderRegistry::registerLoader(std::shared_ptr<ModuleLoader> loader)
{
    std::lock_guard lock(m_mutex);
    m_loaders.push_back(std::move(loader));
}

std::shared_ptr<ModuleLoader> ModuleLoaderRegistry::select(const ModuleDescriptor& desc) const
{
    std::lock_guard lock(m_mutex);

    // Explicit id override: caller pinned a specific loader id.
    if (desc.loaderConfig.contains("id")) {
        std::string requestedId = desc.loaderConfig.at("id").get<std::string>();
        for (const auto& loader : m_loaders) {
            if (loader->id() == requestedId)
                return loader;
        }
        // Unknown explicit id — don't fall through to canHandle.
        return nullptr;
    }

    // Format/capability-based: first loader that accepts this descriptor.
    for (const auto& loader : m_loaders) {
        if (loader->canHandle(desc))
            return loader;
    }
    return nullptr;
}

void ModuleLoaderRegistry::terminateAll()
{
    std::lock_guard lock(m_mutex);
    for (const auto& loader : m_loaders)
        loader->terminateAll();
}

bool ModuleLoaderRegistry::terminate(const std::string& name)
{
    std::lock_guard lock(m_mutex);
    for (const auto& loader : m_loaders) {
        if (loader->hasModule(name)) {
            loader->terminate(name);
            return true;
        }
    }
    return false;
}

std::unordered_map<std::string, int64_t> ModuleLoaderRegistry::getAllPids() const
{
    std::lock_guard lock(m_mutex);
    std::unordered_map<std::string, int64_t> result;
    for (const auto& loader : m_loaders) {
        auto pids = loader->getAllPids();
        result.insert(pids.begin(), pids.end());
    }
    return result;
}

std::unordered_map<std::string, ModuleResourceUsage>
ModuleLoaderRegistry::getAllResourceUsage() const
{
    std::lock_guard lock(m_mutex);
    std::unordered_map<std::string, ModuleResourceUsage> result;
    for (const auto& loader : m_loaders) {
        auto usage = loader->getAllResourceUsage();
        result.insert(usage.begin(), usage.end());
    }
    return result;
}

void ModuleLoaderRegistry::clearForTests()
{
    std::lock_guard lock(m_mutex);
    m_loaders.clear();
}

} // namespace LogosCore
