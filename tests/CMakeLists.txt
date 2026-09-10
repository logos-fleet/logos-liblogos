# Logos Core Tests

# Get SDK root from parent scope
set(_default_sdk_root "${CMAKE_SOURCE_DIR}/vendor/logos-cpp-sdk")
if(NOT DEFINED LOGOS_CPP_SDK_ROOT OR LOGOS_CPP_SDK_ROOT STREQUAL "")
  set(LOGOS_CPP_SDK_ROOT "${_default_sdk_root}")
endif()
get_filename_component(LOGOS_CPP_SDK_ROOT "${LOGOS_CPP_SDK_ROOT}" ABSOLUTE)

add_executable(logos_core_tests
    test_app_lifecycle.cpp
    test_access_policy.cpp
    test_module_manager.cpp
    test_process_stats.cpp
    test_dependency_resolver.cpp
    test_subprocess_manager.cpp
    test_composite_module_loader.cpp
    test_module_name_validation.cpp
    test_module_loader_registry.cpp
    test_module_loader_abstraction.cpp
    test_protocol_gate.cpp
    test_dependency_gate.cpp
    test_module_state_observer.cpp
    test_expected_exit_marks.cpp
    test_load_verdict.cpp
    test_load_concurrency.cpp
    test_inproc_container.cpp
    test_web_container.cpp
    test_embedded_bare_module.cpp
)

# Imported container/loader targets the tests drive via SubprocessManager /
# CompositeModuleLoader: logos_container + logos_module_loader (the contracts),
# and the build-selected implementation targets LogosContainerImpl::impl /
# LogosFormatLoaderImpl::impl (default subprocess + qt — the test shim
# subprocess_manager.h includes their concrete headers, which the impl configs'
# include dirs provide). The find_package calls are repeated here because the
# imported targets created in src/ are not visible in this sibling directory;
# each config guards with `if(NOT TARGET ...)` so this is safe.
find_package(LogosContainerImpl REQUIRED)
find_package(LogosFormatLoaderImpl REQUIRED)
target_link_libraries(logos_core_tests PRIVATE
    logos_core
    process_stats
    logos_container
    LogosContainerImpl::impl
    logos_module_loader
    LogosFormatLoaderImpl::impl
    GTest::gtest
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Network
)

target_include_directories(logos_core_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/logos_core
)

# Propagate portable build flag to tests so manifest variant keys match
if(LOGOS_PORTABLE_BUILD)
    target_compile_definitions(logos_core_tests PRIVATE LOGOS_PORTABLE_BUILD=1)
endif()

# Add process-stats include directory
if(DEFINED PROCESS_STATS_ROOT AND EXISTS "${PROCESS_STATS_ROOT}/include")
  target_include_directories(logos_core_tests PRIVATE ${PROCESS_STATS_ROOT}/include)
endif()

# Add SDK include directories (same layering as src/CMakeLists.txt:
# Qt-free base headers + qt-host runtime layer + protocol layer)
if(EXISTS "${LOGOS_CPP_SDK_ROOT}/include")
  target_include_directories(logos_core_tests PRIVATE 
    ${LOGOS_CPP_SDK_ROOT}/include
    ${LOGOS_CPP_SDK_ROOT}/include/cpp
  )
else()
  target_include_directories(logos_core_tests PRIVATE ${LOGOS_CPP_SDK_ROOT}/cpp)
endif()
# The Qt host runtime's headers (logos_api.h and friends). These come from
# logos-qt-host, NOT logos-qt-sdk: src/CMakeLists.txt links logos_core against
# logos-qt-host::logos_qt_host, and the tests link logos_core, so pointing this
# at any other prefix would compile the tests against a DIFFERENT copy of
# LogosAPI than the one they link -- the exact split-brain the Windows export
# block in src/CMakeLists.txt exists to prevent.
#
# Not a silent `if(EXISTS ...)` skip. src/CMakeLists.txt already FATAL_ERRORs
# when LOGOS_QT_HOST_ROOT is unusable and runs first (top-level CMakeLists adds
# src/ before tests/), so reaching here with it unset means the layout changed
# underneath us -- and a skip would hand that to the compiler as a confusing
# "logos_api.h: No such file" instead of naming the cause.
if(NOT DEFINED LOGOS_QT_HOST_ROOT OR NOT EXISTS "${LOGOS_QT_HOST_ROOT}/include")
  message(FATAL_ERROR
    "LOGOS_QT_HOST_ROOT does not point at a built logos-qt-host prefix "
    "(got '${LOGOS_QT_HOST_ROOT}'), so logos_core_tests cannot be compiled "
    "against the same Qt host runtime logos_core links.")
endif()
target_include_directories(logos_core_tests PRIVATE
  ${LOGOS_QT_HOST_ROOT}/include
  ${LOGOS_QT_HOST_ROOT}/include/cpp
  ${LOGOS_QT_HOST_ROOT}/include/core
)
if(DEFINED LOGOS_PROTOCOL_ROOT AND EXISTS "${LOGOS_PROTOCOL_ROOT}/include")
  target_include_directories(logos_core_tests PRIVATE
    ${LOGOS_PROTOCOL_ROOT}/include
    ${LOGOS_PROTOCOL_ROOT}/include/cpp
  )
endif()

# Real Qt plugins that exist solely to carry object-form dependencies in their
# EMBEDDED metadata: one with a version range, one whose constraint is not a
# string. Nothing in the fleet declares either, so these are the only inputs
# that exercise the production discovery -> gate path end to end;
# RealDependencyRangeTest / RealMalformedConstraintTest consume them via
# TEST_PLUGIN_DEP_RANGE and TEST_PLUGIN_DEP_MALFORMED.
foreach(_fixture dep_range_fixture dep_malformed_fixture)
    add_library(${_fixture}_plugin MODULE fixtures/${_fixture}_plugin.cpp)
    set_target_properties(${_fixture}_plugin PROPERTIES
        AUTOMOC ON
        PREFIX ""
    )
    if(APPLE)
        # CMake gives a MODULE library .so even on Darwin; match the .dylib the
        # rest of the stack (and the check's pluginExt) names plugins by.
        set_target_properties(${_fixture}_plugin PROPERTIES SUFFIX ".dylib")
    endif()
    target_link_libraries(${_fixture}_plugin PRIVATE Qt${QT_VERSION_MAJOR}::Core)

    # nix/tests.nix builds the `logos_core_tests` target only, so hanging the
    # fixtures off it keeps the test binary and its inputs shipping together.
    add_dependencies(logos_core_tests ${_fixture}_plugin)
endforeach()

# The Bare module the Native container tests dlopen. A plain shared library:
# no Qt, no logos-protocol, no `lp_*` — which is exactly what "bare" means, and
# what lets this one be opened by a test binary rather than by a host image.
#
# MODULE rather than SHARED for the same reason the Qt fixtures above are: it is
# dlopen'd, never linked. The name carries the `_bare` suffix the production
# discovery gate keys on, so the fixture is also the only thing asserting that
# convention end to end.
#
# BUILT TWICE — `bare_fixture_bare` and its twin `bare_twin_bare`, same sources,
# two images. Two Bare modules in one process export the SAME module-impl C ABI
# symbol names by construction; that is what the ABI is. So "each handle reaches
# its own image" is a property of how the container opens them (RTLD_LOCAL) and
# cannot be asserted with one library, and a second build of these sources is
# the only way to produce the collision on purpose: identical symbols, distinct
# images, distinct module-global state.
foreach(_bare bare_fixture_bare bare_twin_bare)
    add_library(${_bare} MODULE fixtures/bare_fixture_module.cpp)
    set_target_properties(${_bare} PROPERTIES PREFIX "")
    if(APPLE)
        set_target_properties(${_bare} PROPERTIES SUFFIX ".dylib")
    endif()
    # The version macros only — <logos_protocol.h> declares the lp_* C ABI and
    # includes nothing, and the fixture CALLS none of it, so this adds a header
    # path and no link dependency. It is what lets the fixture report this
    # build's own protocol version and never fail the container's runtime major
    # handshake for a reason unrelated to the test.
    target_include_directories(${_bare} PRIVATE
        ${LOGOS_PROTOCOL_ROOT}/include
        ${LOGOS_PROTOCOL_ROOT}/include/cpp
    )
    add_dependencies(logos_core_tests ${_bare})
endforeach()

gtest_discover_tests(logos_core_tests DISCOVERY_MODE PRE_TEST)
