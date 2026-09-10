#ifndef LOGOS_CORE_H
#define LOGOS_CORE_H

// Define export macro for the library.
//
// Windows needs __declspec, not visibility attributes: mingw-gcc accepts
// __attribute__((visibility)) and silently ignores it on PE, so this header
// used to export nothing at all and the build compensated with
// -Wl,--export-all-symbols. That worked, but it exported the ENTIRE image --
// 13,252 symbols, of which only 18 are this C API -- including every internal
// C++ symbol such as LogosAPI's. Any consumer that links liblogos_core AND the
// qt-sdk static library then gets the same definition twice and the link fails
// with "multiple definition of `LogosAPI::LogosAPI'".
//
// Annotating the C API explicitly fixes that at the root, and does so twice
// over: GNU ld disables PE auto-export image-wide as soon as ANY symbol is
// dllexported, so the internal C++ surface stops leaking as a side effect.
#if defined(_WIN32)
#  if defined(LOGOS_CORE_LIBRARY)
#    define LOGOS_CORE_EXPORT __declspec(dllexport)
#  else
#    define LOGOS_CORE_EXPORT __declspec(dllimport)
#  endif
#elif defined(LOGOS_CORE_LIBRARY)
#  define LOGOS_CORE_EXPORT __attribute__((visibility("default")))
#else
#  define LOGOS_CORE_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#else
// `bool` in C requires <stdbool.h>. C++ has it built-in as a keyword.
#include <stdbool.h>
#endif

// Initialize the logos core library
LOGOS_CORE_EXPORT void logos_core_init(int argc, char *argv[]);

// Add a modules directory to scan (allows multiple directories).
// Duplicate paths are silently ignored.
LOGOS_CORE_EXPORT void logos_core_add_modules_dir(const char* modules_dir);

// Start the logos core functionality
LOGOS_CORE_EXPORT void logos_core_start();

// Clean up resources
LOGOS_CORE_EXPORT void logos_core_cleanup();

// Get the list of loaded modules
// Returns a null-terminated array of module names that must be freed by the caller
LOGOS_CORE_EXPORT char** logos_core_get_loaded_modules();

// Get the list of known modules
// Returns a null-terminated array of module names that must be freed by the caller
LOGOS_CORE_EXPORT char** logos_core_get_known_modules();

// How far logos_core_load_module walks the dependency graph.
//
// An ENUM rather than a second bool: "best effort optional" means nothing
// unless dependencies are being resolved at all, and two independent bools
// would let a caller ask for a combination that does not exist.
//
// THE FIRST TWO VALUES ARE 0 AND 1 ON PURPOSE. This replaced a
// `bool with_dependencies`, and under C linkage the symbol mangles the same
// either way — a consumer that hand-copies the old prototype (logos-standalone-app
// does, in an extern "C" block) keeps compiling AND linking against the new
// library with no diagnostic anywhere. Pinning false→MODULE_ONLY and
// true→REQUIRED_DEPS makes that silent case keep its old behaviour exactly,
// so a consumer that has not bumped yet is stale rather than broken. Do not
// renumber these.
typedef enum {
    // Load this module alone. Its dependencies must already be up.
    LOGOS_LOAD_MODULE_ONLY = 0,
    // Resolve and load the REQUIRED tree in topological order first. Optional
    // dependencies are not loaded, and a missing one is not a failure.
    LOGOS_LOAD_REQUIRED_DEPS = 1,
    // As above, and additionally load every optional dependency that is
    // INSTALLED, ordered ahead of the module that names it.
    //
    // Best effort in every direction that can go wrong: one that is not
    // installed is skipped in silence, one that is installed but FAILS to load
    // is logged and stepped over, and one whose OWN required dependencies are
    // not all installed is left out entirely. None of the three changes the
    // return value — nothing requires these, which is what makes them optional.
    //
    // TRANSITIVE, and admitted whole or not at all. An optional dependency
    // brings its own required dependencies with it, and its own optional ones
    // are considered in turn. A branch joins only if every module it REQUIRES
    // is installed: half of one would just fail at load time for something
    // nobody asked for. Use it to bring a module up alongside collaborators
    // that happen to be present, without making its startup contingent on them.
    LOGOS_LOAD_REQUIRED_AND_OPTIONAL = 2,
} LogosLoadDeps;

// Load a specific module by name.
// `deps` decides how far the graph is walked; see LogosLoadDeps above.
//
// Semantics: "ensure loaded", not "load fresh". Returns 1 when all
// required modules end up loaded — including the case where the target
// (or, when resolving, any of its REQUIRED deps) was already loaded
// before the call. This idempotency is load-bearing for callers that
// use it as a guard ("make sure X is up before I use it").
// Returns 0 only when the module is unknown, dependency resolution
// fails, an actual load step (not an already-loaded no-op) fails, or the
// call is RE-ENTRANT — see the concurrency note below. A best-effort optional
// dependency failing to load is none of those.
// Aborts the process if `module_name` is NULL.
//
// "Loaded" here means the module's plugin loaded in its host process, not
// merely that the host was spawned and not yet that the module is
// reachable — the call waits for the host to report which. It therefore
// BLOCKS for as long as bringing the module up takes, and a module whose
// plugin fails to load answers 0 with the reason in the log and on the
// modules_state feed. A host too old to report anything is not treated as
// a failure: the call falls back to what it always did and warns.
//
// CONCURRENCY. Because the call blocks for the whole bring-up, loads of
// DIFFERENT modules run at the same time: each takes only its own module's
// lock. Two callers naming the SAME module are still one load — the second
// waits and is then answered by the "already loaded" no-op above, so it never
// starts a second host.
//
// A load may NOT be started from inside one, even on the same thread: it is
// refused and answers 0 (or 1 if that module is already up). This is reachable
// without threads — a call out to capability_module spins a nested Qt event
// loop, so a load posted with a queued connection can be delivered inside one
// already running. Proceeding would take the fleet lock recursively, which is
// undefined behaviour; the single lock this replaced deadlocked outright.
//
// Which thread. Core's outbound calls all run on the thread that called
// logos_core_start() — inline when you are that thread, posted to it when you
// are not — so a load off it neither strands Qt objects on a dying worker nor
// deadlocks against the owner. Two consequences for an off-thread caller:
// the capability-module registration and the readiness watch complete
// ASYNCHRONOUSLY after the call returns, and they need the owner thread to be
// running its event loop. A caller that needs the registration to have
// happened before it proceeds should load from the owner thread, where it
// still runs inline and in order.
LOGOS_CORE_EXPORT int logos_core_load_module(const char* module_name, LogosLoadDeps deps);

// Which optional dependencies LOGOS_LOAD_REQUIRED_AND_OPTIONAL would leave out
// for `module_name`, and why. Returns a JSON array; each element is an object:
//   "module"    the optional dependency that would not be loaded
//   "named_by"  the module whose metadata names it optionally
//   "reason"    "not_installed" — it is not installed here
//               "unsatisfiable" — it is, but something it REQUIRES is not
//   "missing"   present only for "unsatisfiable": the module that is absent
// Returns "[]" when nothing would be left out. The string must be freed by
// the caller. Aborts the process if `module_name` is NULL.
//
// A QUERY rather than an out-parameter on the load, because a skipped module
// is otherwise invisible: it keeps whatever state it had, so nothing appears on
// the lifecycle feed and nothing distinguishes "deliberately left out" from
// "nobody ever asked for it". Deterministic given what is installed, so calling
// it before a load predicts, and calling it after explains.
LOGOS_CORE_EXPORT char* logos_core_optional_load_report(const char* module_name);

// Unload a specific module by name.
// When with_dependents is true, also unloads every loaded module that
// (transitively) depends on it. Dependents come down first (leaves-first)
// so no process is briefly pointing at a terminated parent.
// Returns 1 if successful, 0 if failed
LOGOS_CORE_EXPORT int logos_core_unload_module(const char* module_name, bool with_dependents);

// Return the modules that `module_name` depends on (forward edges).
// If `recursive` is true, returns the full transitive dependency closure
// reached by a breadth-first walk; the target itself is not included.
// Unknown names yield a zero-length array (just a trailing NULL).
// Returns a null-terminated array of module names that must be freed by the caller.
LOGOS_CORE_EXPORT char** logos_core_get_module_dependencies(const char* module_name, bool recursive);

// Return the modules that depend on `module_name` (reverse edges).
// If `recursive` is true, returns the full transitive dependent closure.
// Unknown names yield a zero-length array (just a trailing NULL).
// Returns a null-terminated array of module names that must be freed by the caller.
LOGOS_CORE_EXPORT char** logos_core_get_module_dependents(const char* module_name, bool recursive);

// Return the modules `module_name` declares as OPTIONAL dependencies: concrete
// modules it can call but does not require. They are never auto-loaded, their
// absence is not a load failure, and unloading one does not take its dependents
// down — so unlike the two accessors above there is no `recursive` form. An
// optional edge says nothing about what lies beyond it.
// Unknown names yield a zero-length array (just a trailing NULL).
// Returns a null-terminated array of module names that must be freed by the caller.
LOGOS_CORE_EXPORT char** logos_core_get_module_optional_dependencies(const char* module_name);

// Get information about all known modules as a JSON string.
// Returns a JSON array; each element is an object with:
//   "name"         module name
//   "path"         path to the module binary
//   "loaded"       bool — whether the module is currently loaded
//   "loaded_at"    unix-seconds timestamp of the current load, 0 when not
//                  loaded (callers compute uptime as now - loaded_at)
//   "dependencies" array of direct dependency names
//   "dependents"   array of direct dependent names
//   "optional_dependencies" / "optional_dependents" — the same two edges for
//                  metadata.json#optional_dependencies, kept as separate keys
//                  because the loader treats the two sets differently
//   "metadata"     the module's full embedded metadata object (name, version,
//                  type, description, dependencies, …), or null if unreadable
// The returned string must be freed by the caller.
LOGOS_CORE_EXPORT char* logos_core_get_modules_info();

// Process a module file and add it to known modules
// Returns the module name if successful, NULL if failed
LOGOS_CORE_EXPORT char* logos_core_process_module(const char* module_path);

// Get a token by key from the core token manager
// Returns the token value if found, NULL if not found
// The returned string must be freed by the caller
LOGOS_CORE_EXPORT char* logos_core_get_token(const char* key);

// Get module statistics (CPU and memory usage) for all loaded modules.
// Returns a JSON string containing an array of module stats, NULL on error.
// The returned string must be freed by the caller.
//
// ONE ENTRY PER RUNNING MODULE, whichever container it runs in. A module in the
// Native container has no process of its own — it reports pid -1, the
// LoadedModuleHandle sentinel — so its `cpu_percent`, `cpu_time_seconds` and
// `memory_mb` are NULL rather than 0: those resources belong to the host
// process and are already counted against its pid. `pid` is what distinguishes
// the two cases.
LOGOS_CORE_EXPORT char* logos_core_get_module_stats();

// Set the base directory for module instance persistence.
// Each module gets a subdirectory: {path}/{module_name}/{instance_id}/
// Must be called before logos_core_start().
LOGOS_CORE_EXPORT void logos_core_set_persistence_base_path(const char* path);

// Register a per-module transport set for the named module. The loader
// passes this through to the module's child subprocess so its
// LogosAPIProvider binds every transport in the set instead of only
// the global default (LocalSocket).
//
// `transport_set_json` is a JSON array of LogosTransportConfig values
// (see logos-cpp-sdk/cpp/logos_transport_config_json.h for the shape).
// NULL or "" clears any previously-registered entry.
//
// Must be called BEFORE the module is loaded — for capability_module
// this means before logos_core_start(); for user modules, before any
// logos_core_load_module() call. Modules without an entry continue to
// inherit the global default.
LOGOS_CORE_EXPORT void logos_core_set_module_transports(const char* module_name,
                                                         const char* transport_set_json);

// Install the inter-module access policy: which callers may invoke which
// targets. `policy_json` shape:
//
//     {
//       "version": 1,
//       "mode": "enforce",
//       "restrictions": {
//         "package_manager":    { "allowedCallers": ["package_manager_ui"] },
//         "package_downloader": { "allowedCallers": ["package_manager_ui"] }
//       }
//     }
//
// A restricted target rejects callers outside its allowlist; a target
// absent from `restrictions` is unrestricted. Only `mode` == "enforce"
// activates gating (any other value registers nothing). Enforced by
// capability_module, which won't issue a token — hence won't allow the
// call — for a disallowed caller.
//
// Must be called before logos_core_start(). NULL or "" clears the policy.
LOGOS_CORE_EXPORT void logos_core_set_access_policy(const char* policy_json);

// Constrain which CONTAINER modules are allowed to run in.
//
// Which container actually runs a module is decided by its artifact: a Bare
// module image runs in-process (the Native container), a Qt plugin runs in a
// subprocess host. This does not change that. It asserts what the operator
// EXPECTS, so a workspace that does not match says so at load instead of
// quietly doing the other thing:
//
//   "auto"        (default, and what NULL/"" means) — run each module in
//                 whichever container its artifact calls for.
//   "inproc"      — every module must be a Bare module. A Qt plugin is refused.
//   "subprocess"  — every module must be a Qt plugin. A Bare module is refused.
//   "web"         — every module must be a web variant (a page). A Qt plugin
//                 and a Bare module are both refused.
//
// The reason it is an assertion and not a switch: a module has ONE artifact in
// a given directory, and no flag can turn a Qt plugin into a Bare module, or
// either of them into a page. A flag that silently fell back would make `--container inproc` mean
// "in-process if you happen to have built it that way", which is not something
// an operator can rely on or a CI job can assert.
//
// Must be called before the modules it governs are loaded. An unrecognised
// value is refused and leaves the previous policy in place.
LOGOS_CORE_EXPORT void logos_core_set_container_policy(const char* policy);

// Re-scan all module directories and update known modules.
// Call after installing new modules so they become discoverable.
LOGOS_CORE_EXPORT void logos_core_refresh_modules();

#ifdef __cplusplus
}
#endif

#endif // LOGOS_CORE_H
