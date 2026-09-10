# liblogos_core and everything it links, as static archives for one iOS
# package set (logos-nix's pkgsIosSimulator or pkgsIos).
#
# Why the chain lives here rather than one mobile output per repo, the way
# x86_64-windows works: two of the nine stages -- logos-container-subprocess
# and logos-module-loader-qt -- are consumed only as flake inputs and are not
# editable from this workspace, so their two CMake gates are carried as
# patches under ./patches. Every other stage builds this repo's own inputs
# unpatched, against the options that landed in them.
#
# STATIC IS THE POINT. Qt for iOS is static, so a second dynamic image would
# be a second QtCore: liblogos_core, logos_protocol and logos_qt_host are all
# built as archives and linked into the app. mkIosCmakeStage fails a stage
# that installs a dynamic image, so this is checked, not assumed.
{
  pkgs,
  # Source trees (flake inputs' outPath).
  # { protocol, pluginQt, package, module, processStats,
  #   containerSubprocess, moduleLoaderQt, packageManager, liblogos }
  srcs,
  # Build-platform packages whose outputs are platform-neutral: header-only
  # libraries and INTERFACE-only CMake packages (they install headers and a
  # CMake config, no compiled object).
  # { cppSdk, qtSdk, logosContainer, logosModuleLoader, cppSemver }
  native,
}:

let
  inherit (pkgs) lib;
  buildPkgs = pkgs.pkgsBuildBuild;
  patchDir = ./patches;

  patched =
    name: src: patches: buildPkgs.applyPatches { inherit name src patches; };

  # An iOS sysroot puts find_package in root-only mode (the toolchain file
  # sets CMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY), so every input has to be
  # named as a ROOT -- being on CMAKE_PREFIX_PATH is not enough.
  roots = inputs: "-DCMAKE_FIND_ROOT_PATH=${lib.concatStringsSep ";" (map toString inputs)}";

  thirdParty = [
    pkgs.boost
    pkgs.openssl
    pkgs.spdlog
    pkgs.nlohmann_json
  ];

  protocol = pkgs.mkIosCmakeStage {
    pname = "logos-protocol-ios";
    version = "0.1.0";
    src = srcs.protocol;
    sourceDir = "cpp";
    buildInputs = thirdParty;
    cmakeFlags = [
      (roots thirdParty)
      "-DOPENSSL_ROOT_DIR=${pkgs.openssl}"
      "-DLOGOS_PROTOCOL_BUILD_SHARED=OFF"
    ];
  };

  qtHost = pkgs.mkIosCmakeStage {
    pname = "logos-qt-host-ios";
    version = "0.1.0";
    src = srcs.pluginQt;
    sourceDir = "cpp";
    buildInputs = thirdParty ++ [ protocol ];
    cmakeFlags = [
      (roots (thirdParty ++ [ protocol ]))
      "-DOPENSSL_ROOT_DIR=${pkgs.openssl}"
      "-DLOGOS_PROTOCOL_ROOT=${protocol}"
      "-DLOGOS_QT_HOST_BUILD_SHARED=OFF"
    ];
  };

  # The lgx C ABI as liblgx.a; CoreFoundation stands in for ICU, which has no
  # iOS build on this pin.
  lgx = pkgs.mkIosCmakeStage {
    pname = "logos-package-ios";
    version = "0.1.0";
    src = srcs.package;
    buildInputs = [
      pkgs.libsodium
      pkgs.nlohmann_json
      native.cppSemver
    ];
    cmakeFlags = [
      (roots [
        pkgs.libsodium
        pkgs.nlohmann_json
        native.cppSemver
      ])
      "-DLGX_STATIC_CABI=ON"
      "-DLGX_UNICODE_COREFOUNDATION=ON"
      "-DLGX_BUILD_TESTS=OFF"
    ];
    # The same shape logos-package's own nix/lib.nix produces: lgx.h beside
    # the archive, and the cpp-semver header that include/logos/semver.hpp
    # includes.
    postInstall = ''
      cp ../src/lgx.h $out/include/
      cp -r ${native.cppSemver}/include/semver $out/include/
    '';
  };

  logosModule = pkgs.mkIosCmakeStage {
    pname = "logos-module-ios";
    version = "0.1.0";
    src = srcs.module;
    buildInputs = [ lgx ];
    cmakeFlags = [
      (roots [ lgx ])
      "-DLOGOS_PACKAGE_ROOT=${lgx}"
    ];
  };

  processStats = pkgs.mkIosCmakeStage {
    pname = "process-stats-ios";
    version = "0.1.0";
    src = srcs.processStats;
    buildInputs = [ pkgs.nlohmann_json ];
    cmakeFlags = [
      (roots [ pkgs.nlohmann_json ])
      "-DPROCESS_STATS_BUILD_TESTS=OFF"
    ];
  };

  # Boost.Process compiles here and the factory is linked, but iOS has no
  # subprocesses, so the container is never SELECTED at runtime. Building it
  # anyway keeps one liblogos_core source tree for both platforms.
  containerSubprocess = pkgs.mkIosCmakeStage {
    pname = "logos-container-subprocess-ios";
    version = "0.1.0";
    src = patched "logos-container-subprocess" srcs.containerSubprocess [
      "${patchDir}/logos-container-subprocess-tests-option.patch"
    ];
    buildInputs = thirdParty ++ [ native.logosContainer ];
    cmakeFlags = [
      (roots (thirdParty ++ [ native.logosContainer ]))
      "-DLOGOS_CONTAINER_ROOT=${native.logosContainer}"
      "-DLOGOS_BUILD_TESTS=OFF"
    ];
  };

  moduleLoaderQtInputs = thirdParty ++ [
    protocol
    qtHost
    logosModule
    native.cppSdk
    native.qtSdk
    native.logosContainer
    native.logosModuleLoader
    pkgs.cli11
  ];
  moduleLoaderQt = pkgs.mkIosCmakeStage {
    pname = "logos-module-loader-qt-ios";
    version = "0.1.0";
    src = patched "logos-module-loader-qt" srcs.moduleLoaderQt [
      "${patchDir}/logos-module-loader-qt-ios.patch"
    ];
    buildInputs = moduleLoaderQtInputs;
    cmakeFlags = [
      (roots moduleLoaderQtInputs)
      "-DOPENSSL_ROOT_DIR=${pkgs.openssl}"
      "-DLOGOS_CPP_SDK_ROOT=${native.cppSdk}"
      "-DLOGOS_PROTOCOL_ROOT=${protocol}"
      "-DLOGOS_QT_SDK_ROOT=${native.qtSdk}"
      "-DLOGOS_MODULE_ROOT=${logosModule}"
      "-DLOGOS_CONTAINER_ROOT=${native.logosContainer}"
      "-DLOGOS_MODULE_LOADER_ROOT=${native.logosModuleLoader}"
      # The native logos-qt-sdk config HINTS at the native logos-qt-host; a
      # _DIR cache entry is what outranks a hint.
      "-Dlogos-qt-host_DIR=${qtHost}/lib/cmake/logos-qt-host"
      "-DLOGOS_BUILD_TESTS=OFF"
    ];
  };

  packageManager = pkgs.mkIosCmakeStage {
    pname = "logos-package-manager-ios";
    version = "1.0.0-dev";
    src = srcs.packageManager;
    buildInputs = [
      lgx
      pkgs.nlohmann_json
    ];
    cmakeFlags = [
      (roots [
        lgx
        pkgs.nlohmann_json
      ])
      "-DLGX_ROOT=${lgx}"
      "-DLGPM_STATIC_LIB=ON"
    ];
    # liblogos looks for lgx BESIDE package_manager_lib (logos-package-manager's
    # own nix/lib.nix stages the dylib there); stage the archive the same way.
    postInstall = ''
      cp ${lgx}/lib/liblgx.a $out/lib/
      cp ${lgx}/include/lgx.h $out/include/
    '';
  };

  liblogosInputs = moduleLoaderQtInputs ++ [
    processStats
    containerSubprocess
    moduleLoaderQt
    packageManager
    lgx
  ];
  liblogos = pkgs.mkIosCmakeStage {
    pname = "logos-liblogos-ios";
    version = "0.1.0";
    src = srcs.liblogos;
    buildInputs = liblogosInputs;
    cmakeFlags = [
      (roots liblogosInputs)
      "-DOPENSSL_ROOT_DIR=${pkgs.openssl}"
      "-DLOGOS_CPP_SDK_ROOT=${native.cppSdk}"
      "-DLOGOS_PROTOCOL_ROOT=${protocol}"
      "-DLOGOS_QT_HOST_ROOT=${qtHost}"
      "-DLOGOS_MODULE_ROOT=${logosModule}"
      "-DPROCESS_STATS_ROOT=${processStats}"
      "-DLOGOS_CONTAINER_ROOT=${native.logosContainer}"
      "-DLOGOS_MODULE_LOADER_ROOT=${native.logosModuleLoader}"
      "-DLOGOS_PACKAGE_MANAGER_ROOT=${packageManager}"
      "-Dlogos-qt-host_DIR=${qtHost}/lib/cmake/logos-qt-host"
      "-DLOGOS_CORE_STATIC=ON"
      "-DLOGOS_BUILD_TESTS=OFF"
      # logos_core includes logos_module_loader/*.h but never links the
      # logos_module_loader INTERFACE target that carries the include dir. A
      # native build gets it anyway, from nix's cc-wrapper (NIX_CFLAGS_COMPILE
      # names every buildInput's include/); Xcode's clang has no equivalent,
      # so the omission only shows up here.
      "-DCMAKE_CXX_FLAGS=-I${native.logosModuleLoader}/include"
    ];
  };
in
{
  inherit
    protocol
    qtHost
    lgx
    logosModule
    processStats
    containerSubprocess
    moduleLoaderQt
    packageManager
    liblogos
    ;
  # Everything an app links, in one CMAKE_FIND_ROOT_PATH-able list.
  all = liblogosInputs ++ [
    liblogos
    pkgs.libsodium
  ];
}
