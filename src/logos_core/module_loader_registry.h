#ifndef MODULE_LOADER_REGISTRY_H
#define MODULE_LOADER_REGISTRY_H

#include "module_loader.h"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace LogosCore {

// Holds all registered ModuleLoader instances and selects one for a given descriptor.
// Thread-safe (all operations protected by an internal mutex).
class ModuleLoaderRegistry {
public:
    // Register a loader. Loaders are consulted in registration order when
    // no explicit loaderConfig["id"] is present.
    void registerLoader(std::shared_ptr<ModuleLoader> loader);

    // Select a loader for the descriptor.
    // Selection order:
    //   1. If desc.loaderConfig["id"] is set, return the matching loader by id.
    //      Returns nullptr if the id is unknown (does not fall through to canHandle).
    //   2. Otherwise, return the first registered loader whose canHandle(desc) is true.
    //   3. Returns nullptr if no loader matches.
    std::shared_ptr<ModuleLoader> select(const ModuleDescriptor& desc) const;

    // Fan-out terminateAll() to every registered loader.
    void terminateAll();

    // Terminate a single module by name: calls terminate(name) on the first
    // registered loader that reports hasModule(name). Returns true if one
    // matched. Used by the unload path when no loader was recorded for the
    // module (e.g. loaded via markLoaded(name) directly).
    bool terminate(const std::string& name);

    // Aggregate getAllPids() across all loaders. Later-registered loaders win on
    // name collision (should not happen in practice).
    std::unordered_map<std::string, int64_t> getAllPids() const;

    // Aggregate getAllResourceUsage() the same way. Only loaders whose modules
    // have no pid contribute anything.
    std::unordered_map<std::string, ModuleResourceUsage> getAllResourceUsage() const;

    // Testing hook: remove all loaders so a test can install a FakeModuleLoader
    // without triggering any real Qt subprocess side effects.
    void clearForTests();

private:
    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<ModuleLoader>> m_loaders;
};

} // namespace LogosCore

#endif // MODULE_LOADER_REGISTRY_H
