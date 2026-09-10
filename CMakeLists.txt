cmake_minimum_required(VERSION 3.14)
project(SimplePluginExample LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Qt packages
# Network is needed explicitly by the token-exchange tests (QLocalServer /
# QLocalSocket); it is also pulled transitively by RemoteObjects in the
# runtime build, but we declare it here so the test target can link it
# explicitly without depending on that transitive resolution.
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Network RemoteObjects)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network RemoteObjects)

# Find nlohmann_json
find_package(nlohmann_json REQUIRED)

# OpenSSL needed transitively by logos_sdk (plain-C++ TCP+SSL transport)
find_package(OpenSSL REQUIRED)

# Find spdlog
find_package(spdlog REQUIRED)

# Set output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Add external library paths to build RPATH so test binaries can find them
if(DEFINED LOGOS_PACKAGE_MANAGER_ROOT)
    list(APPEND CMAKE_BUILD_RPATH "${LOGOS_PACKAGE_MANAGER_ROOT}/lib")
endif()

# The shared C++ runtime, which logos_core now IMPORTS rather than absorbing.
# Both libraries carry an @rpath install name, so a consumer that does not know
# where they live cannot load at all -- and the failure is at dyld time, before
# main(), with no hint from the build.
#
# Measured before this existed: logos_core_tests aborted with
#   dyld: Library not loaded: @rpath/liblogos_qt_host.dylib
# while `nix build .#default` had already succeeded. The library builds and
# installs whether or not anything can load it.
#
# The INSTALLED liblogos_core is covered separately by INSTALL_RPATH $ORIGIN in
# src/CMakeLists.txt plus staging both libraries beside it (nix/lib.nix); this
# entry is what makes the BUILD tree -- tests especially -- resolve them.
# BUILD_RPATH covers binaries run from the build tree; INSTALL_RPATH covers the
# ones that get installed and run from $out -- which is what the test derivation
# actually executes. Both are needed, and only Linux says so: on macOS the
# installed test binary resolved the libraries anyway and the suite passed, while
# the same commit on Linux died with
#   error while loading shared libraries: liblogos_qt_host.so
# A macOS-green test run is not evidence here.
#
# logos_core itself is unaffected by the INSTALL_RPATH lines: it sets its own
# INSTALL_RPATH ($ORIGIN / @loader_path in src/CMakeLists.txt), which overrides
# the directory-level default, and nix/lib.nix stages both libraries beside it.
if(DEFINED LOGOS_PROTOCOL_ROOT)
    list(APPEND CMAKE_BUILD_RPATH "${LOGOS_PROTOCOL_ROOT}/lib")
    list(APPEND CMAKE_INSTALL_RPATH "${LOGOS_PROTOCOL_ROOT}/lib")
endif()
if(DEFINED LOGOS_QT_HOST_ROOT)
    list(APPEND CMAKE_BUILD_RPATH "${LOGOS_QT_HOST_ROOT}/lib")
    list(APPEND CMAKE_INSTALL_RPATH "${LOGOS_QT_HOST_ROOT}/lib")
endif()

# Build src first to ensure logos_core is built before modules
add_subdirectory(src)

# Tests. Gated EXPLICITLY rather than left to find_package: the suite is
# POSIX-only (tests/test_process_stats.cpp uses <spawn.h>, <sys/wait.h>,
# posix_spawn/waitpid/kill and `environ`; tests/test_subprocess_manager.cpp
# spawns /bin/sh), none of which mingw-w64 provides. The default build target
# is `all`, so an ungated tests/ subdirectory would be compiled by every
# cross build.
#
# The gate wraps the GTest lookup too: dropping only `add_subdirectory(tests)`
# would still leave find_package(GTest) able to miss and fall through to the
# FetchContent download, which dies in the nix sandbox with a misleading
# "downloading ... failed".
option(LOGOS_BUILD_TESTS "Build the logos_core test suite" ON)
if(WIN32)
    set(LOGOS_BUILD_TESTS OFF)
endif()

if(LOGOS_BUILD_TESTS)
    # Try to find GoogleTest via find_package first (for Nix and system installations)
    find_package(GTest QUIET)

    if(NOT GTest_FOUND)
        # Fall back to FetchContent if GoogleTest is not found
        message(STATUS "GoogleTest not found via find_package, using FetchContent")
        include(FetchContent)
        FetchContent_Declare(googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        )
        FetchContent_MakeAvailable(googletest)
    else()
        message(STATUS "Using system GoogleTest")
    endif()

    enable_testing()
    include(GoogleTest)

    # Add tests subdirectory
    add_subdirectory(tests)
endif()

# Install rules. The logos_host_qt binary (and its logos_host symlink) now live
# in logos-module-loader-qt; liblogos installs only the core library here, and
# bin.nix re-exports the host binary from that package so frontends are
# unaffected.
# RUNTIME DESTINATION matters on Windows: a DLL is a RUNTIME artifact, and with
# only LIBRARY/ARCHIVE named CMake sends it to the DEFAULT runtime destination
# (bin) while nix/lib.nix only ever copies ${build}/lib -- which would yield a
# lib output holding the import library and no DLL, silently. Pointing RUNTIME
# at lib keeps the output shape identical on all three platforms.
install(TARGETS logos_core
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION lib
)

# Install headers
#
# web_module_view.h joins logos_core.h because it is a SEAM A HOST FILLS, not an
# internal type: a `web` module is a page, and the only process that can open one
# is the frontend (logoscore under `--container web`, a desktop shell, a phone
# app). liblogos deliberately names no browser -- it is linked into a headless
# CLI, a desktop shell and a static iOS core, and exactly one of those can host a
# Chromium -- so the backend is installed from OUTSIDE, and that requires the
# declaration. It pulls in logos-protocol's message_channel.h, which
# logos-protocol installs for the same reason.
install(FILES src/logos_core/logos_core.h
              src/logos_core/web_module_view.h
    DESTINATION include
)
