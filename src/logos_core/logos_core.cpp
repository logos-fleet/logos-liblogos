#include "logos_core.h"
#include "logging/logos_log.h"
#include "module_manager.h"
#include <logos_instance.h>
#include <process_stats/process_stats.h>
#include "token_manager.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <nlohmann/json.hpp>

// === C API Implementation (Thin Wrappers) ===

void logos_core_init(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
}

void logos_core_add_modules_dir(const char* modules_dir) {
    ModuleManager::addModulesDir(modules_dir);
}

void logos_core_start() {
    logos::initLogging();
    LogosInstance::id();
    // Before anything dials: this thread becomes the owner of every client.
    ModuleManager::anchorCoreApi();
    ModuleManager::discoverInstalledModules();
    ModuleManager::initializeCapabilityModule();
    // After capability_module: this one is optional, and its snapshot back-fills
    // everything that happened before it was up.
    ModuleManager::initializeModulesState();
}

void logos_core_cleanup() {
    ModuleManager::clear();
}

char** logos_core_get_loaded_modules() {
    return ModuleManager::getLoadedModulesCStr();
}

char** logos_core_get_known_modules() {
    return ModuleManager::getKnownModulesCStr();
}

int logos_core_load_module(const char* module_name, LogosLoadDeps deps) {
    if (!module_name) { logos::logger("core").critical("logos_core_load_module: module_name must not be null"); std::abort(); }
    // "Already loaded ⇒ success" is implemented in
    // ModuleManager::loadModuleInternal (see the block at the top there
    // for the rationale and the dep-tree fast path). The header doc
    // documents this as part of the public contract — keep both in sync.
    switch (deps) {
    case LOGOS_LOAD_REQUIRED_AND_OPTIONAL:
        return ModuleManager::loadModuleWithDependencies(
                   module_name, DependencyResolver::OptionalLoad::BestEffort) ? 1 : 0;
    case LOGOS_LOAD_REQUIRED_DEPS:
        return ModuleManager::loadModuleWithDependencies(
                   module_name, DependencyResolver::OptionalLoad::OrderOnly) ? 1 : 0;
    case LOGOS_LOAD_MODULE_ONLY:
        return ModuleManager::loadModule(module_name) ? 1 : 0;
    }
    // An out-of-range enum is a caller bug, and loading the required tree is
    // the answer that surprises least: it is what every caller of the old
    // `with_dependencies=true` asked for.
    logos::logger("core").warn("logos_core_load_module: unrecognised LogosLoadDeps {}; "
                               "treating as LOGOS_LOAD_REQUIRED_DEPS", static_cast<int>(deps));
    return ModuleManager::loadModuleWithDependencies(
               module_name, DependencyResolver::OptionalLoad::OrderOnly) ? 1 : 0;
}

char* logos_core_optional_load_report(const char* module_name) {
    if (!module_name) { logos::logger("core").critical("logos_core_optional_load_report: module_name must not be null"); std::abort(); }
    return ModuleManager::optionalLoadReportCStr(module_name);
}

int logos_core_unload_module(const char* module_name, bool with_dependents) {
    if (!module_name) { logos::logger("core").critical("logos_core_unload_module: module_name must not be null"); std::abort(); }
    if (with_dependents)
        return ModuleManager::unloadModuleWithDependents(module_name) ? 1 : 0;
    return ModuleManager::unloadModule(module_name) ? 1 : 0;
}

char** logos_core_get_module_dependencies(const char* module_name, bool recursive) {
    if (!module_name) { logos::logger("core").critical("logos_core_get_module_dependencies: module_name must not be null"); std::abort(); }
    return ModuleManager::getDependenciesCStr(module_name, recursive);
}

char** logos_core_get_module_optional_dependencies(const char* module_name) {
    if (!module_name) { logos::logger("core").critical("logos_core_get_module_optional_dependencies: module_name must not be null"); std::abort(); }
    return ModuleManager::getOptionalDependenciesCStr(module_name);
}

char** logos_core_get_module_dependents(const char* module_name, bool recursive) {
    if (!module_name) { logos::logger("core").critical("logos_core_get_module_dependents: module_name must not be null"); std::abort(); }
    return ModuleManager::getDependentsCStr(module_name, recursive);
}

char* logos_core_get_modules_info() {
    return ModuleManager::getModulesInfoCStr();
}

char* logos_core_process_module(const char* module_path) {
    if (!module_path) { logos::logger("core").critical("logos_core_process_module: module_path must not be null"); std::abort(); }
    return ModuleManager::processModuleCStr(module_path);
}

char* logos_core_add_bare_module(const char* metadata_json, const char* image_path) {
    if (!metadata_json) { logos::logger("core").critical("logos_core_add_bare_module: metadata_json must not be null"); std::abort(); }
    if (!image_path) { logos::logger("core").critical("logos_core_add_bare_module: image_path must not be null"); std::abort(); }
    return ModuleManager::addEmbeddedBareModuleCStr(metadata_json, image_path);
}

char* logos_core_get_token(const char* key) {
    if (!key) { logos::logger("core").critical("logos_core_get_token: key must not be null"); std::abort(); }

    std::string token = TokenManager::instance().getToken(std::string(key));
    if (token.empty()) return nullptr;

    char* result = new char[token.size() + 1];
    memcpy(result, token.c_str(), token.size() + 1);
    return result;
}

char* logos_core_get_module_stats() {
    const auto pids = ModuleManager::getModuleProcessIds();

    // process-stats answers by READING A PROCESS, so it can only answer for
    // modules that are one. Every module the Native container runs reports the
    // documented in-process sentinel (-1) and is silently absent from its
    // array, which would make a running module look like no module at all —
    // "stats has an entry per running module" is the whole contract of this
    // call, and the pid it reports is what tells them apart.
    //
    // So the in-process ones are appended here with cpu/memory NULL rather than
    // 0. Null is the honest value: their CPU and memory are the HOST's, already
    // counted once against the daemon's own pid, and reporting 0 would say
    // "measured, idle" for something that was never measured. A consumer that
    // wants the number for an in-process module looks at the host process.
    char* raw = ProcessStats::getModuleStats(pids);
    nlohmann::json stats = raw
        ? nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false)
        : nlohmann::json(nullptr);
    delete[] raw;
    if (!stats.is_array())
        stats = nlohmann::json::array();

    std::unordered_set<std::string> reported;
    for (const auto& entry : stats) {
        if (entry.is_object() && entry.contains("name") && entry["name"].is_string())
            reported.insert(entry["name"].get<std::string>());
    }

    for (const auto& [name, pid] : pids) {
        if (pid >= 0 || reported.count(name))
            continue;
        stats.push_back({
            {"name", name},
            {"pid", pid},
            {"cpu_percent", nullptr},
            {"cpu_time_seconds", nullptr},
            {"memory_mb", nullptr},
        });
    }

    const std::string dumped = stats.dump();
    char* result = new char[dumped.size() + 1];
    memcpy(result, dumped.c_str(), dumped.size() + 1);
    return result;
}

void logos_core_set_persistence_base_path(const char* path) {
    if (!path) { logos::logger("core").critical("logos_core_set_persistence_base_path: path must not be null"); std::abort(); }
    ModuleManager::setPersistenceBasePath(path);
}

void logos_core_set_module_transports(const char* module_name,
                                       const char* transport_set_json) {
    if (!module_name) {
        logos::logger("core").critical("logos_core_set_module_transports: module_name must not be null");
        std::abort();
    }
    ModuleManager::setModuleTransports(
        std::string(module_name),
        transport_set_json ? std::string(transport_set_json) : std::string{});
}

void logos_core_set_access_policy(const char* policy_json) {
    // NULL/"" clears the policy (see header) — unlike the module-name
    // setters above, this does not abort on NULL.
    ModuleManager::setAccessPolicy(
        policy_json ? std::string(policy_json) : std::string{});
}

void logos_core_set_container_policy(const char* policy) {
    // NULL/"" resets to "auto" (see header) — like the access policy, this
    // does not abort on NULL.
    ModuleManager::setContainerPolicy(policy ? std::string(policy) : std::string{});
}

void logos_core_refresh_modules()
{
    ModuleManager::discoverInstalledModules();
}
