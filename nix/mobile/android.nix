# liblogos_core and everything it links for the aarch64-android package set
# (logos-nix's pkgsAndroid: nixpkgs' own Android cross stdenv, Qt shared).
#
# The same nine stages and the same two patches as ./ios.nix; the CMake
# options those patches and the upstream repos gained stay at their DEFAULTS
# here (shared libraries), because Android's Qt is shared and one .so per
# component is what the APK packages. Only the test/CLI gates are used.
#
# `logos_host_qt` and the subprocess container are kept, unlike on iOS:
# Android can fork, so out-of-process modules stay on the table.
{
  pkgs,
  srcs,
  native,
}:

let
  inherit (pkgs) lib;
  buildPkgs = pkgs.pkgsBuildBuild;
  patchDir = ./patches;

  patched =
    name: src: patches: buildPkgs.applyPatches { inherit name src patches; };

  crossFlags = pkgs.logosQtCrossCmakeFlags ++ [
    "-DCMAKE_TOOLCHAIN_FILE=${pkgs.logosQtCrossToolchainFile}"
    "-GNinja"
    # Qt6Config only looks for extra modules under the prefixes named here,
    # and the Android overlay's flags list none on the target side.
    "-DQT_ADDITIONAL_PACKAGES_PREFIX_PATH=${pkgs.qt6.qtremoteobjects}"
  ];

  stage =
    {
      pname,
      src,
      sourceDir ? ".",
      buildInputs ? [ ],
      cmakeFlags ? [ ],
      postInstall ? "",
      # lgx's pkg_check_modules(libsodium) yields a bare `-lsodium` with no
      # -L under cross ("ld.lld: error: unable to find library -lsodium");
      # without pkg-config on PATH its CMake falls back to find_library and
      # links the absolute store path.
      pkgConfig ? true,
    }:
    pkgs.stdenv.mkDerivation {
      inherit
        pname
        src
        buildInputs
        postInstall
        ;
      version = "0.1.0";
      cmakeDir = "../${sourceDir}";
      nativeBuildInputs = [
        buildPkgs.cmake
        buildPkgs.ninja
      ]
      ++ lib.optional pkgConfig buildPkgs.pkg-config;
      dontWrapQtApps = true;
      cmakeFlags = crossFlags ++ [
        "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH"
        "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH"
        "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH"
      ]
      ++ cmakeFlags;
    };

  qt = [
    pkgs.qt6.qtbase
    pkgs.qt6.qtremoteobjects
  ];
  thirdParty = [
    pkgs.boost
    pkgs.openssl
    pkgs.spdlog
    pkgs.nlohmann_json
  ];

  protocol = stage {
    pname = "logos-protocol-android";
    src = srcs.protocol;
    sourceDir = "cpp";
    buildInputs = qt ++ thirdParty;
  };

  qtHost = stage {
    pname = "logos-qt-host-android";
    src = srcs.pluginQt;
    sourceDir = "cpp";
    buildInputs = qt ++ thirdParty ++ [ protocol ];
    cmakeFlags = [ "-DLOGOS_PROTOCOL_ROOT=${protocol}" ];
  };

  lgx = stage {
    pname = "logos-package-android";
    src = srcs.package;
    buildInputs = [
      pkgs.zlib
      pkgs.icu
      pkgs.libsodium
      pkgs.nlohmann_json
      native.cppSemver
    ];
    cmakeFlags = [
      "-DLGX_BUILD_SHARED=ON"
      "-DLGX_BUILD_TESTS=OFF"
    ];
    pkgConfig = false;
    postInstall = ''
      rm -rf $out/bin
      cp ../src/lgx.h $out/include/
      cp -r ${native.cppSemver}/include/semver $out/include/
    '';
  };

  logosModule = stage {
    pname = "logos-module-android";
    src = srcs.module;
    buildInputs = qt ++ [ lgx ];
    cmakeFlags = [ "-DLOGOS_PACKAGE_ROOT=${lgx}" ];
    postInstall = "rm -rf $out/bin";
  };

  processStats = stage {
    pname = "process-stats-android";
    src = srcs.processStats;
    buildInputs = [ pkgs.nlohmann_json ];
    cmakeFlags = [ "-DPROCESS_STATS_BUILD_TESTS=OFF" ];
  };

  containerSubprocess = stage {
    pname = "logos-container-subprocess-android";
    src = patched "logos-container-subprocess" srcs.containerSubprocess [
      "${patchDir}/logos-container-subprocess-tests-option.patch"
    ];
    buildInputs = thirdParty ++ [ native.logosContainer ];
    cmakeFlags = [
      "-DLOGOS_CONTAINER_ROOT=${native.logosContainer}"
      "-DLOGOS_BUILD_TESTS=OFF"
    ];
  };

  moduleLoaderQtInputs = qt ++ thirdParty ++ [
    protocol
    qtHost
    logosModule
    native.cppSdk
    native.qtSdk
    native.logosContainer
    native.logosModuleLoader
    pkgs.cli11
  ];
  moduleLoaderQt = stage {
    pname = "logos-module-loader-qt-android";
    src = patched "logos-module-loader-qt" srcs.moduleLoaderQt [
      "${patchDir}/logos-module-loader-qt-ios.patch"
    ];
    buildInputs = moduleLoaderQtInputs;
    cmakeFlags = [
      "-DLOGOS_CPP_SDK_ROOT=${native.cppSdk}"
      "-DLOGOS_PROTOCOL_ROOT=${protocol}"
      "-DLOGOS_QT_SDK_ROOT=${native.qtSdk}"
      "-DLOGOS_MODULE_ROOT=${logosModule}"
      "-DLOGOS_CONTAINER_ROOT=${native.logosContainer}"
      "-DLOGOS_MODULE_LOADER_ROOT=${native.logosModuleLoader}"
      "-Dlogos-qt-host_DIR=${qtHost}/lib/cmake/logos-qt-host"
      "-DLOGOS_BUILD_TESTS=OFF"
    ];
  };

  packageManager = stage {
    pname = "logos-package-manager-android";
    src = srcs.packageManager;
    buildInputs = [
      lgx
      pkgs.nlohmann_json
    ];
    cmakeFlags = [ "-DLGX_ROOT=${lgx}" ];
    postInstall = ''
      rm -rf $out/bin
      cp ${lgx}/lib/liblgx.so $out/lib/
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
  liblogos = stage {
    pname = "logos-liblogos-android";
    src = srcs.liblogos;
    buildInputs = liblogosInputs;
    cmakeFlags = [
      "-DLOGOS_CPP_SDK_ROOT=${native.cppSdk}"
      "-DLOGOS_PROTOCOL_ROOT=${protocol}"
      "-DLOGOS_QT_HOST_ROOT=${qtHost}"
      "-DLOGOS_MODULE_ROOT=${logosModule}"
      "-DPROCESS_STATS_ROOT=${processStats}"
      "-DLOGOS_CONTAINER_ROOT=${native.logosContainer}"
      "-DLOGOS_MODULE_LOADER_ROOT=${native.logosModuleLoader}"
      "-DLOGOS_PACKAGE_MANAGER_ROOT=${packageManager}"
      "-Dlogos-qt-host_DIR=${qtHost}/lib/cmake/logos-qt-host"
      "-DLOGOS_BUILD_TESTS=OFF"
      "-DCMAKE_CXX_FLAGS=-I${native.logosModuleLoader}/include"
    ];
    # Everything an APK has to carry beside liblogos_core.so. Collected here
    # rather than left to the consumer because the set is a property of what
    # logos_core links, not of the app.
    postInstall = ''
      cp ${protocol}/lib/liblogos_protocol.so ${qtHost}/lib/liblogos_qt_host.so \
         ${packageManager}/lib/libpackage_manager_lib.so ${lgx}/lib/liblgx.so $out/lib/
    '';
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
  all = liblogosInputs ++ [ liblogos ];
  # The package set the chain was built from, so a consumer can build its
  # own stage against it without instantiating a second one.
  inherit pkgs;
}
