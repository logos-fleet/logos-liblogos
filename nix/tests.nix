# Builds tests
{ pkgs, common, build }:

# The suite is POSIX-only (posix_spawn/waitpid/kill, /bin/sh) and CMake turns
# LOGOS_BUILD_TESTS off for a Windows host, so there would be no
# `logos_core_tests` target to build. Refuse loudly instead: the configurePhase
# below hand-rolls its `cmake` invocation and never expands $cmakeFlags, so a
# Windows instantiation would silently drop -DCMAKE_SYSTEM_NAME=Windows and
# every entry of logosQtCrossCmakeFlags -- i.e. configure as a NATIVE build and
# link the wrong architecture, which is far worse than an error. flake.nix
# already withholds this attribute on Windows; this makes that non-negotiable.
if pkgs.stdenv.hostPlatform.isWindows then
  throw "logos-liblogos: the logos_core test suite is POSIX-only and cannot be cross-compiled for ${pkgs.stdenv.hostPlatform.system}"
else

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-tests";
  version = common.version;
  
  inherit (build) src;
  inherit (common) buildInputs meta env;
  
  # Add platform-specific tools conditionally
  nativeBuildInputs = common.nativeBuildInputs 
    ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [ pkgs.darwin.cctools ]
    ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.patchelf ];
  
  # Use the same CMake flags as the main build
  cmakeFlags = common.cmakeFlags;
  
  # Configure phase - reuse build outputs
  configurePhase = ''
    runHook preConfigure
    
    # Copy the built artifacts from the main build
    cp -r ${build}/* .
    chmod -R u+w .
    
    # Reconfigure to generate test targets
    cmake -B build -S ${build.src} \
      -GNinja \
      -DLOGOS_CPP_SDK_ROOT=${common.env.LOGOS_CPP_SDK_ROOT} \
      -DLOGOS_PROTOCOL_ROOT=${common.env.LOGOS_PROTOCOL_ROOT} \
      -DLOGOS_QT_HOST_ROOT=${common.env.LOGOS_QT_HOST_ROOT} \
      -DLOGOS_MODULE_ROOT=${common.env.LOGOS_MODULE_ROOT} \
      -DPROCESS_STATS_ROOT=${common.env.PROCESS_STATS_ROOT} \
      -DLOGOS_CONTAINER_ROOT=${common.env.LOGOS_CONTAINER_ROOT} \
      -DLOGOS_MODULE_LOADER_ROOT=${common.env.LOGOS_MODULE_LOADER_ROOT} \
      -DLOGOS_PACKAGE_MANAGER_ROOT=${common.env.LOGOS_PACKAGE_MANAGER_ROOT} \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$out
    
    runHook postConfigure
  '';
  
  # Build phase - build the test executable
  buildPhase = ''
    runHook preBuild
    
    cd build
    ninja logos_core_tests
    
    runHook postBuild
  '';
  
  # Install phase - install the test executable
  installPhase = ''
    runHook preInstall
    
    mkdir -p $out/bin
    cp bin/logos_core_tests $out/bin/
    
    # Copy the libraries so tests can run
    mkdir -p $out/lib
    cp -r lib/* $out/lib/ || true
    # Copy package_manager_lib and liblgx (logos_core links both) from their
    # nix store path
    cp ${common.env.LOGOS_PACKAGE_MANAGER_ROOT}/lib/libpackage_manager_lib.* $out/lib/ || true
    cp ${common.env.LOGOS_PACKAGE_MANAGER_ROOT}/lib/liblgx.* $out/lib/ || true

    # The constraint fixture plugins, which the blanket lib/ copy above already
    # carried in. Assert them rather than trust it: without a fixture its test
    # skips, and a skip renders as a pass -- the exact silent hole the fixtures
    # were added to close.
    for _name in dep_range_fixture_plugin dep_malformed_fixture_plugin bare_fixture_bare bare_twin_bare; do
      _fixture=""
      for cand in $out/lib/$_name.so $out/lib/$_name.dylib; do
        [ -f "$cand" ] && _fixture="$cand"
      done
      if [ -z "$_fixture" ]; then
        echo "Error: $_name missing from $out/lib" >&2
        ls -la lib >&2 || true
        exit 1
      fi
    done

    ${pkgs.lib.optionalString pkgs.stdenv.isDarwin ''
      # Fix RPATH to find libraries in $out/lib on macOS
      install_name_tool \
        -change @rpath/liblogos_core.dylib $out/lib/liblogos_core.dylib \
        $out/bin/logos_core_tests || true
    ''}

    ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
      # Fix RPATH on Linux to avoid /build/ references and include all dependencies.
      # spdlog links libfmt; both must be on RPATH because patchelf replaces the default search paths.
      #
      # THIS LIST IS THE WHOLE SEARCH PATH. `patchelf --set-rpath` REPLACES what
      # CMake wrote, so CMAKE_BUILD_RPATH / CMAKE_INSTALL_RPATH have no effect on
      # the installed binaries here -- anything missing from this string is
      # missing at runtime, full stop. Measured: adding both to CMAKE_*_RPATH
      # changed nothing and the suite still died with
      #   error while loading shared libraries: liblogos_qt_host.so
      # while the same commit passed on macOS, which does not go through here.
      # OpenSSL (libssl, libcrypto) is needed because the SDK's plain-C++ TLS
      # transport links it transitively — without this the wrapped binary
      # dies with `libssl.so.3: cannot open shared object`.
      _rpath="$out/lib:${common.env.LOGOS_PROTOCOL_ROOT}/lib:${common.env.LOGOS_QT_HOST_ROOT}/lib:${pkgs.boost}/lib:${common.env.LOGOS_PACKAGE_MANAGER_ROOT}/lib:${pkgs.gtest}/lib:${pkgs.qt6.qtbase}/lib:${pkgs.qt6.qtremoteobjects}/lib:${pkgs.spdlog}/lib:${pkgs.fmt}/lib:${pkgs.openssl.out}/lib:${pkgs.stdenv.cc.cc.lib}/lib"
      patchelf --set-rpath "$_rpath" $out/bin/logos_core_tests || true
      # Fix RPATH on liblogos_core.so so it can find its transitive deps (e.g. libboost_process, spdlog, fmt, libssl)
      _rpath_lib="$out/lib:${common.env.LOGOS_PROTOCOL_ROOT}/lib:${common.env.LOGOS_QT_HOST_ROOT}/lib:${pkgs.boost}/lib:${common.env.LOGOS_PACKAGE_MANAGER_ROOT}/lib:${pkgs.qt6.qtbase}/lib:${pkgs.qt6.qtremoteobjects}/lib:${pkgs.spdlog}/lib:${pkgs.fmt}/lib:${pkgs.openssl.out}/lib:${pkgs.stdenv.cc.cc.lib}/lib"
      patchelf --set-rpath "$_rpath_lib" $out/lib/liblogos_core.so || true
    ''}
    
    runHook postInstall
  '';
}
