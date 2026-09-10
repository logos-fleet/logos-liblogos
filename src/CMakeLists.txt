set(CMAKE_AUTOMOC ON)

set(_default_sdk_root "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/logos-cpp-sdk")
if(NOT DEFINED LOGOS_CPP_SDK_ROOT OR LOGOS_CPP_SDK_ROOT STREQUAL "")
  set(LOGOS_CPP_SDK_ROOT "${_default_sdk_root}")
endif()

get_filename_component(LOGOS_CPP_SDK_ROOT "${LOGOS_CPP_SDK_ROOT}" ABSOLUTE)

# The SDK stack since the Qt split: logos-protocol (transports + lp_* C
# ABI), logos-qt-host (LogosAPI + provider glue — the Qt HOST RUNTIME this
# library links), logos-cpp-sdk (Qt-free std headers). The `logos_sdk` alias
# is kept so the rest of this file (and the host targets) stay unchanged;
# it points at logos-qt-host, whose link interface chains logos-protocol
# and its OpenSSL/Boost/nlohmann deps.
#
# The host runtime used to be COMPILED INTO logos-qt-sdk and reached here as
# logos-qt-sdk::logos_qt_sdk. The code now lives in logos-plugin-qt, which
# publishes it as logos-qt-host::logos_qt_host. logos-qt-sdk keeps only the
# developer layer (the UI plugin context, the LpBridge/wire headers, the Qt
# generator) — none of which this library compiles against — so the host
# runtime is found DIRECTLY here rather than arriving as a transitive
# dependency of find_package(logos-qt-sdk). This repo still consumes
# logos-qt-sdk as a nix input, but only to re-export those developer headers
# from nix/include.nix; nothing in this CMake build needs it.
if(NOT DEFINED LOGOS_PROTOCOL_ROOT AND DEFINED ENV{LOGOS_PROTOCOL_ROOT})
  set(LOGOS_PROTOCOL_ROOT "$ENV{LOGOS_PROTOCOL_ROOT}")
endif()
if(NOT DEFINED LOGOS_QT_HOST_ROOT AND DEFINED ENV{LOGOS_QT_HOST_ROOT})
  set(LOGOS_QT_HOST_ROOT "$ENV{LOGOS_QT_HOST_ROOT}")
endif()

if(EXISTS "${LOGOS_PROTOCOL_ROOT}/lib/cmake/logos-protocol"
   AND EXISTS "${LOGOS_QT_HOST_ROOT}/lib/cmake/logos-qt-host")
  message(STATUS "Using logos-protocol at ${LOGOS_PROTOCOL_ROOT}")
  message(STATUS "Using logos-qt-host at ${LOGOS_QT_HOST_ROOT}")
  find_package(logos-protocol REQUIRED
    PATHS "${LOGOS_PROTOCOL_ROOT}/lib/cmake/logos-protocol" NO_DEFAULT_PATH)
  find_package(logos-qt-host REQUIRED
    PATHS "${LOGOS_QT_HOST_ROOT}/lib/cmake/logos-qt-host" NO_DEFAULT_PATH)
  if(NOT TARGET logos_sdk)
    add_library(logos_sdk INTERFACE IMPORTED)
    # The SHARED Qt host runtime, not the static archive. This is the line that
    # replaces the whole-archive + generated-.def block further down: it makes
    # liblogos_core a CONSUMER of the runtime rather than its provider, so
    # TokenManager / LogosAPIClient / LogosAPI each have exactly one definition
    # in the process because there is exactly one library defining them.
    #
    # logos_qt_host_shared itself PUBLIC-links logos_protocol_shared, so the
    # protocol half arrives transitively and correctly layered -- it is asserted
    # in logos-plugin-qt by checks.shared-runtime-layering.
    if(LOGOS_CORE_STATIC)
      target_link_libraries(logos_sdk INTERFACE logos-qt-host::logos_qt_host)
    else()
      target_link_libraries(logos_sdk INTERFACE logos-qt-host::logos_qt_host_shared)
    endif()
  endif()
else()
  message(FATAL_ERROR "logos-protocol / logos-qt-host not found. Set "
                      "LOGOS_PROTOCOL_ROOT and LOGOS_QT_HOST_ROOT to built packages.")
endif()

# Qt-free base SDK headers (logos_module_context.h etc.) — header-only.
if(EXISTS "${LOGOS_CPP_SDK_ROOT}/lib/cmake/logos-cpp-sdk")
  find_package(logos-cpp-sdk REQUIRED
    PATHS "${LOGOS_CPP_SDK_ROOT}/lib/cmake/logos-cpp-sdk" NO_DEFAULT_PATH)
  if(NOT TARGET logos_headers)
    add_library(logos_headers INTERFACE IMPORTED)
    target_link_libraries(logos_headers INTERFACE logos-cpp-sdk::logos_headers)
  endif()
  target_link_libraries(logos_sdk INTERFACE logos-cpp-sdk::logos_headers)
endif()

# Find logos-module library
if(DEFINED LOGOS_MODULE_ROOT AND EXISTS "${LOGOS_MODULE_ROOT}/lib")
  message(STATUS "Using logos-module at ${LOGOS_MODULE_ROOT}")
  
  find_library(LOGOS_MODULE_LIBRARY
    NAMES logos_module
    PATHS "${LOGOS_MODULE_ROOT}/lib"
    NO_DEFAULT_PATH
  )
  
  if(NOT LOGOS_MODULE_LIBRARY)
    message(FATAL_ERROR "logos_module library not found in ${LOGOS_MODULE_ROOT}/lib")
  endif()
  
  # Create an imported target for logos_module
  add_library(logos_module STATIC IMPORTED)
  set_target_properties(logos_module PROPERTIES
    IMPORTED_LOCATION "${LOGOS_MODULE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_MODULE_ROOT}/include"
  )
else()
  message(STATUS "LOGOS_MODULE_ROOT not set, logos-module will need to be provided")
endif()

# Find process-stats library
if(DEFINED PROCESS_STATS_ROOT AND EXISTS "${PROCESS_STATS_ROOT}/lib")
  message(STATUS "Using process-stats at ${PROCESS_STATS_ROOT}")

  find_library(PROCESS_STATS_LIBRARY
    NAMES process_stats
    PATHS "${PROCESS_STATS_ROOT}/lib"
    NO_DEFAULT_PATH
  )

  if(NOT PROCESS_STATS_LIBRARY)
    message(FATAL_ERROR "process_stats library not found in ${PROCESS_STATS_ROOT}/lib")
  endif()

  add_library(process_stats STATIC IMPORTED)
  set_target_properties(process_stats PROPERTIES
    IMPORTED_LOCATION "${PROCESS_STATS_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${PROCESS_STATS_ROOT}/include"
  )
else()
  message(STATUS "PROCESS_STATS_ROOT not set, process-stats will need to be provided")
endif()

# Find logos-container (the Qt-free, header-only container contract:
# ModuleContainer interface + ModuleDescriptor/LoadedModuleHandle + the
# module-name allowlist). Header-only, so it is an INTERFACE import keyed on
# its include dir rather than a library.
if(DEFINED LOGOS_CONTAINER_ROOT AND EXISTS "${LOGOS_CONTAINER_ROOT}/include")
  message(STATUS "Using logos-container at ${LOGOS_CONTAINER_ROOT}")

  add_library(logos_container INTERFACE IMPORTED GLOBAL)
  set_target_properties(logos_container PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_CONTAINER_ROOT}/include"
    INTERFACE_LINK_LIBRARIES "nlohmann_json::nlohmann_json"
  )
else()
  message(FATAL_ERROR "LOGOS_CONTAINER_ROOT not set or ${LOGOS_CONTAINER_ROOT}/include does not exist")
endif()

# Container implementation — selected by the BUILD, not named here.
#
# The chosen implementation package ships a generic CMake config package
# (LogosContainerImpl) that defines the imported target LogosContainerImpl::impl,
# carrying its static library AND its own transitive deps (Boost, spdlog, ...).
# liblogos just asks for "the container implementation" by that generic name and
# links the target — it knows neither the library name nor the dependencies. nix
# puts the chosen package on CMAKE_PREFIX_PATH (default logos-container-subprocess);
# switching to another container (Docker, ...) means a package that provides the
# same config — no edit to this file, the nix flags, or any C++.
find_package(LogosContainerImpl REQUIRED)

# Find logos-module-loader (the header-only ModuleFormatLoader contract).
if(DEFINED LOGOS_MODULE_LOADER_ROOT AND EXISTS "${LOGOS_MODULE_LOADER_ROOT}/include")
  message(STATUS "Using logos-module-loader at ${LOGOS_MODULE_LOADER_ROOT}")

  add_library(logos_module_loader INTERFACE IMPORTED GLOBAL)
  set_target_properties(logos_module_loader PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_MODULE_LOADER_ROOT}/include"
    INTERFACE_LINK_LIBRARIES "logos_container"
  )
else()
  message(FATAL_ERROR "LOGOS_MODULE_LOADER_ROOT not set or ${LOGOS_MODULE_LOADER_ROOT}/include does not exist")
endif()

# Format-loader implementation — selected by the BUILD (mirrors the container).
# The chosen package ships a generic CMake config package (LogosFormatLoaderImpl)
# defining the imported target LogosFormatLoaderImpl::impl with its own deps.
# Default: logos-module-loader-qt (which also ships the logos_host_qt binary,
# re-exported via bin.nix — independent of this lib link).
find_package(LogosFormatLoaderImpl REQUIRED)


# Define the library sources.
# The container contract (ModuleContainer + descriptor value types) lives in
# logos-container and the ModuleFormatLoader contract in logos-module-loader.
# The concrete container + format-loader implementations are NOT named here:
# they are linked through the build-selected `LogosContainerImpl::impl` /
# `LogosFormatLoaderImpl::impl` targets found via find_package above (default
# subprocess + qt, chosen in nix/default.nix). The ModuleLoader base, the
# composite/registry orchestration, and the logging foundation are core concerns
# and stay here.
set(LOGOS_CORE_SOURCES
    logging/logos_log.cpp
    logging/logos_log.h
    logos_core/logos_core.cpp
    logos_core/logos_core.h
    logos_core/module_registry.cpp
    logos_core/module_registry.h
    logos_core/dependency_resolver.cpp
    logos_core/dependency_resolver.h
    logos_core/dependency_gate.cpp
    logos_core/dependency_gate.h
    logos_core/access_policy.cpp
    logos_core/access_policy.h
    logos_core/module_state_observer.cpp
    logos_core/module_state_observer.h
    logos_core/module_manager.cpp
    logos_core/module_manager.h
    logos_core/module_loader.h
    logos_core/composite_module_loader.cpp
    logos_core/composite_module_loader.h
    logos_core/module_loader_registry.cpp
    logos_core/module_loader_registry.h
    # The NATIVE CONTAINER. Unlike the subprocess container and the Qt format
    # loader, this pair is NOT reached through the link-time factory seams and
    # so is built here: those seams admit exactly one provider each, which
    # answers "which container is the default" and cannot answer "which
    # containers exist". Both are registered, and the module's artifact shape
    # decides which one runs it.
    logos_core/bare_module_abi.cpp
    logos_core/bare_module_abi.h
    logos_core/bare_module_glue.cpp
    logos_core/bare_module_glue.h
    logos_core/bare_module_loader.cpp
    logos_core/bare_module_loader.h
    logos_core/inproc_container.cpp
    logos_core/inproc_container.h
)

# The format-loader implementation and the logos_host_qt host binary live in the
# package the build selects (default logos-module-loader-qt); the
# ModuleFormatLoader interface lives in logos-module-loader. liblogos links the
# implementation lib via the find_package'd `LogosFormatLoaderImpl::impl` target
# and re-exports the host binary from the same package via bin.nix.

# Create the logos core library
# Static-only hosts (iOS) link the core into the app image.
option(LOGOS_CORE_STATIC "Build liblogos_core as a static archive and link static dependencies" OFF)
set(LOGOS_CORE_LIB_TYPE SHARED)
if(LOGOS_CORE_STATIC)
    set(LOGOS_CORE_LIB_TYPE STATIC)
endif()
add_library(logos_core ${LOGOS_CORE_LIB_TYPE} ${LOGOS_CORE_SOURCES})

# Set the LOGOS_CORE_LIBRARY definition for the library
target_compile_definitions(logos_core PRIVATE LOGOS_CORE_LIBRARY)

# PE export table: nothing to do here. LOGOS_CORE_EXPORT now expands to
# __declspec(dllexport) on Windows (see logos_core/logos_core.h), which both
# exports the C API and, because GNU ld disables auto-export image-wide once any
# symbol is dllexported, stops the internal C++ surface leaking.
#
# This deliberately REPLACES an earlier `-Wl,--export-all-symbols`. That did
# export the C API, but it also exported all 13,252 symbols in the image --
# LogosAPI's among them -- so any consumer linking both liblogos_core and the
# qt-sdk static library failed with "multiple definition of `LogosAPI::LogosAPI'".
# Do not reinstate it; if the C API ever stops being exported, the cause is a
# missing LOGOS_CORE_EXPORT on a declaration, not a missing linker flag.
#
# The C API is not the whole story, and the rest of it is no longer solved here.
#
# Narrowing the export table to the C API left every in-process image with its
# own statically linked copy of the shared C++ runtime -- and therefore its own
# TokenManager singleton, so a capability token saved by the host was invisible
# to the UI plugin and every cross-module call was refused. This file used to
# close that gap by absorbing liblogos_protocol.a and liblogos_qt_host.a whole
# (--whole-archive) and re-publishing their symbols through a generated .def, so
# that liblogos_core became the single provider for types it does not own.
#
# It is not the provider any more. logos-protocol and logos-qt-host now ship
# REAL SHARED LIBRARIES that own their own symbols -- liblogos_protocol exports
# its table from a generated .def of its own (logos-protocol#65), and
# liblogos_qt_host owns LogosAPI (logos-plugin-qt#22). liblogos_core links them
# like any other dependency and IMPORTS the runtime instead of defining it.
#
# So this whole block is gone, along with cmake/gen-shared-exports.sh. What
# replaces it is one line: logos_sdk carries logos_qt_host_shared rather than
# the static archive, near the top of this file.
#
# WHAT DID NOT CHANGE is the invariant. The runtime must still exist exactly
# once per process; it is now enforced by there being one shared library per
# type rather than by one image absorbing everything. Consumers still empty
# their static archives (that was logos-basecamp/cmake/LogosSharedFromDll.cmake,
# deleted in logos-basecamp#348 and logos-logoscore-cli#98 once they linked the
# shared libraries directly and had no second static copy left to suppress) so ld
# cannot pull an archive member that would redefine an imported symbol, and
# logos-basecamp/nix/symbol-gate.nix still asserts the result.
#
# OUT-OF-PROCESS consumers are deliberately unaffected: module plugins and
# ui_qml backends keep linking the STATIC archive, because each runs in its own
# process where its own copy is the CORRECT per-process singleton, and because a
# .lgx records an empty nix closure and could not carry a shared library anyway.
#
# Full rationale, including why the consumer-side __declspec(dllimport) is the
# load-bearing half rather than the export, lives in
# logos-protocol/cpp/logos_shared_api.h.

# Portable build: selects portable LGX variants instead of dev variants
option(LOGOS_PORTABLE_BUILD "Build for portable variant selection" OFF)
if(LOGOS_PORTABLE_BUILD)
    target_compile_definitions(logos_core PRIVATE LOGOS_PORTABLE_BUILD=1)
endif()

# Find Boost (header-only usage + Boost.Process/Asio)
find_package(Boost REQUIRED COMPONENTS process filesystem)

# Find logos-package-manager library
if(DEFINED LOGOS_PACKAGE_MANAGER_ROOT AND EXISTS "${LOGOS_PACKAGE_MANAGER_ROOT}/lib")
  message(STATUS "Using logos-package-manager at ${LOGOS_PACKAGE_MANAGER_ROOT}")

  find_library(LOGOS_PACKAGE_MANAGER_LIBRARY
    NAMES package_manager_lib
    PATHS "${LOGOS_PACKAGE_MANAGER_ROOT}/lib"
    NO_DEFAULT_PATH
  )

  if(NOT LOGOS_PACKAGE_MANAGER_LIBRARY)
    message(FATAL_ERROR "package_manager_lib not found in ${LOGOS_PACKAGE_MANAGER_ROOT}/lib")
  endif()

  add_library(package_manager_lib ${LOGOS_CORE_LIB_TYPE} IMPORTED)
  set_target_properties(package_manager_lib PROPERTIES
    IMPORTED_LOCATION "${LOGOS_PACKAGE_MANAGER_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_PACKAGE_MANAGER_ROOT}/include"
  )
  # On Windows a SHARED IMPORTED target links through its IMPORT library, and
  # CMake hard-errors at generate time when IMPORTED_IMPLIB is unset. find_library
  # returns libpackage_manager_lib.dll.a there (CMAKE_FIND_LIBRARY_SUFFIXES for
  # MinGW is .dll.a;.a), which is exactly the import library to link.
  if(WIN32)
    set_target_properties(package_manager_lib PROPERTIES
      IMPORTED_IMPLIB "${LOGOS_PACKAGE_MANAGER_LIBRARY}"
    )
  endif()

  # liblgx, for the semver the dependency gate evaluates ranges with. It ships
  # in the same root (lgx.h is already on package_manager_lib's include dirs)
  # but package_manager_lib only IMPORTS it, so it must be named here too.
  find_library(LOGOS_LGX_LIBRARY
    NAMES lgx
    PATHS "${LOGOS_PACKAGE_MANAGER_ROOT}/lib"
    NO_DEFAULT_PATH
  )

  if(NOT LOGOS_LGX_LIBRARY)
    message(FATAL_ERROR "lgx not found in ${LOGOS_PACKAGE_MANAGER_ROOT}/lib")
  endif()

  add_library(lgx_lib ${LOGOS_CORE_LIB_TYPE} IMPORTED)
  set_target_properties(lgx_lib PROPERTIES
    IMPORTED_LOCATION "${LOGOS_LGX_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_PACKAGE_MANAGER_ROOT}/include"
  )
  if(WIN32)
    set_target_properties(lgx_lib PROPERTIES
      IMPORTED_IMPLIB "${LOGOS_LGX_LIBRARY}"
    )
  endif()
else()
  message(FATAL_ERROR "LOGOS_PACKAGE_MANAGER_ROOT not set or ${LOGOS_PACKAGE_MANAGER_ROOT}/lib does not exist")
endif()

# Link Qt libraries, SDK, module library, and package-manager library.
# The container/format-loader implementations are linked through the generic,
# build-selected `LogosContainerImpl::impl` / `LogosFormatLoaderImpl::impl`
# targets (each comes from a find_package config that carries its own deps);
# `logos_container` is the shared contract. liblogos names no specific
# implementation here.
target_link_libraries(logos_core PUBLIC
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::RemoteObjects
    logos_sdk
    logos_module
    process_stats
    logos_container
    LogosContainerImpl::impl
    LogosFormatLoaderImpl::impl
    package_manager_lib
    lgx_lib
    nlohmann_json::nlohmann_json
    Boost::process
    spdlog::spdlog
)

# Include directories for the library
target_include_directories(logos_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/logos_core
    ${CMAKE_CURRENT_SOURCE_DIR}/module_loaders
    ${Qt${QT_VERSION_MAJOR}_INCLUDE_DIRS}
)

# Add logos_module include directory if available
if(DEFINED LOGOS_MODULE_ROOT AND EXISTS "${LOGOS_MODULE_ROOT}/include")
  target_include_directories(logos_core PRIVATE
    ${LOGOS_MODULE_ROOT}/include
    ${LOGOS_MODULE_ROOT}/include/module_lib
  )
endif()

# Add process-stats include directory if available
if(DEFINED PROCESS_STATS_ROOT AND EXISTS "${PROCESS_STATS_ROOT}/include")
  target_include_directories(logos_core PRIVATE ${PROCESS_STATS_ROOT}/include)
endif()


# Add SDK include directory if using built SDK
if(EXISTS "${LOGOS_CPP_SDK_ROOT}/lib" AND EXISTS "${LOGOS_CPP_SDK_ROOT}/include")
  target_include_directories(logos_core PRIVATE 
    ${LOGOS_CPP_SDK_ROOT}/include
    ${LOGOS_CPP_SDK_ROOT}/include/cpp
    ${LOGOS_CPP_SDK_ROOT}/include/core
  )
else()
  target_include_directories(logos_core PRIVATE ${LOGOS_CPP_SDK_ROOT}/cpp)
endif()

# Set RPATH settings for the library
if(APPLE)
    set(_qt_runtime_paths "@loader_path")
    if(DEFINED Qt${QT_VERSION_MAJOR}_DIR)
        get_filename_component(_qt_lib_dir "${Qt${QT_VERSION_MAJOR}_DIR}/../.." REALPATH)
        list(APPEND _qt_runtime_paths "${_qt_lib_dir}")
    endif()
    list(REMOVE_DUPLICATES _qt_runtime_paths)

    set_target_properties(logos_core PROPERTIES
        INSTALL_RPATH "${_qt_runtime_paths}"
        INSTALL_NAME_DIR "@rpath"
        BUILD_WITH_INSTALL_NAME_DIR TRUE
        BUILD_WITH_INSTALL_RPATH TRUE)
else()
    set_target_properties(logos_core PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH TRUE)
endif()
