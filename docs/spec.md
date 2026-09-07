# Logos Core Library

## Overall Description

Logos Core is a modular runtime platform for hosting and orchestrating independently developed modules. It provides a C-API shared library (`liblogos_core`) and a pluggable module-loading system that together enable a module-based architecture for decentralised applications. The module-loading system separates two orthogonal concerns — **containers** (how a module process is launched, managed, and communicates) and **module format loaders** (how a specific type of module binary is resolved and loaded). These are composed into a `CompositeModuleLoader` that implements the `ModuleLoader` interface. The default configuration pairs a `SubprocessContainer` (Boost.Process v2) with a `QtPluginFormatLoader` (Qt plugin format loader) to spawn a `logos_host_qt` process per module.

Both abstractions are split across repositories so each side is swappable. Each has a Qt-free header-only **contract** package and a separate **implementation** package:

- **Container** (where/how a module runs): contract in [`logos-container`](https://github.com/logos-co/logos-container) (the `ModuleContainer` interface + the `ModuleDescriptor` / `LoadedModuleHandle` value types); subprocess implementation in [`logos-container-subprocess`](https://github.com/logos-co/logos-container-subprocess).
- **Format loader** (what format a module is): contract in [`logos-module-loader`](https://github.com/logos-co/logos-module-loader) (the `ModuleFormatLoader` interface); Qt-plugin implementation — `QtPluginFormatLoader` plus the `logos_host_qt` host binary — in [`logos-module-loader-qt`](https://github.com/logos-co/logos-module-loader-qt).

`liblogos_core` consumes all four and re-exports the `logos_host_qt` binary so frontends are unaffected by where it is built. The `ModuleLoader` base, the `CompositeModuleLoader` / `ModuleLoaderRegistry` orchestration, and the logging foundation are core concerns and stay in `liblogos_core`. The core names no concrete implementation: it builds its default loader through the contract factory seams `makeContainer()` / `makeFormatLoader()`, whose definitions are supplied by whichever implementation library the build links in. Because each implementation depends *down* onto its contract and the contracts don't depend on the core, a different container (Docker, in-process) or format loader (WASM, native) is added by writing a new package that defines the same factory symbol — chosen at link time, without modifying the core.

The platform is designed to:
- Load, start, stop, and introspect modules at runtime
- Isolate each module in its own process for robustness and security
- Provide transparent inter-module RPC via a remote object registry
- Support token-based authentication for secure module-to-module communication
- Expose a C API so that host applications in any language can drive the runtime
- Support diverse container strategies (subprocess, Docker, in-process) and module formats (Qt plugin, WASM) through composable abstractions

## Definitions & Acronyms

| Term | Definition |
|------|------------|
| **Module** | An independently developed module that implements `PluginInterface` and is dynamically loaded by the core |
| **Core Library** | `liblogos_core` — the shared library that provides the C API for module management |
| **Module Host** | `logos_host_qt` — a lightweight executable that loads a single Qt module in its own process (a `logos_host` symlink exists for backward compatibility) |
| **Module Container** | An abstract interface (`ModuleContainer`) that defines how a module's execution environment is managed — process lifecycle, I/O, and credential delivery (e.g. subprocess, Docker, in-process) |
| **Module Format Loader** | An abstract interface (`ModuleFormatLoader`) that defines how a specific type of module binary is resolved and prepared for loading — host binary resolution and CLI argument construction (e.g. Qt plugin, WASM) |
| **Module Loader** | An abstract interface (`ModuleLoader`) that encapsulates a complete strategy for loading and managing modules. Implemented by `CompositeModuleLoader`, which pairs a `ModuleContainer` with a `ModuleFormatLoader` |
| **Composite Module Loader** | A concrete `ModuleLoader` that composes a `ModuleContainer` and a `ModuleFormatLoader`, delegating process lifecycle to the container and module-type resolution to the format loader |
| **Module Loader Registry** | The central registry (`ModuleLoaderRegistry`) that selects the appropriate `ModuleLoader` for a given module based on its format descriptor |
| **Core Manager** | A built-in module that exposes core functionality via RPC, allowing modules to manage the core without linking against the C API |
| **Capability Module** | A built-in module that handles authorization tokens for inter-module communication |
| **RPC** | Remote Procedure Call — the mechanism by which modules invoke methods on each other |
| **IPC** | Inter-Process Communication — the underlying transport |
| **Token** | A UUID-based authentication credential issued by the core or capability module for securing RPC calls |
| **SDK** | The [logos-cpp-sdk](https://github.com/logos-co/logos-cpp-sdk) — client library that abstracts connection management, token handling, and asynchronous invocation |

## Domain Model

### System Architecture

At a high level, the Logos Core consists of:

**Core Library** — The C/C++ shared library (`liblogos_core`) that provides the API functions for lifecycle management, module loading/unloading, and introspection.

**Module Loader Registry** — A central registry of `ModuleLoader` implementations. When a module is loaded, the registry selects the first loader whose `canHandle()` returns true for the module's descriptor. This decouples the core from any specific loading mechanism.

**Module Containers** — Pluggable implementations of the `ModuleContainer` interface that define the execution environment for modules. The interface itself lives in the standalone [`logos-container`](https://github.com/logos-co/logos-container) contract package, and each implementation is a separate package so the isolation mechanism can be swapped without touching the core. The default `SubprocessContainer` — provided by [`logos-container-subprocess`](https://github.com/logos-co/logos-container-subprocess) — spawns a separate process per module using Boost.Process v2, manages I/O pipes, and delivers the auth token over the child's stdin pipe. Future containers (Docker, in-process, sandboxed) are added by writing a sibling package that implements the same `ModuleContainer` interface.

**Module Format Loaders** — Pluggable implementations of the `ModuleFormatLoader` interface that define how a specific type of module binary is prepared for loading. The interface lives in the standalone [`logos-module-loader`](https://github.com/logos-co/logos-module-loader) contract package, and each implementation is a separate package. The default `QtPluginFormatLoader` — provided by [`logos-module-loader-qt`](https://github.com/logos-co/logos-module-loader-qt) along with the `logos_host_qt` host binary it resolves — constructs the CLI arguments the host expects. Future format loaders (WASM/Extism, native shared libraries, scripting runtimes) are added by writing a sibling package implementing the same interface.

**Composite Module Loader** — A `ModuleLoader` implementation that composes a `ModuleContainer` with a `ModuleFormatLoader`. Its `load()` method first asks the format loader to resolve the host binary and build arguments, then delegates process launch to the container. All other operations (sendToken, terminate, hasModule, pid) are forwarded to the container. The default composite loader pairs `SubprocessContainer` with `QtPluginFormatLoader` — but the core does not name those types: it obtains them through the contract factory seams `makeContainer()` / `makeFormatLoader()`, whose definitions the build links in (see Future Work / the `loaderRegistry()` construction site).

**Core Manager** — A built-in module that runs in the core process and exposes core functionality as RPC methods, allowing remote modules to manage the core without linking against the C API directly.

**Module Host** — A lightweight executable (`logos_host`) that loads a single module in its own process. On startup it first reads an authentication token from the channel its container designated via `--token-source` (default: stdin), then loads the Qt plugin and initializes `LogosAPI` with that token. The host is agnostic to which container spawned it — it just reads bytes from an OS handle — so container concerns (credential delivery) stay independent from loader concerns (plugin loading).

**Capability Module** — A built-in module that handles authorization for inter-module communication by issuing tokens and notifying both communicating parties.

**Remote Object Registry** — A registry that maintains a mapping of module names to remote object replicas and forwards method calls/events.

### Process Architecture

Each module runs in its own process for isolation:

```
┌──────────────────────────────────────────────────────┐
│  Host Application                                    │
│  ┌────────────────────────────────────────────────┐  │
│  │  liblogos_core                                 │  │
│  │  ├─ ModuleLoaderRegistry                       │  │
│  │  │   └─ CompositeModuleLoader (default)         │  │
│  │  │       ├─ SubprocessContainer (container)     │  │
│  │  │       └─ QtPluginFormatLoader (loader)       │  │
│  │  ├─ Core Manager (built-in module)             │  │
│  │  ├─ Capability Module (built-in module)        │  │
│  │  └─ Remote Object Registry                     │  │
│  └────────────────────────────────────────────────┘  │
│           │ IPC (local socket)                        │
│     ┌─────┼─────────┐                                │
│     ▼     ▼         ▼                                 │
│  ┌─────┐ ┌─────┐ ┌─────┐                             │
│  │host │ │host │ │host │  (logos_host_qt processes)   │
│  │mod A│ │mod B│ │mod C│                               │
│  └─────┘ └─────┘ └─────┘                             │
└──────────────────────────────────────────────────────┘
```

- The core uses a `ModuleLoaderRegistry` to select the appropriate `ModuleLoader` for each module
- The default `CompositeModuleLoader` pairs a `SubprocessContainer` with a `QtPluginFormatLoader`
- `SubprocessContainer` manages process lifecycle (spawn, terminate, token delivery) using Boost.Process v2
- `QtPluginFormatLoader` resolves the `logos_host_qt` binary and builds CLI arguments for Qt plugin modules
- Modules with `format == "qt-plugin"` or no explicit format are handled by the default composite loader
- Communication happens via the Logos API. Each module's transport set is configured per-module by the host: by default modules listen on a LocalSocket only, but the host can register a `LogosTransportSet` (LocalSocket, TCP, TCP+TLS) per module via `logos_core_set_module_transports` and the loader threads it through to the child via `--transport-set`
- Faulty or untrusted modules cannot crash the core or other modules
- Modules can be written in different languages as long as they implement the RPC protocol
- Alternative containers (Docker, in-process) and loaders (WASM, Extism) can be composed and registered

### Token-Based Authentication

Since the remote object registry has no built-in security mechanisms, all RPC calls require an authentication token. This is transparent to module developers when using the SDK:

1. **Core → Module**: When a module is loaded, the core generates a UUID token and sends it to the module process via the container's `sendToken()` mechanism. *How* the token reaches the child is the container's private business; the host just reads its token from the channel the container designates via `--token-source` (see [Token delivery channel](#token-delivery-channel)). For the subprocess container this channel is the child's **stdin**: the parent writes the token (newline-framed) to a pipe the child inherits as fd 0, then closes it. The host reads its token from stdin before initializing `LogosAPI`. The module uses this token to authenticate calls from the core.
2. **Module → Module**: When modules need to communicate, they request authorization from the Capability Module, which issues a token and notifies both parties. The modules then use this token for subsequent requests.
3. **Token Storage**: Each module stores tokens in a thread-safe `TokenManager` (part of the SDK). `ModuleProxy` validates tokens before dispatching method calls.

#### Token delivery channel

Token delivery is the container's responsibility, and the host is agnostic to it. The host reads its token from an OS handle named by `--token-source`:

- `stdin` — read fd 0 (the default; the subprocess container writes the token to the child's stdin pipe)
- `fd:<n>` — read an inherited file descriptor
- `file:<path>` — read a file (e.g. a Docker secret mount)

This keeps the host (`logos_host`) free of any dependency on a specific container — it depends on neither `logos-container-subprocess` nor `logos-container`, just libc. A future Docker or sandbox container delivers the token through its own channel and names it here, with no change to the host. The menu is the extensibility seam.

**Security — why an inherited pipe, not a named socket.** The token used to be handed off over a Unix-domain socket at a predictable path under the world-writable `$TMPDIR` (`/tmp/logos_token_<name>`). Because the path was guessable and the directory world-writable, a same-host co-tenant could pre-bind it and race the parent to intercept or inject the token (CWE-940 / F-012) — which is why the old code carried owner-only (`0600`) socket nodes plus mutual `SO_PEERCRED` / `getpeereid` peer-credential gates on both ends.

The subprocess container now delivers the token over the child's **stdin pipe** instead. That pipe has no name in the filesystem and is inherited only by the process the parent spawned, so there is nothing for a co-tenant to squat and no peer to authenticate — the attack surface is removed by construction rather than guarded. The predictable socket, the peer-credential hardening, and the separate child-side receiver are all gone. (This is the `socketpair()`-style fd handoff the previous revision of this spec named as the endgame; stdin is the same private-inherited-pipe idea using fd 0.)

### Module Metadata

Every module ships a `metadata.json` referenced by Qt's `Q_PLUGIN_METADATA` macro. Required fields:

| Field | Purpose |
|-------|---------|
| `name` | Unique module identifier. **Enforced**, not advisory: it must match the installed package name at discovery time and the plugin's `name()` return value at load time — a mismatch on either is refused (see Discovery and Loading). |
| `version` | Semantic version string |
| `description` | Human-readable description |
| `author` | Module author or organization |
| `type` | Module type (e.g., `"core"`) |
| `category` | Module category for organization |
| `main` | Main module class name |
| `dependencies` | Array of required module names |
| `capabilities` | Array of capabilities this module provides |
| `include` | Optional array of extra files (shared libs, resources) to bundle |

### Module name validation

A module's `name` originates from its embedded plugin metadata, which is **untrusted input** — a malicious installed `.lgx` can declare any name. That name is used as the registry map key, the RPC target, and a single filesystem path segment for the instance-persistence directory (`basePath/<name>/<instanceId>`).

**The attack (CWE-22):** a malicious module declares `name='../<x>'`. The `/` or `..` escapes the intended directory when the name is used as a path segment (e.g. instance persistence), or collides with another module's registry key / RPC identity.

The core therefore validates the name against an allowlist **at the registry trust boundary** (`ModuleRegistry::processModuleInternal`), so every downstream consumer inherits the guarantee and an unsafe name never enters the registry in the first place. The rule (`logos::isValidModuleName`, declared in `module_registry.h`):

- non-empty and at most 64 bytes;
- every byte is `[A-Za-z0-9_-]` — rejecting `/`, `\`, whitespace, NUL, `.` and any other separator;
- `.` and `..` are rejected outright.

A module whose name fails validation is dropped during discovery (logged and skipped), never registered, and never loaded.

## Features & Requirements

### Module Lifecycle

#### Discovery

1. Core scans configured module directories for `.so`, `.dylib`, or `.dll` files
2. For each file, metadata is extracted via `QPluginLoader`
3. A module's **identity is bound to the trusted package name** (the `manifest.json` name the package manager scanned and dedupes on), not to the name a plugin embeds in its own metadata. If a plugin's embedded `name` disagrees with its package name, the plugin is **refused** (not registered under either name). This prevents a package installed under an innocuous name from shipping a binary that claims a privileged identity (e.g.  `capability_module`) and inheriting that module's token/trust relationships.
4. The name is validated against the module-name allowlist (see [Module name validation](#module-name-validation)); modules with an invalid name are logged and skipped
5. Modules are added to the "known" list without being loaded
6. Multiple module directories can be configured

The module **name** comes from untrusted plugin JSON metadata and later becomes the registry key, the RPC target, and a filesystem path segment for the instance-persistence directory. It is therefore validated against an allowlist at the trust
boundary: during processing (`ModuleRegistry::processModuleInternal`) a module whose name is not a valid identifier — empty, `.`, `..`, longer than 64 bytes, or containing any byte outside `[A-Za-z0-9_-]` (so any path separator, whitespace or embedded NUL) — is rejected and never added to the registry. This prevents a crafted name such as `seg/../victim` from escaping the persistence directory or colliding with another module's identity. See `logos::isValidModuleName` (declared in `src/logos_core/module_registry.h`, defined in `module_registry.cpp`) and the [Module name validation](#module-name-validation) section.

#### Loading

1. Core locates the module file for the requested module name
2. Core resolves dependencies and loads them first (topological sort with circular dependency detection)
3. If a persistence base path is configured, core resolves an instance ID and persistence directory for the module (reusing an existing instance or creating a new one)
4. Core builds a `ModuleDescriptor` (name, path, format, module dirs, persistence path, transport-set JSON if registered via `logos_core_set_module_transports`)
5. Core asks the `ModuleLoaderRegistry` to `select()` a loader for the descriptor (the default `CompositeModuleLoader` handles `"qt-plugin"` format and modules with no explicit format)
6. The selected loader's `load()` is called:
   a. The `ModuleFormatLoader` resolves the host binary (e.g. `logos_host_qt`) and builds CLI arguments (including `--transport-set` if configured)
   b. The `ModuleContainer` launches the process with the resolved binary and arguments, appending its own `--token-source` so the child knows where to read its token (the subprocess container appends `--token-source stdin`)
7. Core generates a UUID authentication token
8. Core sends the token to the module via the loader's `sendToken()` (delegates to the container; the subprocess container writes it to the child's stdin pipe — see Token-Based Authentication)
9. Host process reads the token from the designated channel (`TokenSource`, default stdin — a container concern, but resolved generically with no container dependency), then loads the module plugin and calls `initLogos(LogosAPI*)` (loader concern). As a defense-in-depth identity check, the host **refuses to initialize** the plugin if its `name()` does not match the name it was loaded as (the trusted registry key passed by the core) — a binary cannot run, or receive tokens, under a name it does not implement
10. The `LogosAPI` instance exposes `modulePath`, `instanceId`, and `instancePersistencePath` as properties
11. Host process registers the module with the remote object registry
12. Core waits for registration and records the module as loaded (along with the loader and handle)

#### Unloading

1. The module's host process is terminated
2. The module is removed from the loaded modules list
3. Associated tokens and state are cleaned up

#### Cascade Unloading

`logos_core_unload_module(name, true)` unloads the named module together with every currently loaded module that transitively depends on it. Teardown order is leaves-first (dependents before dependencies) so no process is left briefly pointing at a terminated parent. The call is serialised with ordinary load/unload operations under a single lock span — a late-arriving load cannot interleave between tearing down the dependents and the target.

### Dependency Resolution

- Dependencies are declared in each module's `metadata.json`
- `logos_core_load_module(name, true)` performs topological sort
- Circular dependencies are detected and cause the load to fail (returns 0)
- Missing/unknown dependencies cause the load to fail (returns 0)
- The resolver itself (`DependencyResolver::resolve`) returns a `ResolveResult` containing the partial topological order, a list of missing dependency names, and a cycle flag. The load path treats any resolution error as a hard failure; the teardown path (`unloadModuleWithDependents`) uses the partial order best-effort
- Dependencies are loaded in correct order before the requesting module
- The core maintains an in-process dependency graph with both forward and reverse edges. The reverse edges are re-derived from the forward edges at the tail of every discovery or metadata-processing pass, so cascade unload and dependent queries answer from memory without re-reading manifests from disk.

#### Optional dependencies

`metadata.json#optional_dependencies` is a SECOND edge set: concrete modules a module can call but does not require. The registry keeps it, and its reverse edges, apart from `dependencies` — every statement above is about the required set, and each of the differences below is a place where merging them would be wrong.

- **Not loaded.** `logos_core_load_module(name, true)` resolves and loads the required tree only. An optional dependency is never pulled in, so the caller (an app, `logoscore -l`, a package manager) owns its lifetime.
- **Not a failure.** An optional dependency that is absent or unknown never appears in `ResolveResult::missing` and never fails a load.
- **Ordering only, and only when it can.** `DependencyResolver::resolve` takes the optional edges as SOFT edges: they order modules already in the set — so an optional dependency requested in the same batch comes up first and the dependent's startup calls land — but they never expand it. A soft edge that would close a cycle is dropped, and `hasCycle` continues to reflect the required edges alone: breaking a cycle is what optional dependencies are for, so reporting one as a cycle would refuse the configuration the feature exists to allow.
- **No cascade.** `unloadModuleWithDependents` walks required reverse edges only. A module that declared it tolerates absence is not taken down when the thing it tolerates goes away.
- **Still a caller.** Under `mode: "enforce"`, a loaded optional dependent IS in a target's derived allowed-caller list. The declaration is what grants the right to call; whether the loader had to supply the target is a separate question. Omitting them would deny a declared call between two loaded modules, and the caller would see a default value rather than an error.

`logos_core_get_module_optional_dependencies(name)` reads the set. There is no `recursive` form: an optional edge says nothing about what lies beyond it.

### Process Monitoring

- CPU percentage, CPU time, and memory usage tracked per module process
- Statistics returned as JSON via `logos_core_get_module_stats()`
- Core Manager process is excluded from stats
- Not available on iOS

### Thread Safety

The C API is designed to be safe for use from multi-threaded host applications:

- **Load/unload operations** (`load_module`, `unload_module`) are serialised — only one runs at a time, so rapid concurrent load/unload cycles on the same or different modules do not produce data races. The cascade variant (`with_dependents=true`) holds the lock for its full leaves-first teardown.
- **Read-only queries** (`get_known_modules`, `get_loaded_modules`) use a shared reader-writer lock and may execute concurrently with each other and with load/unload operations.
- **Module discovery** (`refresh_modules`) is protected by the registry's own write lock.
- **Lifecycle functions** (`init`, `start`, `cleanup`) are not thread-safe and must be called from a single thread.

### Dev vs Portable Builds

The platform supports two build variants:

- **Dev build** (default): Module loading looks for LGX variants with `-dev` suffix (e.g., `linux-amd64-dev`). Used in Nix/development environments.
- **Portable build**: Looks for portable variants without suffix (e.g., `linux-amd64`). Used in self-contained distributed applications.

## API Description

### Core Lifecycle

| Function | Purpose |
|----------|---------|
| `logos_core_init(argc, argv)` | Initialize global state, optionally set module directory. Creates a QCoreApplication if one does not exist. |
| `logos_core_add_modules_dir(path)` | Add a module directory to scan (duplicates ignored). |
| `logos_core_start()` | Scan module directories, process metadata, create Core Manager, load built-in modules, start remote object registry. |
| `logos_core_cleanup()` | Unload all modules, stop processes, clean up global state. |

### Module Management

| Function | Purpose |
|----------|---------|
| `logos_core_get_loaded_modules() → char**` | Return null-terminated array of loaded module names. Caller must free. |
| `logos_core_get_known_modules() → char**` | Return null-terminated array of all discovered modules. Caller must free. |
| `logos_core_load_module(name, with_dependencies) → int` | Load a module by name. When `with_dependencies` is true, resolves the dependency tree and loads in topological order. Returns 1 on success, 0 on failure. |
| `logos_core_unload_module(name, with_dependents) → int` | Terminate the module's process and remove it. When `with_dependents` is true, cascade unloads every loaded transitive dependent leaves-first. Returns 1 only if every step succeeded. |
| `logos_core_get_module_dependencies(name, recursive) → char**` | Return null-terminated array of modules that `name` depends on (forward edges). With `recursive=true`, walks the forward dependency graph transitively via BFS. Unknown names yield an empty array. Caller must free. |
| `logos_core_get_module_dependents(name, recursive) → char**` | Return null-terminated array of modules that depend on `name` (reverse edges). With `recursive=true`, walks the reverse dependency graph transitively via BFS. Unknown names yield an empty array. Caller must free. |
| `logos_core_process_module(path) → char*` | Read a module file's metadata and register it as known without loading. Returns the module name or NULL. Caller must free. |
| `logos_core_set_module_transports(name, json)` | Register a per-module `LogosTransportSet` (JSON, see logos-cpp-sdk shape) for the named module. The loader forwards it to the child via `--transport-set` so the child's `LogosAPIProvider` binds every transport instead of only the global default LocalSocket. Must be called before the module is loaded. NULL or empty clears any previously-registered entry. |
| `logos_core_set_access_policy(json)` | Install the inter-module access policy: a JSON document with `version`, `mode` (e.g. `enforce`), and `restrictions` mapping each target module to its `allowedCallers` allowlist. Core parses it and, once capability_module loads, registers the concrete per-target restrictions with it via `registerRestriction` (authenticated by capability_module's auth token, so only the trusted core channel can register or relax restrictions — a peer module cannot); capability_module then refuses to mint a token (in `requestModule`) for a caller not in a restricted target's allowlist, so the call can never proceed. Only `mode: "enforce"` activates gating. **Under an enforce policy, restrictions are also derived automatically from the dependency graph** — a module may only call modules it declared as a dependency, so for each loaded target core registers its loaded dependents plus a trusted set (`core`, `core_service`) as the allowed callers (re-pushed on every load/unload). An explicit `restrictions` entry overrides the derived set for that target verbatim. Call before modules load. NULL or empty clears any previously-set policy. |

### Token and Monitoring

| Function | Purpose |
|----------|---------|
| `logos_core_get_token(key) → char*` | Return the auth token for a key. Caller must free. NULL if not found. |
| `logos_core_get_module_stats() → char*` | Return JSON array of CPU/memory stats per loaded module. Caller must free. Not available on iOS. |

### Core Manager Module (RPC Surface)

The Core Manager is a built-in module exposing core functionality to remote modules:

| Method | Purpose |
|--------|---------|
| `setModulesDirectory(directory)` | Set the module search directory. |
| `start()` | Start the core's registry and load built-in modules. |
| `cleanup()` | Unload all modules and shut down. |
| `getLoadedModules() → std::vector<std::string>` | Return names of loaded modules. |
| `getKnownModules() → QJsonArray` | Return all known modules with `loaded` flag. |
| `loadModule(name) → bool` | Load a module by name. |
| `unloadModule(name) → bool` | Unload a module by name. |
| `processModule(filePath) → std::string` | Read a module file's metadata and register it. |
| `getModuleMethods(name) → QJsonArray` | Introspect a module's methods via Qt meta-object system. |

## Module Implementation

A complete module must implement the Logos Module interface.

### Inter-Module Communication

Modules communicate using the SDK's generated C++ wrappers:

```
logos-><module_name>.<method>(args...)       // call a remote method
logos-><module_name>.on("event", callback)   // subscribe to events
logos-><module_name>.trigger("event", data)  // emit an event
```

The SDK abstracts away registry lookup, token management, and async invocation.

## Supported Platforms

- macOS (aarch64-darwin, x86_64-darwin)
- Linux (aarch64-linux, x86_64-linux)

## Future Work

- **Signature support** — Signing and verifying module packages. Required to fully close the *Module Identity & Trust* residual: enforcing REQUIRE-mode signature checks for reserved/privileged module names so a malicious package cannot claim a privileged name (e.g. `capability_module`) by winning scan-time dedup from a user-writable directory
- **Additional containers and format loaders** — Register alternative `ModuleContainer` implementations (Docker, in-process, sandboxed) or `ModuleFormatLoader` implementations (WASM, native) and compose them. Each contract lives in its own package ([`logos-container`](https://github.com/logos-co/logos-container), [`logos-module-loader`](https://github.com/logos-co/logos-module-loader)) and each implementation in a sibling package ([`logos-container-subprocess`](https://github.com/logos-co/logos-container-subprocess), [`logos-module-loader-qt`](https://github.com/logos-co/logos-module-loader-qt)), so a new one is a standalone package implementing the same interface — no change to `liblogos_core`. **This already works additively today:** a frontend (or any embedder) can register its own loader via `ModuleManager::loaders().registerLoader(...)` before `logos_core_start()`, and a module can pin one by id with `loaderConfig["id"]`. The new loader composes with the built-in default.
- **Frontend-owned loader registration (decoupling the default)** — `liblogos_core` names no concrete loader in its C++ *or* its CMake: it composes its default from the contract factory seams `makeContainer()` / `makeFormatLoader()` (declared in `logos-container` / `logos-module-loader`), and `src/CMakeLists.txt` discovers the implementation via `find_package(LogosContainerImpl)` / `find_package(LogosFormatLoaderImpl)`. Each implementation package ships that generic **CMake config package**, which carries its library and its own deps — so the core knows none of them. **Which implementation is the default is chosen in nix**: `nix/default.nix` puts the `containerImpl` / `formatLoaderImpl` packages (default subprocess + qt, wired in `flake.nix`) on `CMAKE_PREFIX_PATH` — no build flags. Selecting a different default (e.g. a Docker container) is a one-line `flake.nix` change pointing at a package that provides the same config + factory symbol — no C++ and no CMake edit. The remaining future step is *runtime* selection: each frontend (logoscore-cli, basecamp) registers the loader(s) it wants at startup via `ModuleManager::loaders().registerLoader(...)`. Deferred deliberately — the build-time default lets frontends work with zero startup wiring until a second implementation exists. See the `loaderRegistry()` construction site in `module_manager.cpp`.
- **Additional loaders** — Register alternative `ModuleLoader` implementations (e.g. WASM/Extism, native shared libraries) that can be composed with any container
- **Cross-language modules** — Modules in languages other than C++
- **Move away from Qt** — Logos API will move away from Qt. Process management has been migrated from Qt (`QProcess`) to Boost.Process v2, and Qt container/utility types (`QString`, `QStringList`, `QHash`, `QDir`, `QFile`, `QUuid`) have been replaced with standard C++ and Boost equivalents (`std::string`, `std::vector`, `std::unordered_map`, `std::filesystem`, `boost::uuids`). The container/loader separation (`ModuleContainer` / `ModuleFormatLoader` / `CompositeModuleLoader` / `ModuleLoaderRegistry`) decouples the core from any specific loading or execution strategy. Remaining Qt dependencies (event loop, module loading, remote objects) are isolated in `QtPluginFormatLoader` and the `logos_host_qt` binary.
