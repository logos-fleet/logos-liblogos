# logos-liblogos

The core runtime library for the Logos modular application platform. Provides `liblogos_core` (a C-API shared library) and `logos_host` (the module subprocess host binary).

`logos-liblogos` is a **library**. It is consumed by two frontends:
- **[logos-basecamp](https://github.com/logos-co/logos-basecamp)** — the desktop GUI application shell
- **[logos-logoscore-cli](https://github.com/logos-co/logos-logoscore-cli)** — the headless CLI runtime (`logoscore`)

## Composed from

The runtime pulls its module-loading behaviour from a few separate repos so each
piece can be swapped independently:

- **[logos-capability-module](https://github.com/logos-co/logos-capability-module)** — issues the auth tokens that gate inter-module calls.
- **[logos-container](https://github.com/logos-co/logos-container)** — the container interface (*where/how* a module runs); current default is the subprocess implementation: **[logos-container-subprocess](https://github.com/logos-co/logos-container-subprocess)** (one OS process per module).
- **[logos-module-loader](https://github.com/logos-co/logos-module-loader)** — the format-loader interface (*what kind* of module); current default is : **[logos-module-loader-qt](https://github.com/logos-co/logos-module-loader-qt)**.

## How to Build

The project uses a Nix flake for reproducible builds with a modular structure:

#### Build Complete Library (Binaries + Libraries + Headers)

```bash
# Build everything (default)
nix build

# Or explicitly
nix build '.#logos-liblogos'
nix build '.#default'
```

The result will include:
- `/bin/` - Host binary (logos_host)
- `/lib/` - Core library (liblogos_core)
- `/include/` - Headers (logos_core.h, interface.h)

#### Build Individual Components

```bash
# Build only the binaries (outputs to /bin)
nix build '.#logos-liblogos-bin'

# Build only the libraries (outputs to /lib)
nix build '.#logos-liblogos-lib'

# Build only the headers (outputs to /include)
nix build '.#logos-liblogos-include'

# Build and run tests
nix build '.#logos-liblogos-tests'

# Build portable variant (selects portable LGX variants instead of dev)
nix build '.#portable'
```

#### Running Tests

```bash
# Build and run tests (tests run automatically during build)
nix build '.#logos-liblogos-tests'

# To run tests manually after building:
./result/bin/logos_core_tests

# Run specific tests
./result/bin/logos_core_tests --gtest_filter=AppLifecycleTest.*

# List all available tests
./result/bin/logos_core_tests --gtest_list_tests
```

#### Development Shell

```bash
# Enter development shell with all dependencies
nix develop
```

**Note:** In zsh, you need to quote targets with `#` to prevent glob expansion.

If you don't have flakes enabled globally, add experimental flags:

```bash
nix build '.#logos-liblogos' --extra-experimental-features 'nix-command flakes'
```

The compiled artifacts can be found at `result/`

#### Modular Architecture

The nix build system is organized into modular files in the `/nix` directory:
- `nix/default.nix` - Common configuration (dependencies, flags, metadata)
- `nix/build.nix` - Shared build that compiles everything once
- `nix/bin.nix` - Extracts binaries (logos_host, includes libraries for runtime linking)
- `nix/lib.nix` - Extracts libraries only
- `nix/include.nix` - Header installation
- `nix/tests.nix` - Test suite build and execution

**Note:** The `logos-liblogos-bin` package includes both the `logos_host` binary and its required libraries to ensure proper runtime linking.

#### Local Development

To build against a local checkout of a dependency, override its input. For example the SDK:

```bash
nix build --override-input logos-cpp-sdk path:../logos-cpp-sdk
```

The container and format-loader abstractions live in their own repos, consumed
as inputs the same way `process-stats` is. To build against local checkouts:

```bash
nix build \
  --override-input logos-container path:../logos-container \
  --override-input logos-module-loader path:../logos-module-loader \
  --override-input default-container path:../logos-container-subprocess \
  --override-input default-module-loader path:../logos-module-loader-qt
```

(`default-container` / `default-module-loader` are the input *slots* for the
built-in default implementations; they point at the subprocess / qt-plugin repos.)

## Library API

`logos-liblogos` exposes a C API via `logos_core.h`:

```c
// Lifecycle
void logos_core_init(int argc, char *argv[]);
void logos_core_start();
void logos_core_cleanup();

// Module directory management
void logos_core_add_modules_dir(const char* dir);

// Instance persistence
void logos_core_set_persistence_base_path(const char* path);

// Per-module transport configuration (forwarded to the module's
// child subprocess so its LogosAPIProvider binds every listener
// instead of only the global default LocalSocket). Must be called
// before the module is loaded.
void logos_core_set_module_transports(const char* name, const char* transport_set_json);

// Inter-module access policy (per-target allowed-caller allowlists).
// Core parses it and registers the per-target restrictions with
// capability_module, which then denies token issuance for disallowed
// (caller, target) pairs. Call before logos_core_start(); NULL/"" clears.
void logos_core_set_access_policy(const char* policy_json);

// Module management
int  logos_core_load_module(const char* name, bool with_dependencies);
int  logos_core_unload_module(const char* name, bool with_dependents);
char* logos_core_process_module(const char* path);
void logos_core_refresh_modules();

// Register a Bare module image that ships inside the HOST's own bundle, with
// its manifest handed over separately -- an iOS framework in
// <App>.app/Frameworks/, or an Android .so in the app's native library dir.
// Both are read-only, flat, and the only place their platform will dlopen
// from, so there is no room for a manifest beside the image. Returns the
// registered name; load it afterwards like any other module.
char* logos_core_add_bare_module(const char* metadata_json, const char* image_path);

// Dependency graph queries (forward + reverse edges; recursive walks BFS)
char** logos_core_get_module_dependencies(const char* name, bool recursive);
char** logos_core_get_module_dependents(const char* name, bool recursive);

// Module queries
char** logos_core_get_loaded_modules();
char** logos_core_get_known_modules();

// Module stats and tokens
char* logos_core_get_module_stats();
char* logos_core_get_token(const char* key);
```

See `src/logos_core/logos_core.h` for the full API.

### Inter-module access enforcement (off by default)

By default a loaded module may call any other loaded module. Enforcement is
opt-in, and `mode` in the access policy is the switch:

```jsonc
{"version": 1, "mode": "enforce"}
```

Installing that document (via `logos_core_set_access_policy`, before
`logos_core_start()`) turns on **deny-by-default**: for every loaded target,
core derives the allowed callers from the declared dependency graph — the
target's loaded dependents, plus the trusted `core` / `core_service` — and
registers them with capability_module. A module that never declared the target
as a dependency is refused a token, so its call can never proceed, and
capability_module logs the refusal with both names:

```
[capability_module] access policy denies 'caller_module' -> 'target_module'
```

Anything other than `mode: "enforce"` — no policy, `NULL`, `""`, unparseable
JSON, a different mode — leaves enforcement **off**, which is the pre-existing
behaviour. Core says which side it landed on at startup, so a mistyped mode is
visible rather than silently permissive:

```
Inter-module access enforcement is ON (mode=enforce): deny-by-default — ...
Inter-module access enforcement is OFF (no access policy set): ...
```

A `restrictions` entry overrides the derived list for that target verbatim,
which is the escape hatch for callers that legitimately cannot declare their
target (out-of-process `ui_qml` plugins are not tracked as dependents, so they
need an explicit entry):

```jsonc
{"version": 1, "mode": "enforce",
 "restrictions": {"accounts_module": {"allowedCallers": ["accounts_ui"]}}}
```

`capability_module`, `core` and `core_service` are never restricted as targets.

Hosts expose this as `--access-policy` — see the logoscore CLI and Basecamp
READMEs.

### Thread safety

Module load/unload operations (`logos_core_load_module`, `logos_core_unload_module`) are serialised internally by a single mutex. It is safe to call them concurrently from multiple threads, including rapid and repeated load/unload cycles on the same module — each call waits for its turn and the process management layer handles teardown cleanly before the next launch. `logos_core_unload_module` with `with_dependents=true` in particular holds the lock for its entire leaves-first teardown so a late-arriving load can't interleave between tearing down a dependent and its parent.

`logos_core_refresh_modules` is synchronised through the module registry's reader-writer lock — it is safe to call concurrently with other registry accesses, but it is **not** serialised against load/unload by the same mutex as above.

Read-only accessors (`logos_core_get_known_modules`, `logos_core_get_loaded_modules`) use that shared reader-writer lock and are safe to call concurrently with each other and with `logos_core_refresh_modules`.

## Module lifecycle observer

`src/logos_core/module_state_observer.h` turns lifecycle changes into
structured, sequenced facts. Until it existed, load/unload/crash were
`spdlog::info` lines and registry membership changes were silent, so every
consumer polled — `logos-basecamp` runs a 2s `QTimer` and infers module state
from package-install events.

It reports; it does not drive anything. Transitions go to a **sink**, and with
none installed `record()` early-outs before it allocates, so a host that
consumes nothing pays nothing. The sink that pushes to `modules_state` is in
`module_manager.cpp`.

States: `unloaded`, `loading`, `loaded`, `stopping`, `error`, plus the
event-only `absent`, which names the two membership edges (`absent -> unloaded`
on discovery, `unloaded -> absent` on prune).

Two rules govern every call site, and both are load-bearing:

- **Never dispatch under `loadMutex()`.** `record()` buffers; `flush()`
  dispatches, and entry points declare `ScopedModuleStateFlush` *before* their
  lock guard so it is destroyed *after* it. A sink doing an RPC from inside the
  load path while holding that lock is the shape of two failures already paid
  for here: the ui-host startup token deadlock, and a ~417s Basecamp stall from
  a synchronous call to an absent module.

- **One `seq` counter, for deltas and snapshots alike.** Consumers apply a
  transition only when its `seq` beats what they hold, and tombstone a departed
  record at a seq. A second counter makes that tombstone unreachably high (a
  real later delta dropped forever) or trivially low (a stale delta resurrecting
  a pruned module).

`onTerminated` fires for both an orderly unload and a module that died, so
teardown announces intent before `terminate()` and the callback consumes it.
Host shutdown announces every loaded module first — without that, a clean exit
reports the whole fleet as crashed.

## Dev vs Portable Builds

The library supports two build modes controlled by the `LOGOS_PORTABLE_BUILD` CMake flag:

- **Dev build** (default): Module loading looks for LGX variants with `-dev` suffix (e.g., `linux-amd64-dev`). Used in Nix/development environments.
- **Portable build** (`-DLOGOS_PORTABLE_BUILD=ON`): Looks for portable variants without suffix (e.g., `linux-amd64`). Used in self-contained distributed applications.

Build the portable variant with `nix build '.#portable'`.

## Supported Platforms
- macOS (aarch64-darwin, x86_64-darwin)
- Linux (aarch64-linux, x86_64-linux)
- iOS and Android, as a cross-built `liblogos_core` only (see below)

## Mobile (iOS, Android)

`liblogos_core` and the eight repos it links are cross-built from source by
`nix/mobile/ios.nix` (static archives; Qt for iOS is static) and
`nix/mobile/android.nix` (shared objects), on logos-nix's mobile
pseudo-systems. They are exposed three ways:

```bash
# Flake packages, keyed by target (Android's build platform is x86_64-linux here)
nix build .#packages.aarch64-ios-simulator.default
nix build .#packages.aarch64-ios.logos-protocol

# The same, keyed by the platform that BUILDS them -- how a Mac asks for Android
nix build .#legacyPackages.aarch64-darwin.mobile.aarch64-android.default
```

A consumer that links the whole set (an app) uses `lib.mkMobileChains
{ androidBuildSystem ? "x86_64-linux" }`, which returns one chain per target
with every stage, `all` (the complete list of prefixes to link) and `pkgs`
(the package set the chain was built from). logos-basecamp's
`mobile/liblogos-smoke` is the reference consumer.

## Building with a different container or module loader

The container and format-loader are selected by which package liblogos links —
not by its C++ or CMake. To use your own, build a package that:

- implements the contract interface (`LogosCore::ModuleContainer` from
  `logos-container`, or `LogosCore::ModuleFormatLoader` from `logos-module-loader`),
- defines the factory symbol (`LogosCore::makeContainer()` /
  `LogosCore::makeFormatLoader()`), and
- ships the generic CMake config (`LogosContainerImpl` / `LogosFormatLoaderImpl`,
  exposing the `…::impl` target).

(Copy the structure from `logos-container-subprocess` / `logos-module-loader-qt`.)
Then point liblogos at it by overriding the input slot:

```bash
# different container
nix build '.#logos-liblogos' --override-input default-container <flake-ref>
# different format loader
nix build '.#logos-liblogos' --override-input default-module-loader <flake-ref>
```

`<flake-ref>` is e.g. `github:you/your-impl` or `path:../your-impl`; it must expose
`packages.<system>.default` carrying that config + factory symbol. For a permanent
default, add it as an input and set `containerImpl` / `formatLoaderImpl` in
`flake.nix`. Container and loader are independent — swap either or both.

Note: the format-loader package also provides the `logos_host` binary that
`bin.nix` re-exports, so a replacement loader must ship its own host binary.

## Disclaimer
This repository is part of an experimental development environment. Components are under active development and may be incomplete, unstable, modified or discontinued at any time.

The software is provided for development and testing purposes only and is not intended for production use. 

The code and related materials are made available on an open-source, “as-is” basis without warranties or guarantees of any kind, express or implied, including warranties of correctness, security, performance or fitness for a particular purpose. Use at your own risk.
