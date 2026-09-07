# Project Description

## Project Structure

```
logos-liblogos/
├── CMakeLists.txt                       # Root CMake configuration
├── README.md                            # Project overview and build instructions
├── flake.nix                            # Nix flake configuration
├── flake.lock                           # Nix flake lock file
├── docs/
│   ├── index.md                         # Documentation index
│   ├── spec.md                          # High-level specification
│   └── project.md                       # This document
├── src/
│   ├── CMakeLists.txt                   # Source build configuration
│   └── logos_core/                      # Core library implementation
│       ├── logos_core.h                 # C API header (public)
│       ├── logos_core.cpp               # C API implementation
│       ├── module_manager.h/cpp         # Facade: orchestrates registry, loader registry, resolver
│       ├── module_registry.h/cpp        # In-memory registry of discovered/loaded modules
│       ├── dependency_resolver.h/cpp    # Topological sort with circular dependency detection
│       ├── module_loader.h              # Abstract ModuleLoader base (Qt-free)
│       ├── composite_module_loader.h/cpp # Pairs a container + format loader into a ModuleLoader
│       └── module_loader_registry.h/cpp  # Registry of ModuleLoader implementations
│   (the Qt-plugin loader + logos_host_qt binary now live in the external
│    logos-module-loader-qt package — see "External packages" below)
├── tests/                               # Google Test suite
│   ├── CMakeLists.txt                   # Test build configuration
│   ├── test_app_lifecycle.cpp           # C API lifecycle tests (init, exec, cleanup, processEvents)
│   ├── test_module_manager.cpp          # ModuleManager + ModuleRegistry tests
│   ├── test_subprocess_manager.cpp      # SubprocessManager lifecycle and subprocess tests
│   ├── test_composite_module_loader.cpp # CompositeModuleLoader pairing tests
│   ├── test_module_loader_registry.cpp  # ModuleLoaderRegistry selection and fan-out tests
│   ├── test_module_loader_abstraction.cpp   # End-to-end loader abstraction tests (FakeModuleLoader)
│   ├── test_dependency_resolver.cpp     # DependencyResolver tests
│   ├── test_process_stats.cpp           # ProcessStats tests (external process-stats lib)
│   ├── test_module_name_validation.cpp  # Module-name allowlist regression (F-030)
│   ├── subprocess_manager.h             # Test-only shim composing the external container + Qt loader
│   └── qt_test_adapter.h               # Qt test utilities/adapter header
├── nix/                                 # Nix build modules
│   ├── default.nix                      # Common configuration (deps, flags, metadata)
│   ├── build.nix                        # Shared build derivation
│   ├── bin.nix                          # Re-exports logos_host_qt (from logos-module-loader-qt) + runtime libs
│   ├── lib.nix                          # Library extraction (liblogos_core)
│   ├── include.nix                      # Header installation
│   ├── modules.nix                      # Bundled built-in modules
│   └── tests.nix                        # Test suite build
└── .github/
    └── workflows/
        └── ci.yml                       # GitHub Actions CI workflow
```

### External packages

Both the container and the format-loader abstractions are extracted to their own
repos so each side can be swapped without touching the core. Each has a Qt-free
header-only **contract** package and a separate **implementation** package.
liblogos consumes all four as nix inputs via `*_ROOT` CMake roots, the same way
it consumes `process-stats`.

**Container side**

- **[logos-container](https://github.com/logos-co/logos-container)** — the Qt-free,
  header-only **container contract**, included as `<logos_container/...>`:
  - `module_container.h` — `ModuleContainer` interface (where/how a module runs)
  - `module_descriptor.h` — `ModuleDescriptor` / `LoadedModuleHandle` value types exchanged across the container boundary
- **[logos-container-subprocess](https://github.com/logos-co/logos-container-subprocess)**
  — the subprocess `ModuleContainer` (a single Qt-free static lib), included as
  `<logos_container_subprocess/...>` and linked into `logos_core`:
  - `subprocess_container.h/cpp` — process spawn, pipes, kill, crash detection, and auth-token delivery over the child's stdin (no child-side library — the child reads its token from stdin via the host's own `TokenSource`)

**Format-loader side**

- **[logos-module-loader](https://github.com/logos-co/logos-module-loader)** — the
  Qt-free, header-only **format-loader contract**, included as
  `<logos_module_loader/...>` (depends on `logos-container` for `ModuleDescriptor`):
  - `module_format_loader.h` — `ModuleFormatLoader` interface (what format a module is; resolves a host binary + builds CLI args)
- **[logos-module-loader-qt](https://github.com/logos-co/logos-module-loader-qt)**
  — the whole Qt-plugin mechanism:
  - `logos_module_loader_qt` static lib — `QtPluginFormatLoader` (parent side, light: boost::dll + spdlog), included as `<logos_module_loader_qt/...>` and linked into `logos_core`
  - `logos_host_qt` binary — the child-side host (`logos_host`, `command_line_parser`, `module_initializer`, `qt_app`, `token_source`), linking the full SDK stack + Qt. liblogos no longer builds it; `bin.nix` re-exports it from this package so frontends are unaffected.

The `ModuleLoader` base, the `CompositeModuleLoader` / `ModuleLoaderRegistry`
orchestration, the `isValidModuleName` allowlist (in `module_registry`), and the
`logos_log` logging foundation are core concerns and remain in `logos-liblogos`
(`src/logos_core/` and `src/logging/`).

The core names **no concrete** container or loader — not in its C++ and not in
its CMake. It builds its default by calling the contract factory seams
`LogosCore::makeContainer()` (`<logos_container/container_factory.h>`) and
`LogosCore::makeFormatLoader()` (`<logos_module_loader/format_loader_factory.h>`)
and composing the results into a `CompositeModuleLoader`. Each seam's
*definition* is provided by whichever implementation library is linked in:
`logos-container-subprocess` defines `makeContainer()` → `SubprocessContainer`,
`logos-module-loader-qt` defines `makeFormatLoader()` → `QtPluginFormatLoader`.

**Which implementation is the default is chosen in nix**, not in the source.
Each implementation package ships a generic **CMake config package** —
`LogosContainerImpl` (from `logos-container-subprocess`) and
`LogosFormatLoaderImpl` (from `logos-module-loader-qt`) — that defines an
imported target (`LogosContainerImpl::impl` / `LogosFormatLoaderImpl::impl`)
carrying its static library *and its own transitive deps* (Boost, spdlog, …).
`src/CMakeLists.txt` just does `find_package(LogosContainerImpl REQUIRED)` and
links the target; it knows neither the library name nor the dependencies.
`nix/default.nix` puts the `containerImpl` / `formatLoaderImpl` packages in
`buildInputs` (so they land on `CMAKE_PREFIX_PATH`) — no impl flags at all; the
packages are chosen in `flake.nix` (default `logos-container-subprocess` /
`logos-module-loader-qt`). Swapping to a different container (Docker, in-process)
is a one-line change in `flake.nix` pointing at a package that provides the same
config + factory symbol — no C++, CMake, or nix-flag edit.

A test-only `SubprocessManager` shim (`tests/subprocess_manager.h`) composes the
external `SubprocessContainer` with `QtPluginFormatLoader` so test code that
references the old name keeps compiling. It lives in `tests/` (not `src/`)
deliberately — the production module-loader layer stays free of any dependency
on a specific container.

## Stack, Frameworks & Dependencies

| Component | Purpose |
|-----------|---------|
| **C++17** | Implementation language |
| **CMake 3.14+** | Build system |
| **Qt 6** (Core, RemoteObjects) | Event loop, module system, IPC, meta-object system |
| **Boost** (Process, Asio, Uuid) | Subprocess management, async I/O, and UUID generation |
| **nlohmann_json** | JSON parsing/serialization (replaces Qt JSON internally) |
| **OpenSSL** | Required transitively by the SDK's plain-C++ TCP+TLS transport |
| **zstd** | Compression (build dependency) |
| **spdlog** | Structured logging |
| **Google Test** | Unit testing framework |
| **Nix** | Package management and reproducible builds |

### External Logos Dependencies

| Dependency | Purpose |
|------------|---------|
| **[logos-cpp-sdk](https://github.com/logos-co/logos-cpp-sdk)** | C++ client library (LogosAPI, LogosAPIClient, TokenManager, PluginInterface) |
| **[logos-module](https://github.com/logos-co/logos-module)** | Module library (metadata extraction, module loading utilities) |
| **[logos-capability-module](https://github.com/logos-co/logos-capability-module)** | Built-in capability authorization module |
| **[process-stats](https://github.com/logos-co/process-stats)** | CPU and memory monitoring for module processes |
| **[logos-container](https://github.com/logos-co/logos-container)** | Qt-free header-only container contract (`ModuleContainer` interface, `ModuleDescriptor`/`LoadedModuleHandle` value types) |
| **[logos-container-subprocess](https://github.com/logos-co/logos-container-subprocess)** | Subprocess `ModuleContainer` implementation (process spawn + auth-token handoff) |
| **[logos-module-loader](https://github.com/logos-co/logos-module-loader)** | Qt-free header-only format-loader contract (`ModuleFormatLoader` interface) |
| **[logos-module-loader-qt](https://github.com/logos-co/logos-module-loader-qt)** | Qt-plugin format loader (`QtPluginFormatLoader`) + the `logos_host_qt` module-host binary |
| **[logos-package-manager](https://github.com/logos-co/logos-package-manager-module)** | Package management library for installed module discovery |
| **[logos-nix](https://github.com/logos-co/logos-nix)** | Common Nix tooling and nixpkgs pin |

## Core Modules

### ModuleManager

**Files:** `src/logos_core/module_manager.h`, `src/logos_core/module_manager.cpp`

**Purpose:** Thin facade that orchestrates `ModuleRegistry`, `ModuleLoaderRegistry`, and `DependencyResolver`. Provides the C++-level API for module lifecycle management. Each module runs in a separate subprocess managed by the selected `ModuleLoader` implementation (default: `SubprocessManager`, which spawns `logos_host_qt` processes).

**Thread safety:** `loadModule`, `loadModuleWithDependencies`, and `unloadModule` are serialised by a static `loadMutex()` (one load/unload at a time). `discoverInstalledModules` delegates to `ModuleRegistry` which has its own reader-writer lock.

**API (namespace `ModuleManager`):**

| Method | Description |
|--------|-------------|
| `registry() → ModuleRegistry&` | Access the shared module registry |
| `loaders() → ModuleLoaderRegistry&` | Access the shared loader registry |
| `setModulesDir(path)` | Set the primary module directory (clears existing) |
| `addModulesDir(path)` | Add an additional module directory |
| `setPersistenceBasePath(path)` | Set base directory for module instance persistence |
| `setModuleTransports(name, json)` | Register a per-module `LogosTransportSet` (serialized JSON). Threaded through to the child subprocess on load so its provider binds every listener instead of only the global default. Empty clears the entry. Read+write protected by `loadMutex()` |
| `discoverInstalledModules()` | Scan all module directories and register discovered modules |
| `processModule(path) → std::string` | Extract metadata from a module file, register as known |
| `processModuleCStr(path) → char*` | C-string variant of processModule |
| `loadModule(name) → bool` | Load a module (selects a loader via ModuleLoaderRegistry, spawns subprocess, sends auth token) |
| `loadModuleWithDependencies(name) → bool` | Resolve dependency tree, load in topological order. Returns false if any dependency is unknown or a cycle is detected (hard failure on `!ResolveResult::ok()`) |
| `initializeCapabilityModule() → bool` | Load the built-in capability module if available |
| `unloadModule(name) → bool` | Terminate module process and update registry |
| `unloadModuleWithDependents(name) → bool` | Cascade unload: terminate the named module together with every currently loaded module that transitively depends on it, leaves-first |
| `terminateAll()` | Terminate all running module processes |
| `clear()` | Clear registry and reset all state |
| `resolveDependencies(modules) → std::vector<std::string>` | Topological sort with circular dependency detection |
| `getDependencies(name, recursive) → std::vector<std::string>` | Declared dependencies of `name` among known modules; walks the forward graph transitively when `recursive=true`. Cycle- and diamond-safe BFS |
| `getDependents(name, recursive) → std::vector<std::string>` | Declared dependents of `name` among known modules; walks the reverse graph transitively when `recursive=true`. Reads from the in-process registry, no disk query |
| `getDependenciesCStr(name, recursive) → char**` | C-string variant backing `logos_core_get_module_dependencies` |
| `getDependentsCStr(name, recursive) → char**` | C-string variant backing `logos_core_get_module_dependents` |
| `getLoadedModulesCStr() → char**` | Return loaded module names as null-terminated C string array |
| `getKnownModulesCStr() → char**` | Return known module names as null-terminated C string array |
| `isModuleLoaded(name) → bool` | Check if a module is currently loaded |
| `getModuleProcessIds() → std::unordered_map<std::string, int64_t>` | Return module name → process ID mappings |

### ModuleRegistry

**Files:** `src/logos_core/module_registry.h`, `src/logos_core/module_registry.cpp`

**Purpose:** In-memory registry of discovered and loaded modules. Single source of truth for the dependency graph: stores module paths, forward dependencies, and the derived reverse edges (dependents). All public methods are thread-safe: mutating methods acquire a `std::unique_lock` on an internal `std::shared_mutex`; read-only methods acquire a `std::shared_lock`, allowing concurrent reads.

**Trust boundary — module name validation:** A module's name comes verbatim from its (untrusted) embedded plugin metadata and is later used as this map's key, as the RPC target, and as a filesystem path segment for the instance-persistence directory. Prevents this attack (CWE-22): a malicious installed module declares `name='../<x>'`; the `/` or `..` escapes the intended directory when used as a path segment, or collides with another module's registry key / RPC identity. `processModuleInternal()` validates the name with `logos::isValidModuleName` (declared in `src/logos_core/module_registry.h`, allowlist `[A-Za-z0-9_-]`, ≤64 bytes, rejects `/`, `..`, etc.) and skips any module whose name is unsafe, so an unsafe name never enters the registry.

**Data:**
- `ModuleInfo` struct — holds `path`, `dependencies` (`std::vector<std::string>`), `dependents` (`std::vector<std::string>`, reverse-edge cache), `loaded` flag, `loader` (`std::shared_ptr<ModuleLoader>`), `handle` (`LoadedModuleHandle`)
- `std::unordered_map<std::string, ModuleInfo> m_modules` — module database keyed by name
- `std::vector<std::string> m_modulesDirs` — configured module directories
- `std::shared_mutex m_mutex` — reader-writer lock protecting all fields

**Dependency graph invariant:** `ModuleInfo::dependents` mirrors the inverse of `dependencies`, and `ModuleInfo::optionalDependents` the inverse of `optionalDependencies`, across all known modules. The two edge sets are inverted separately and never merged — the load closure, the teardown cascade and the missing-dependency verdict all read the required set, and every one of them must ignore the optional one. `ModuleRegistry` owns this invariant and maintains it by calling the private `recomputeDependentsLocked()` at the tail of every forward-edge mutation (`discoverInstalledModules`, `processModule`, `registerModule` when deps are passed, `registerDependencies`). Callers never populate `dependents` directly. This replaces the previous pattern of querying `PackageManagerLib::resolveDependents()` on disk — the registry is now the single authority for reverse-dep lookups, and `ModuleManager::getDependents` / `unloadModuleWithDependents` read straight from it.

**API (class `ModuleRegistry`):**

| Method | Description |
|--------|-------------|
| `setModulesDir(dir)` | Clear and set single module directory |
| `addModulesDir(dir)` | Add to module directory list |
| `modulesDirs() → std::vector<std::string>` | Return configured directories |
| `discoverInstalledModules()` | Scan directories, parse manifest.json files; recomputes dependents at end |
| `processModule(path) → std::string` | Extract metadata from module file, register as known; recomputes dependents at end. Rejects (returns `""`, no registry entry) a module whose name — taken from untrusted plugin JSON — is not a single safe path segment (`logos::isSafePathSegment`), since the name later becomes a token-socket / persistence path component |
| `registerModule(name, path, deps, optionalDeps)` | Manually register a module; recomputes both dependent sets |
| `registerDependencies(name, deps)` | Set dependencies for a known module; recomputes dependents |
| `registerOptionalDependencies(name, deps)` | Set optional dependencies for a known module; recomputes dependents |
| `moduleOptionalDependencies(name) → std::vector<std::string>` | Forward optional edges. Direct only — an optional edge says nothing about what lies beyond it |
| `moduleOptionalDependents(name) → std::vector<std::string>` | Reverse optional edges. Direct only, same reason |
| `isKnown(name) → bool` | Module exists in registry |
| `modulePath(name) → std::string` | Get file path for a known module |
| `moduleDependencies(name, recursive) → std::vector<std::string>` | Forward-edge lookup. `recursive=false` returns direct dependencies from `ModuleInfo`; `recursive=true` walks the forward graph breadth-first (cycle/diamond safe) |
| `moduleDependents(name, recursive) → std::vector<std::string>` | Reverse-edge lookup. `recursive=false` returns direct dependents from `ModuleInfo`; `recursive=true` walks the reverse graph breadth-first (cycle/diamond safe) |
| `knownModuleNames() → std::vector<std::string>` | All discovered module names |
| `isLoaded(name) → bool` | Module is currently running |
| `markLoaded(name)` / `markUnloaded(name)` | Update load state |
| `markLoaded(name, loader, handle)` | Mark loaded with associated loader and handle |
| `loaderFor(name) → std::shared_ptr<ModuleLoader>` | Get the loader that loaded a given module |
| `loadedModuleNames() → std::vector<std::string>` | Currently running module names |
| `clearLoaded()` | Clear all loaded state |
| `clear()` | Reset entire registry |

### ModuleLoader (interface)

**Files:** `src/logos_core/module_loader.h`

**Purpose:** Abstract interface for module loading strategies. Decouples the core from any specific subprocess or module-loading mechanism (Qt, WASM, in-process, etc.). Each implementation handles a particular module format.

**Supporting types:**
- `ModuleDescriptor` — describes a module to load: `name`, `path`, `format`, `modulesDirs`, `instancePersistencePath`, `transportSetJson` (per-module `LogosTransportSet` serialized as JSON; empty = inherit the global default LocalSocket), `onTerminated` callback
- `LoadedModuleHandle` — opaque handle returned by `load()`: `pid` and `opaque` (`std::any`)

**API (class `ModuleLoader`):**

| Method | Description |
|--------|-------------|
| `id() → std::string` | Unique loader identifier (e.g. `"qt-subprocess"`) |
| `canHandle(desc) → bool` | Whether this loader can load the given module descriptor |
| `load(desc) → std::optional<LoadedModuleHandle>` | Load a module, return a handle on success |
| `sendToken(handle, token) → bool` | Send auth token to the loaded module |
| `terminate(handle)` | Terminate a specific loaded module |
| `terminateAll()` | Terminate all modules managed by this loader |
| `getAllPids() → std::unordered_map<std::string, int64_t>` | Map module names to process IDs |

### ModuleLoaderRegistry

**Files:** `src/logos_core/module_loader_registry.h`, `src/logos_core/module_loader_registry.cpp`

**Purpose:** Central registry of `ModuleLoader` implementations. Selects the appropriate loader for a given `ModuleDescriptor` by iterating registered loaders and calling `canHandle()`. Fans out `terminateAll()` and `getAllPids()` across all registered loaders.

**API (class `ModuleLoaderRegistry`):**

| Method | Description |
|--------|-------------|
| `registerLoader(loader)` | Register a `ModuleLoader` implementation |
| `select(desc) → std::shared_ptr<ModuleLoader>` | Find the first loader that can handle the descriptor |
| `terminateAll()` | Terminate all modules across all loaders |
| `getAllPids() → std::unordered_map<std::string, int64_t>` | Aggregate PIDs from all loaders |

### DependencyResolver

**Files:** `src/logos_core/dependency_resolver.h`, `src/logos_core/dependency_resolver.cpp`

**Purpose:** Compute topological sort of module dependencies using Kahn's algorithm. Detects circular dependencies and missing modules.

**API (namespace `DependencyResolver`):**

**API (namespace `DependencyResolver`):**

| Type / Method | Description |
|---------------|-------------|
| `ResolveResult` | Result struct: `order` (topological load order), `missing` (unknown dependency names), `hasCycle` (cycle detected). `ok()` returns true when `missing` is empty and `hasCycle` is false |
| `resolve(requested, isKnown, getDependencies) → ResolveResult` | Resolves dependencies via Kahn's algorithm. Returns the reachable, known modules in load order plus diagnostic info about missing deps and cycles. Callers decide policy: load paths treat `!ok()` as a hard failure; teardown paths use `.order` only |

Takes callback functions (`IsKnownFn`, `GetDependenciesFn`) so it has no coupling to the registry implementation.

### SubprocessContainer

**Files:** `logos-container-subprocess: subprocess_container.h/cpp`

**Purpose:** Concrete `ModuleContainer` implementation for subprocess-based module isolation using Boost.Process v2 and Boost.Asio. Handles process lifecycle (spawn, monitor, kill) and auth-token delivery over the child's stdin pipe. Knows nothing about what type of module runs inside the subprocess.

- Uses `boost::process::v2::process` for subprocess spawning and `boost::asio::io_context` for async I/O
- Background `io_context` thread with work guard for non-blocking async read and wait callbacks
- Async read loop for stdout/stderr with line buffering
- Synchronous kill with graceful SIGTERM → SIGKILL escalation (5s timeout)
- **Token delivery over stdin.** `launch()` wires a stdin pipe the child inherits as fd 0 and appends `--token-source stdin` to the host's args; `sendToken()` writes the token (newline-framed) to the parent end and closes it. The pipe is private to the parent/child pair and has no filesystem name, so there is no predictable path for a co-tenant to squat and no peer to authenticate — this removes the CWE-940 / F-012 attack surface that the old predictable-socket handoff had to guard with `0600` nodes and `SO_PEERCRED`/`getpeereid` peer-credential gates (all now deleted).
- A `std::mutex` (`s_processesMutex`) protects the `s_processes` map against concurrent access

**ModuleContainer interface:** `id()` → `"subprocess"`, `canHandle()`, `launch()`, `sendToken()`, `terminate()`, `terminateAll()`, `hasModule()`, `pid()`, `getAllPids()`

**Static process management API (used by tests):**

| Method | Description |
|--------|-------------|
| `startProcess(name, executable, arguments, callbacks) → bool` | Launch a subprocess with async output monitoring and a stdin pipe for the token |
| `sendTokenToProcess(name, token) → bool` | Write the auth token (newline-framed) to the child's stdin pipe, then close it |
| `terminateProcess(name)` | Gracefully terminate a specific process |
| `terminateAllProcesses()` | Terminate all managed processes |
| `hasProcess(name) → bool` | Check if a process entry exists |
| `getProcessId(name) → int64_t` | Get PID for a named process |
| `getAllProcessIds() → unordered_map` | Map all process names to PIDs |
| `registerProcess(name)` | Register a placeholder process entry |
| `clearAll()` | Clear all process entries |

A test-only `SubprocessManager` header (`tests/subprocess_manager.h`) inherits from `CompositeModuleLoader` and forwards static methods to `SubprocessContainer`, so test code that references the old name keeps compiling. It lives in `tests/` (not `src/`) so the production module-loader layer stays free of a dependency on a specific container.

### QtPluginFormatLoader (external — logos-module-loader-qt)

**Files:** `logos-module-loader-qt: qt_plugin_format_loader.h/cpp` (consumed via `<logos_module_loader_qt/qt_plugin_format_loader.h>`)

**Purpose:** Concrete `ModuleFormatLoader` implementation for the Qt plugin module format. Resolves the `logos_host_qt` binary path (via `boost::dll`) and builds the CLI arguments (`--name`, `--path`, `--instance-persistence-path`, `--transport-set`) the host binary expects. `id()` returns `"qt-plugin"`. Lives in the external `logos-module-loader-qt` package (linked into `logos_core`); it is light (boost::dll + spdlog, Qt-free).

### CompositeModuleLoader

**Files:** `src/logos_core/composite_module_loader.h`, `src/logos_core/composite_module_loader.cpp`

**Purpose:** Implements the `ModuleLoader` interface by pairing a `ModuleContainer` (where/how to run) with a `ModuleFormatLoader` (what to load). The default registration in `ModuleManager` composes `CompositeModuleLoader(makeContainer(), makeFormatLoader())` — the two contract factory seams, whose concrete implementations are bound at link time (subprocess + Qt-plugin by default). The core never names the concrete types. `id()` returns `"qt-plugin+subprocess"`.

### ProcessStats (external dependency)

**Source:** [process-stats](https://github.com/logos-co/process-stats) library (linked as a static dependency)

**Purpose:** CPU and memory monitoring for loaded module processes.

**API (namespace `ProcessStats`):**

| Method | Description |
|--------|-------------|
| `getProcessStats(pid) → ProcessStatsData` | Return CPU %, CPU time, and memory usage for a process |
| `getModuleStats(processIds) → char*` | Return JSON array of stats for all provided module processes |

### LogosHost (external — logos-module-loader-qt)

**Files (in `logos-module-loader-qt`):** `host/logos_host.cpp`, `host/command_line_parser.h/cpp`, `host/token_source.h/cpp`, `host/module_initializer.h/cpp`, `host/qt/qt_app.h/cpp` — built as the `logos_host_qt` binary, which liblogos re-exports via `bin.nix`.

**Purpose:** Lightweight subprocess (`logos_host_qt`) that loads a single Qt module. Parses `--name`, `--path`, optional `--instance-persistence-path`, optional `--transport-set` (per-module `LogosTransportSet` JSON; empty = global-default LocalSocket only), and `--token-source` arguments. On startup, token receipt (container concern) is separated from module loading (loader concern): the host first reads its auth token from the channel named by `--token-source` (default stdin — see `TokenSource` below), then loads the Qt plugin and constructs the `LogosAPI` with the parsed transport set (explicit-transport ctor when provided, single-arg ctor otherwise). Registers the module with the remote object registry and runs the Qt event loop. A `logos_host` compatibility symlink is installed for backward compatibility with downstream consumers.

Crucially, the host depends on **no container package** — not `logos-container-subprocess`, not even `logos-container`. It reads its token from an OS handle (`TokenSource::read`, plain libc), so it is agnostic to which container spawned it.

### TokenSource (external — logos-module-loader-qt)

**Files (in `logos-module-loader-qt`):** `host/token_source.h/cpp`

**Purpose:** Container-agnostic auth-token reader for the host. `TokenSource::read(source)` reads the token from the channel the container designated via `--token-source`:

- `stdin` (default) — read fd 0; the subprocess container writes the token to the child's stdin pipe
- `fd:<n>` — read an inherited file descriptor
- `file:<path>` — read a file (e.g. a Docker secret mount)

The token is the first newline-framed line (or all bytes up to EOF), bounded by a `poll()` timeout so a never-delivered token can't hang the child. This menu is the extensibility seam: a Docker or sandbox container picks `file:`/`fd:` without any change to the host.

## C API

The public C API (`logos_core.h`) is the only exported interface. All functions use `LOGOS_CORE_EXPORT` for shared library visibility.

**Lifecycle:**

| Function | Description |
|----------|-------------|
| `logos_core_init(argc, argv)` | Initialize the library |
| `logos_core_start()` | Discover modules and initialize capability module |
| `logos_core_cleanup()` | Terminate all modules and clean up |

**Module Management:**

| Function | Description |
|----------|-------------|
| `logos_core_add_modules_dir(dir)` | Add a module directory to scan (duplicates ignored) |
| `logos_core_set_persistence_base_path(path)` | Set base directory for module instance persistence |
| `logos_core_set_module_transports(name, json)` | Register a per-module transport set (JSON, see logos-cpp-sdk shape). Forwarded to the child via `--transport-set` so its `LogosAPIProvider` binds every listener instead of only the global default LocalSocket. Must be called before the module is loaded; empty clears the entry |
| `logos_core_set_access_policy(json)` | Install the inter-module access policy (version + mode + per-target `allowedCallers` allowlists). Core parses it and registers the per-target restrictions with capability_module, which denies token issuance (and thus calls) for disallowed callers when `mode` is `enforce`. Under enforce, restrictions are also auto-derived from the dependency graph (a module may only call its declared dependencies; allowed callers = loaded dependents + trusted `core`/`core_service`, re-pushed on load/unload); an explicit entry overrides the derived set for that target. Call before modules load; NULL/empty clears it |
| `logos_core_load_module(name, with_dependencies) → int` | Load a module (1 = success, 0 = failure). When `with_dependencies` is true, resolves the dependency tree and loads in topological order |
| `logos_core_unload_module(name, with_dependents) → int` | Unload a module. When `with_dependents` is true, cascade unloads every loaded transitive dependent leaves-first. Returns 1 only if every step succeeded |
| `logos_core_get_module_dependencies(name, recursive) → char**` | Modules that `name` depends on (forward edges). `recursive=true` walks the forward graph transitively. Unknown names yield an empty array. Caller frees |
| `logos_core_get_module_dependents(name, recursive) → char**` | Modules that depend on `name` (reverse edges). `recursive=true` walks transitively. Unknown names yield an empty array. Caller frees |
| `logos_core_process_module(path) → char*` | Process module file, return name (caller frees) |
| `logos_core_refresh_modules()` | Re-scan module directories |

**Queries:**

| Function | Description |
|----------|-------------|
| `logos_core_get_loaded_modules() → char**` | Null-terminated array of loaded names (caller frees) |
| `logos_core_get_known_modules() → char**` | Null-terminated array of known names (caller frees) |
| `logos_core_get_module_stats() → char*` | JSON array of CPU/memory stats (caller frees) |
| `logos_core_get_token(key) → char*` | Get auth token by key (caller frees) |

### Thread Safety

| Category | Guarantee |
|----------|-----------|
| `logos_core_load_module`, `logos_core_unload_module` | Serialised by a single internal mutex — safe to call concurrently from multiple threads. The cascade variant (`with_dependents=true`) holds the lock for the entire leaves-first teardown so a late-arriving load can't interleave between tearing down the dependents and the target |
| `logos_core_get_known_modules`, `logos_core_get_loaded_modules` | Protected by a shared reader-writer lock — safe to call concurrently with each other and with the mutating functions above |
| `logos_core_refresh_modules` | Protected by `ModuleRegistry`'s reader-writer lock (write side) — safe for concurrent registry access but not serialised against load/unload |
| `logos_core_init`, `logos_core_start`, `logos_core_cleanup` | Not thread-safe — must be called from a single thread during startup/shutdown |

## Build Artifacts

| Artifact | Description |
|----------|-------------|
| `liblogos_core.{so,dylib,dll}` | Core shared library (C API) |
| `logos_host_qt` | Qt module subprocess host binary (re-exported from `logos-module-loader-qt`) |
| `logos_host` | Compatibility symlink → `logos_host_qt` |
| `logos_core_tests` | Google Test suite |

## Operational

### Nix (Recommended)

Nix provides reproducible builds with all dependencies managed automatically.

**Build everything (binaries + libraries + headers):**
```bash
nix build
```

The result includes:
- `result/bin/logos_host_qt` — Qt module host binary
- `result/bin/logos_host` — Compatibility symlink → `logos_host_qt`
- `result/lib/liblogos_core.{so,dylib}` — Core library
- `result/include/` — Headers (logos_core.h, interface.h)

**Build individual components:**
```bash
nix build '.#logos-liblogos-bin'       # Binaries only
nix build '.#logos-liblogos-lib'       # Libraries only
nix build '.#logos-liblogos-include'   # Headers only
nix build '.#logos-liblogos-tests'     # Test suite
nix build '.#logos-liblogos-modules'   # Built-in modules
nix build '.#portable'                 # Portable variant (LOGOS_PORTABLE_BUILD=ON)
```

**Run tests:**
```bash
nix build '.#logos-liblogos-tests'
./result/bin/logos_core_tests

# Run specific tests
./result/bin/logos_core_tests --gtest_filter=AppLifecycleTest.*

# List all tests
./result/bin/logos_core_tests --gtest_list_tests
```

**Development shell:**
```bash
nix develop
```

**Override local dependencies:**
```bash
nix build --override-input logos-cpp-sdk path:../logos-cpp-sdk
```

### CMake

**Prerequisites:**
- CMake 3.14+
- C++17 compatible compiler
- Qt 6 with Core, Network, and RemoteObjects modules
- Boost (with Process and Filesystem components)
- nlohmann_json
- OpenSSL (transitive: SDK's plain-C++ TCP+TLS transport)
- Google Test (fetched via FetchContent if not system-installed)

**Build:**
```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

`liblogos_core` will be in `build/lib/`. The `logos_host_qt` binary is built by the external `logos-module-loader-qt` package, not this CMake build; the nix `bin` output re-exports it (see below).

**Run tests:**
```bash
make -j$(nproc)
ctest --output-on-failure
```

**Note:** CMake expects the logos-cpp-sdk and logos-module libraries to be available. For a pre-built SDK it uses `find_package(logos-cpp-sdk)` (the package config carries OpenSSL / Boost / nlohmann_json / Qt as transitive deps); otherwise it falls back to the source tree (`cpp/CMakeLists.txt`).

### Dev vs Portable Builds

Controlled by the `LOGOS_PORTABLE_BUILD` CMake flag:

- **Dev** (default): Looks for LGX variants with `-dev` suffix (e.g., `linux-amd64-dev`)
- **Portable** (`-DLOGOS_PORTABLE_BUILD=ON`): Looks for variants without suffix (e.g., `linux-amd64`)

Build portable with Nix: `nix build '.#portable'`

## Consumers

`logos-liblogos` is a library consumed by two frontends:

- **[logos-basecamp](https://github.com/logos-co/logos-basecamp)** — Desktop GUI application shell
- **[logos-logoscore-cli](https://github.com/logos-co/logos-logoscore-cli)** — Headless CLI runtime (`logoscore`)

## Examples

### Basic C API Usage

```c
#include "logos_core.h"

int main(int argc, char *argv[]) {
    logos_core_init(argc, argv);
    logos_core_add_modules_dir("/path/to/modules");

    logos_core_start();
    logos_core_load_module("chat", false);

    char** loaded = logos_core_get_loaded_modules();
    for (int i = 0; loaded[i] != NULL; i++) {
        printf("Loaded: %s\n", loaded[i]);
    }
    free(loaded);

    logos_core_cleanup();
    return 0;
}
```

### Loading with Dependencies

```c
// Resolves the dependency tree and loads in correct order
logos_core_load_module("my_module", true);
```

## Continuous Integration

GitHub Actions workflow (`.github/workflows/ci.yml`) runs on every push/PR to `master`:

1. Checkout code
2. Install Nix with flakes enabled
3. Use cachix cache (`logos-co`)
4. Build: `nix build .#logos-liblogos-tests`
5. Run: `./result/bin/logos_core_tests`

## Supported Platforms

- macOS (aarch64-darwin, x86_64-darwin)
- Linux (aarch64-linux, x86_64-linux)
