{
  description = "Logos liblogos core library";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
    logos-cpp-sdk.url = "github:logos-co/logos-cpp-sdk";
    logos-cpp-sdk.inputs.logos-protocol.follows = "logos-protocol";
    logos-protocol.url = "github:logos-co/logos-protocol";
    # ONE logos-protocol, and ONE logos-qt-host, in the closure. qt-host bakes
    # sizeof(LogosAPIClient) into its own `operator new` while logos-protocol
    # defines the constructor, so a second protocol here is an 8-byte heap
    # overrun on every getClient(), not a version disagreement. Without these,
    # an --override-input on our logos-protocol reaches only the direct edge and
    # leaves qt-sdk's and plugin-qt's copies behind.
    logos-qt-sdk.url = "github:logos-co/logos-qt-sdk";
    logos-qt-sdk.inputs.logos-protocol.follows = "logos-protocol";
    logos-qt-sdk.inputs.logos-plugin-qt.follows = "logos-plugin-qt";
    logos-plugin-qt.url = "github:logos-co/logos-plugin-qt";
    logos-plugin-qt.inputs.logos-protocol.follows = "logos-protocol";
    logos-capability-module.url = "github:logos-co/logos-capability-module";
    logos-modules-state-module.url = "github:logos-co/logos-modules-state-module";
    logos-module.url = "github:logos-co/logos-module";
    process-stats.url = "github:logos-co/process-stats";
    logos-container.url = "github:logos-co/logos-container";
    logos-module-loader.url = "github:logos-co/logos-module-loader";
    default-container.url = "github:logos-co/logos-container-subprocess";
    default-module-loader.url = "github:logos-co/logos-module-loader-qt";
    logos-package-manager.url = "github:logos-co/logos-package-manager";
  };

  outputs = { self, nixpkgs, logos-nix, logos-cpp-sdk, logos-protocol, logos-qt-sdk, logos-plugin-qt, logos-capability-module, logos-modules-state-module, logos-module, logos-package-manager, process-stats, logos-container, default-container, logos-module-loader, default-module-loader }:

    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
        logosSdk = logos-cpp-sdk.packages.${system}.default;
        logosProtocolPkg = logos-protocol.packages.${system}.default;
        logosQtSdk = logos-qt-sdk.packages.${system}.default;
        logosQtHost = logos-plugin-qt.packages.${system}.logos-qt-host;
        capabilityModule = logos-capability-module.packages.${system}.default;
        modulesStateModule = logos-modules-state-module.packages.${system}.default;
        logosModule = logos-module.packages.${system}.default;
        processStats = process-stats.packages.${system}.default;
        logosContainer = logos-container.packages.${system}.default;
        logosModuleLoader = logos-module-loader.packages.${system}.default;
        defaultContainer = default-container.packages.${system}.default;
        defaultModuleLoader = default-module-loader.packages.${system}.default;
        logosPackageManager = logos-package-manager.packages.${system}.lib;
        logosPackageManagerPortable = logos-package-manager.packages.${system}.lib-portable;
      });

      # Same as forAllSystems, plus the "x86_64-windows" pseudo-system. This
      # cannot just be logos-nix.lib.forAllTargets, because that only supplies
      # { system, pkgs } and this flake threads a dozen per-system dependencies
      # through.
      #
      # Every dependency below is a TARGET-side artifact (headers, archives, or
      # DLLs linked into logos_core, plus the host binary / plugin that are
      # merely re-exported). liblogos runs NO code generator at build time
      # (verified: no logos-cpp-generator / qt-generator anywhere in this repo),
      # so nothing here needs to come from the build platform's package set.
      #
      # Applied to `packages` ONLY: `checks` would have to execute PE test
      # binaries on the Linux builder, and a cross devShell offers no way to run
      # what it produces.
      windowsBuildSystem = "x86_64-linux";
      forAllTargets = f:
        nixpkgs.lib.genAttrs (systems ++ [ "x86_64-windows" ]) (system: f {
          inherit system;
          pkgs =
            if system == "x86_64-windows"
            then logos-nix.lib.mkWindowsPkgs { buildSystem = windowsBuildSystem; }
            else import nixpkgs { inherit system; };
          logosSdk = logos-cpp-sdk.packages.${system}.default;
          logosProtocolPkg = logos-protocol.packages.${system}.default;
          logosQtSdk = logos-qt-sdk.packages.${system}.default;
          logosQtHost = logos-plugin-qt.packages.${system}.logos-qt-host;
          capabilityModule = logos-capability-module.packages.${system}.default;
          modulesStateModule = logos-modules-state-module.packages.${system}.default;
          logosModule = logos-module.packages.${system}.default;
          processStats = process-stats.packages.${system}.default;
          logosContainer = logos-container.packages.${system}.default;
          logosModuleLoader = logos-module-loader.packages.${system}.default;
          defaultContainer = default-container.packages.${system}.default;
          defaultModuleLoader = default-module-loader.packages.${system}.default;
          logosPackageManager = logos-package-manager.packages.${system}.lib;
          logosPackageManagerPortable = logos-package-manager.packages.${system}.lib-portable;
        });
    in
    {
      packages = forAllTargets ({ pkgs, system, logosSdk, logosProtocolPkg, logosQtSdk, logosQtHost, capabilityModule, modulesStateModule, logosModule, processStats, logosContainer, logosModuleLoader, defaultContainer, defaultModuleLoader, logosPackageManager, logosPackageManagerPortable }:
        let
          # The built-in default container + format-loader implementations — the
          # single place the default is chosen. Each is just the package; it
          # ships a generic CMake config (LogosContainerImpl / LogosFormatLoaderImpl)
          # that carries its library and deps, which liblogos find_package's. Swap
          # an entry to change the default — no C++, CMake, or nix-flag change.
          containerImpl = defaultContainer;
          formatLoaderImpl = defaultModuleLoader;

          # Common configuration (dev, default)
          common = import ./nix/default.nix {
            inherit pkgs logosSdk logosProtocolPkg logosQtSdk logosQtHost logosModule processStats logosContainer logosModuleLoader logosPackageManager containerImpl formatLoaderImpl;
          };
          # Common configuration (portable)
          commonPortable = import ./nix/default.nix {
            inherit pkgs logosSdk logosProtocolPkg logosQtSdk logosQtHost logosModule processStats logosContainer logosModuleLoader containerImpl formatLoaderImpl;
            logosPackageManager = logosPackageManagerPortable;
            portableBuild = true;
          };
          src = ./.;

          # Shared build that compiles everything (dev)
          build = import ./nix/build.nix { inherit pkgs common src; };

          # Shared build (portable)
          buildPortable = import ./nix/build.nix { inherit pkgs src; common = commonPortable; };

          # Individual package components (reference the shared build)
          lib = import ./nix/lib.nix { inherit pkgs common build; };
          # The bundled set. capability_module is load-bearing at startup;
          # modules_state is the lifecycle registry the observer feeds, and it
          # is inert until something loads it -- the feed arms only when the
          # module itself loads.
          bundledModules = [
            { name = "capability_module"; pkg = capabilityModule;   version = "1.0.0"; }
            { name = "modules_state";     pkg = modulesStateModule; version = "0.1.0"; }
          ];
          modules = import ./nix/modules.nix { inherit pkgs common; modules = bundledModules; };
          modulesPortable = import ./nix/modules.nix {
            inherit pkgs;
            modules = bundledModules;
            common = commonPortable;
            portableBuild = true;
          };
          bin = import ./nix/bin.nix { inherit pkgs common build lib modules formatLoaderImpl; };
          include = import ./nix/include.nix { inherit pkgs common src logosSdk; inherit logosProtocolPkg logosQtSdk logosQtHost; };
          tests = import ./nix/tests.nix { inherit pkgs common build; };

          # Portable package components
          libPortable = import ./nix/lib.nix { inherit pkgs; common = commonPortable; build = buildPortable; };
          binPortable = import ./nix/bin.nix { inherit pkgs formatLoaderImpl; common = commonPortable; build = buildPortable; lib = libPortable; modules = modulesPortable; };
          includePortable = import ./nix/include.nix { inherit pkgs src logosSdk; inherit logosProtocolPkg logosQtSdk logosQtHost; common = commonPortable; };

          # Combined package (dev)
          #
          # propagatedBuildInputs is set HERE as well as on the headers output,
          # and that is not redundant: symlinkJoin builds a NEW derivation and
          # does not carry the propagation of the paths it joins. Consumers take
          # this join, not the headers output, so setting it only there reaches
          # nobody -- measured, logos-module-viewer still failed with
          #     fatal error: nlohmann/json.hpp: No such file or directory
          # until it was set on the join too.
          #
          # nlohmann is needed because this output re-exports the Qt host runtime
          # headers, two of which (logos_provider_object.h, logos_qt_arg_decode.h)
          # include <nlohmann/json.hpp>. Consumers going through
          # find_package(logos-qt-host) get it transitively; consumers taking the
          # include directory directly do not.
          liblogos = pkgs.symlinkJoin {
            name = "logos-liblogos";
            paths = [ bin lib include ];
            propagatedBuildInputs = [ pkgs.nlohmann_json ];
          };

          # Combined package (portable)
          liblogosPortable = pkgs.symlinkJoin {
            name = "logos-liblogos-portable";
            paths = [ binPortable libPortable includePortable ];
            propagatedBuildInputs = [ pkgs.nlohmann_json ];
          };
        in
        {
          # Individual outputs
          logos-liblogos-bin = bin;
          logos-liblogos-lib = lib;
          logos-liblogos-include = include;
          logos-liblogos-modules = modules;

          # Combined output
          logos-liblogos = liblogos;

          # Portable output (compiled with LOGOS_PORTABLE_BUILD)
          portable = liblogosPortable;

          # Default package (dev)
          default = liblogos;
        }
        # The test suite is POSIX-only (posix_spawn/waitpid/kill, /bin/sh) and
        # CMake gates it off for a Windows host, so `ninja logos_core_tests`
        # would have no such target. Not exposing the output at all beats
        # shipping one that cannot be built.
        // pkgs.lib.optionalAttrs (!pkgs.stdenv.hostPlatform.isWindows) {
          logos-liblogos-tests = tests;
        }
      );

      checks = forAllSystems ({ pkgs, system, defaultModuleLoader, ... }:
        let
          testsPkg = self.packages.${system}.logos-liblogos-tests;
          # Real Qt plugin used by RealPluginRegistryTest (TEST_PLUGIN env var).
          # capability_module is already a flake input and builds a real plugin.
          capabilityModulePkg = logos-capability-module.packages.${system}.default;
          pluginExt = if pkgs.stdenv.isDarwin then "dylib" else "so";
        in {
          tests = pkgs.runCommand "logos-liblogos-tests" {
            nativeBuildInputs = [ testsPkg ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.qt6.qtbase ];
          } ''
            export QT_QPA_PLATFORM=offscreen
            ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
              export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
            ''}
            export TEST_PLUGIN="${capabilityModulePkg}/lib/capability_module_plugin.${pluginExt}"
            # The only binaries in reach whose embedded metadata declares an
            # object-form dependency -- one carrying a version range, one whose
            # constraint is not a string (no shipped module declares either), so
            # they are what cover the production discovery -> gate path.
            # Staged into the tests package itself by tests/CMakeLists.txt.
            export TEST_PLUGIN_DEP_RANGE="${testsPkg}/lib/dep_range_fixture_plugin.${pluginExt}"
            export TEST_PLUGIN_DEP_MALFORMED="${testsPkg}/lib/dep_malformed_fixture_plugin.${pluginExt}"
            # Turns a missing fixture into a red run instead of a skip. A skip
            # renders as a pass, which would hand back the coverage hole.
            # The real module host, for RealHostLoadVerdictTest: the load-verdict
            # tests otherwise only prove the stand-in host is handled, and the
            # defect they cover is about what the REAL child does.
            export TEST_REAL_HOST="${defaultModuleLoader}/bin/logos_host_qt"
            export LOGOS_REQUIRE_TEST_FIXTURES=1
            for f in "$TEST_PLUGIN_DEP_RANGE" "$TEST_PLUGIN_DEP_MALFORMED" "$TEST_REAL_HOST"; do
              if [ ! -f "$f" ]; then
                echo "Error: constraint fixture not found at $f" >&2
                exit 1
              fi
            done
            mkdir -p $out
            echo "Running logos-liblogos tests..."
            echo "TEST_PLUGIN=$TEST_PLUGIN"
            echo "TEST_PLUGIN_DEP_RANGE=$TEST_PLUGIN_DEP_RANGE"
            echo "TEST_PLUGIN_DEP_MALFORMED=$TEST_PLUGIN_DEP_MALFORMED"
            ${testsPkg}/bin/logos_core_tests --gtest_output=xml:$out/test-results.xml
          '';
        }
      );

      devShells = forAllSystems ({ pkgs, ... }: {
        default = pkgs.mkShell {
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
          ];
          buildInputs = [
            pkgs.qt6.qtbase
            pkgs.qt6.qtremoteobjects
            pkgs.zstd
            pkgs.spdlog
          ];
        };
      });
    };
}
