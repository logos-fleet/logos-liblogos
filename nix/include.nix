# Installs the logos-liblogos headers
{ pkgs, common, src, logosSdk ? null, logosProtocolPkg ? null, logosQtSdk ? null
, logosQtHost ? null }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-headers";
  version = common.version;
  
  inherit src;
  inherit (common) meta;

  # This output RE-EXPORTS the Qt host runtime headers, and two of them --
  # logos_provider_object.h and logos_qt_arg_decode.h -- include
  # <nlohmann/json.hpp>. So anything compiling against these includes needs
  # nlohmann on its include path, whether or not it has ever heard of nlohmann.
  #
  # Consumers that go through find_package(logos-qt-host) already get it: that
  # package find_dependency's logos-protocol, which PUBLIC-links nlohmann_json.
  # Consumers that take the include directory DIRECTLY -- logos-module-viewer
  # uses find_library + raw -I, and it is not alone in that -- bypass CMake's
  # propagation entirely and fail with
  #
  #     fatal error: nlohmann/json.hpp: No such file or directory
  #
  # in a repo that never mentions nlohmann. Propagating it here fixes both
  # shapes at the source rather than adding a dependency to each consumer that
  # trips over it, which is a list that only grows.
  propagatedBuildInputs = [ pkgs.nlohmann_json ];
  
  # No build phase needed, just install headers
  dontBuild = true;
  dontConfigure = true;
  
  installPhase = ''
    runHook preInstall
    
    # Install headers
    mkdir -p $out/include
    
    # Install logos_core.h (main C API header)
    if [ -f src/logos_core/logos_core.h ]; then
      cp src/logos_core/logos_core.h $out/include/
    fi

    # web_module_view.h -- the seam a HOST fills, so it has to be reachable
    # from outside this repo. A `web` module is a page, and the only process
    # that can open one is the frontend (logoscore under `--container web`, a
    # desktop shell, a phone app); liblogos deliberately names no browser. It
    # includes logos-protocol's message_channel.h, which the copy below
    # supplies. Kept in step with CMakeLists.txt's install(FILES ...) -- this
    # derivation does not run that rule, so both lists exist and both matter.
    if [ -f src/logos_core/web_module_view.h ]; then
      cp src/logos_core/web_module_view.h $out/include/
    fi
    
    # Also copy SDK headers if available (including logos_mode.h)
    if [ -n "${toString logosSdk}" ] && [ -d "${toString logosSdk}/include" ]; then
      cp -r ${toString logosSdk}/include/* $out/include/ 2>/dev/null || true
    fi
    if [ -n "${toString logosProtocolPkg}" ] && [ -d "${toString logosProtocolPkg}/include" ]; then
      cp -r ${toString logosProtocolPkg}/include/* $out/include/ 2>/dev/null || true
    fi
    if [ -n "${toString logosQtSdk}" ] && [ -d "${toString logosQtSdk}/include" ]; then
      cp -r ${toString logosQtSdk}/include/* $out/include/ 2>/dev/null || true
    fi
    # logos-qt-host LAST, and deliberately so. It and logos-qt-sdk both ship a
    # logos_api.h (and the rest of the host-runtime headers) while the B2b
    # forwarders are still in place, so this copy decides which declaration a
    # downstream consumer of THIS prefix compiles against. It has to be the one
    # whose code liblogos_core actually links -- logos-qt-host's. They differ:
    # qt-host's LogosAPI carries LOGOS_SHARED_API, which is the __declspec
    # (dllimport) that makes an in-process Windows consumer import the host's
    # TokenManager instead of linking a second one.
    #
    # Overwriting is the point, so this cp must stay after the qt-sdk one; the
    # qt-sdk copy above still supplies the headers qt-host does not ship at all
    # (logos_qt_lp_bridge.h, logos_qt_wire.h, logos_qt_host_core.h).
    #
    # `cp -rf` plus the chmod are load-bearing, not tidiness: everything copied
    # above came out of the nix store mode 0444, so a plain `cp -r` over it
    # fails with EACCES -- and the `|| true` these lines all carry would swallow
    # that and silently leave logos-qt-sdk's header winning. The assertion below
    # is what actually proves the overwrite happened.
    if [ -n "${toString logosQtHost}" ] && [ -d "${toString logosQtHost}/include" ]; then
      chmod -R u+w $out/include
      cp -rf ${toString logosQtHost}/include/* $out/include/

      # Fail closed if the header that ends up installed is not the one whose
      # code liblogos_core links. LOGOS_SHARED_API is present in qt-host's
      # logos_api.h and absent from qt-sdk's, so it is the discriminator.
      if ! grep -q 'LOGOS_SHARED_API' $out/include/logos_api.h; then
        echo "ERROR: $out/include/logos_api.h is not logos-qt-host's copy." >&2
        echo "       The qt-host headers did not overwrite the qt-sdk ones, so" >&2
        echo "       consumers of this prefix would compile against a different" >&2
        echo "       LogosAPI than liblogos_core links." >&2
        exit 1
      fi
    fi

    runHook postInstall
  '';
}

