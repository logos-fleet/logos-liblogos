#ifndef MODULE_REGISTRY_H
#define MODULE_REGISTRY_H

#include "dependency_gate.h"
#include "module_loader.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>
#include <shared_mutex>
// InstalledPackage — the package-manager's view of a scanned module, which is
// the only metadata a Bare module has (it carries none of its own).
#include <package_manager_lib.h>

namespace logos {

// Allowlist validator for an untrusted module name. The name comes verbatim
// from plugin metadata and becomes the registry map key, the LogosAPI RPC
// target, and the instance-persistence directory segment
// (basePath + "/" + name + "/" + instanceId). Rule: non-empty, <= 64 bytes,
// every byte in [A-Za-z0-9_-] — so '/', '\\', '.', "..", NUL and whitespace
// are all rejected, preventing path traversal (CWE-22) and key collision.
// Enforced at the trust boundary in ModuleRegistry::processModuleInternal.
bool isValidModuleName(const std::string& name);

}  // namespace logos

struct ModuleInfo {
    std::string path;
    // The module's full embedded metadata as a compact JSON string, read once
    // at discovery time via ModuleLib::LogosModule (no plugin instantiation).
    // Empty when the plugin exposes no readable metadata.
    std::string metadataJson;
    // The module's own version, from that embedded metadata. Empty when the
    // plugin carries no version stamp; a dependent's range is evaluated
    // against it.
    std::string version;
    // Declared dependency edges with whatever constraints each entry carried
    // (a bare-name entry constrains nothing). Every graph consumer uses the
    // name-only view from moduleDependencies().
    std::vector<LogosCore::ModuleDependency> dependencies;
    // Direct reverse edges — names of modules whose `dependencies` list
    // includes this module. Kept in sync with `dependencies` across every
    // graph mutation by ModuleRegistry itself; callers never populate it
    // directly. Use ModuleRegistry::moduleDependents() for transitive walks.
    std::vector<std::string> dependents;
    // The SECOND edge set (metadata.json#optional_dependencies): concrete
    // modules this one can call but does not require. Carries the same
    // constraints as `dependencies` — an installer resolves both the same way;
    // only the loader differs.
    //
    // Deliberately NOT merged into `dependencies`: the load closure, the
    // teardown cascade and the missing-dependency verdict all read that one,
    // and every one of them must ignore this one.
    std::vector<LogosCore::ModuleDependency> optionalDependencies;
    std::vector<std::string> optionalDependents;
    // Which SHAPE this module's main file is, and therefore which container
    // can run it. Empty (the overwhelming default) means a Qt plugin, the only
    // shape that existed before the Native container; "bare" is a Bare module
    // image — no Qt plugin metadata, the module-impl C ABI instead, run
    // in-process by InProcContainer; "web" is a page, run in a webview by
    // WebContainer. Set at discovery, read by
    // ModuleManager::loadModuleInternal when it stamps ModuleDescriptor::format.
    std::string format;
    bool loaded = false;
    // Unix timestamp (seconds) of the most recent load, set by markLoaded and
    // cleared to 0 by markUnloaded. 0 ⟺ not currently loaded. Callers derive a
    // module's uptime from it (now - loadedAt), valid only while loaded.
    int64_t loadedAt = 0;
    // Readiness: has the module published its object? Tracked only while a
    // watch is armed, so nullopt means UNKNOWN, not "not ready".
    std::optional<bool> published;
    int64_t publishedAt = 0;
    // Bumped by every markLoaded. A readiness callback carries the epoch it was
    // armed under, so a fast unload/reload cannot let a stale watch mark the new
    // instance ready. Not loadedAt: that is whole seconds and collides.
    uint64_t loadEpoch = 0;
    // Null when loaded directly via markLoaded(name) (test/external scenarios).
    std::shared_ptr<LogosCore::ModuleLoader> loader;
    LogosCore::LoadedModuleHandle handle;
};

class ModuleRegistry {
public:
    void setModulesDir(const std::string& dir);
    void addModulesDir(const std::string& dir);
    std::vector<std::string> modulesDirs() const;

    void discoverInstalledModules();
    std::string processModule(const std::string& modulePath);

    bool isKnown(const std::string& name) const;
    std::string modulePath(const std::string& name) const;
    // A JSON array describing every known module: one object per module with
    // its name, path, loaded flag, load timestamp (loaded_at, unix seconds; 0
    // when not loaded), artifact `format` ("" for a Qt plugin, "bare"/"web" for a
    // Bare module image), `pid` (the loaded module's process id, -1 for a
    // module the Native container runs in-process, null when not loaded),
    // direct dependencies, direct dependents, and full embedded metadata
    // (parsed from the cached metadata JSON; null when unreadable). This is the
    // data backing logos_core_get_modules_info.
    nlohmann::json allModulesInfo() const;
    // Forward-edge accessor. `recursive=false` returns the direct
    // dependencies stored on ModuleInfo. `recursive=true` walks the forward
    // graph breadth-first and returns every transitive dependency. Unknown
    // names yield an empty list. Traversal is cycle- and diamond-safe.
    std::vector<std::string> moduleDependencies(const std::string& name,
                                                bool recursive = false) const;
    // The same direct edges, carrying the constraints they declared. Feeds the
    // dependency gate; unknown names yield an empty list.
    std::vector<LogosCore::ModuleDependency>
    moduleDependencyEntries(const std::string& name) const;
    // A module's own version, or "" when it is unknown or carries no stamp.
    std::string moduleVersion(const std::string& name) const;
    // The module's artifact shape: "" for a Qt plugin, "bare" for a Bare module
    // image. Read by the load path to decide which container can run it.
    std::string moduleFormat(const std::string& name) const;
    // Reverse-edge accessor. `recursive=false` returns the direct
    // dependents stored on ModuleInfo. `recursive=true` walks the reverse
    // graph breadth-first and returns every transitive dependent. Unknown
    // names yield an empty list.
    std::vector<std::string> moduleDependents(const std::string& name,
                                              bool recursive = false) const;
    // The optional edge set. Direct only, in both directions: an optional edge
    // says nothing about what lies beyond it, so a transitive walk mixing the
    // two would answer a question nothing asks.
    std::vector<std::string> moduleOptionalDependencies(const std::string& name) const;
    std::vector<LogosCore::ModuleDependency>
    moduleOptionalDependencyEntries(const std::string& name) const;
    std::vector<std::string> moduleOptionalDependents(const std::string& name) const;
    std::vector<std::string> knownModuleNames() const;
    void registerModule(const std::string& name, const std::string& path,
                        const std::vector<std::string>& dependencies = {});
    void registerDependencies(const std::string& name, const std::vector<std::string>& dependencies);
    // Constraint-carrying overload, and the version a dependent's range is
    // checked against. Direct graph mutators alongside registerModule.
    void registerDependencies(const std::string& name,
                              const std::vector<LogosCore::ModuleDependency>& dependencies);
    void registerOptionalDependencies(const std::string& name,
                                      const std::vector<std::string>& optionalDependencies);
    void registerModuleVersion(const std::string& name, const std::string& version);

    bool isLoaded(const std::string& name) const;
    void markLoaded(const std::string& name);

    // Full mark-as-loaded that stores the owning loader and handle for later
    // use by unloadModule(). Should be called from loadModuleInternal().
    void markLoaded(const std::string& name,
                    std::shared_ptr<LogosCore::ModuleLoader> loader,
                    LogosCore::LoadedModuleHandle handle);

    void markUnloaded(const std::string& name);
    std::vector<std::string> loadedModuleNames() const;
    void clearLoaded();

    // Readiness. beginPublishWatch flips `published` from unknown to false;
    // markPublished sets it true, but only if `epoch` still matches the current
    // load (a stale watch from a previous load is dropped). Returns whether it
    // applied. loadEpoch() reads the value to arm a watch with.
    void beginPublishWatch(const std::string& name);
    bool markPublished(const std::string& name, uint64_t epoch);
    uint64_t loadEpoch(const std::string& name) const;

    // Returns the loader that owns the named loaded module, or nullptr if
    // loaded without a loader association (e.g. via markLoaded(name) only).
    std::shared_ptr<LogosCore::ModuleLoader> loaderFor(const std::string& name) const;

    void clear();

private:
    // Reads the plugin's metadata and upserts a ModuleInfo for it.
    //
    // `trustedName` binds the module's *identity*. When non-empty (the
    // discovery path, where it is the package-manager's InstalledPackage::name)
    // it is used as the registry key, and a plugin whose own embedded metadata
    // name disagrees is REFUSED — this is the F-022 guard against a binary
    // claiming a privileged name it doesn't legitimately own. When empty (the
    // raw processModule() host API), the embedded name is used as before.
    std::string processModuleInternal(const std::string& modulePath,
                                      const std::string& trustedName = {});

    // The Bare-module arm of the same upsert.
    //
    // A Bare module carries NO Qt plugin metadata — that is what "bare" means —
    // so processModuleInternal's very first step, extractMetadata(), finds
    // nothing and it refuses the module. Its identity and its dependency edges
    // come from the package manifest instead, which the package manager has
    // already read and validated, and which is the trusted source in any case
    // (processModuleInternal only ever CHECKS the embedded name against it).
    //
    // Called for a package the discovery gate already identified as a Bare
    // module (looksLikeBareModule). Returns the registered name, or "" when
    // that name is not a valid module identifier.
    std::string processBareModuleInternal(const InstalledPackage& pkg);

    // The WEB arm of the same upsert, and it exists for the same reason: a page
    // carries no Qt plugin metadata either, so its identity and dependency
    // edges come from the package manifest.
    //
    // Called for a package the discovery gate already identified as a web
    // module (its `main` resolved to an .html document). Returns the registered
    // name, or "" when that name is not a valid module identifier.
    std::string processWebModuleInternal(const InstalledPackage& pkg);

    // Re-derives every ModuleInfo::dependents list by inverting the
    // dependencies edges across m_modules. Called at the tail of
    // discoverInstalledModules() and processModule(), and by any other
    // mutation that can change the graph (including registerModule and
    // registerDependencies). Must be called with m_mutex held
    // exclusively. Cost is O(N * avg_deps) — negligible for the module
    // counts we see and simpler than keeping incremental diffs.
    void recomputeDependentsLocked();
    std::vector<std::string> moduleDependenciesLocked(const std::string& name,
                                                      bool recursive) const;
    std::vector<std::string> moduleDependentsLocked(const std::string& name,
                                                    bool recursive) const;

    mutable std::shared_mutex m_mutex;
    std::vector<std::string> m_modulesDirs;
    std::unordered_map<std::string, ModuleInfo> m_modules;
};

#endif // MODULE_REGISTRY_H
