# CMake build for macrame, alongside the Visual Studio solution (macrame.slnx, which holds the
# macrame static-library project and the macrame_playground driver project). Keep the source
# lists in sync across CMakeLists / the two .vcxproj / their .filters when adding files.
#
# Targets:
#   - macrame            : the consumable STATIC library (the 6 src/*.cpp core + include/ts).
#                          Alias macrame::macrame; installable + find_package()-able.
#   - macrame_playground : the dev driver (tests + benchmarks + samples via main.cpp), Windows -
#                          links macrame. The harness (tests/harness.cpp) is Windows-only
#                          (<windows.h> death tests). Not shipped.
#   - ts_tsan            : the portable ThreadSanitizer stress driver (non-Windows), links
#                          macrame; mirrors tsan/run.sh.
# Exceptions are disabled project-wide (ts::fatal instead); see CLAUDE.md.

cmake_minimum_required(VERSION 3.25)
project(macrame VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# The access-safety harness (TS_CHECK_ACCESS) is compiled in by default. A shipping build is a
# Release build with -DTS_SAFETY_CHECKS=OFF, which defines TS_SAFETY_CHECKS=0 and compiles the
# harness out (mirrors the vcxproj "Shipping" configuration). Note: with the harness off, the
# access-violation death tests no longer fire, so the full --tests run is not expected to pass
# under a shipping build -- it is a release/ship artifact, not a test build.
option(TS_SAFETY_CHECKS "Compile in the access-safety harness" ON)
# Profiling/introspection instrumentation (DOT dump, runtime trace). OFF mirrors the
# vcxproj Shipping configuration (TS_PROFILING=0).
option(TS_PROFILING "Compile in graph profiling instrumentation" ON)
# The dev driver (tests/benchmarks/samples). A pure library consumer configures -DMACRAME_BUILD_DRIVER=OFF.
option(MACRAME_BUILD_DRIVER "Build the macrame_playground test/benchmark/sample driver" ON)

set(TS_CORE_SOURCES
    src/access.cpp
    src/fatal.cpp
    src/guarded.cpp
    src/scheduler.cpp
    src/static_task_graph.cpp
    src/worker_thread.cpp
)

set(TS_SAMPLE_SOURCES
    sample/blackboard.cpp
    sample/coloring.cpp
    sample/events.cpp
    sample/game_frame.cpp
    sample/physics.cpp
    sample/scope_access.cpp
)

set(TS_DRIVER_SOURCES
    src/main.cpp
    src/mem_profile.cpp
    benchmarks/benchmarks.cpp
    benchmarks/features_bench.cpp
    benchmarks/game_frame_bench.cpp
    benchmarks/production_contention_bench.cpp
    benchmarks/production_contention_lib_bench.cpp
    benchmarks/scheduler_bench.cpp
    benchmarks/staged_commands_bench.cpp
    tests/access_tests.cpp
    tests/coroutine_tests.cpp
    tests/deferred_tests.cpp
    tests/event_bus_tests.cpp
    tests/graph_tests.cpp
    tests/guarded_tests.cpp
    tests/harness.cpp
    tests/integration_tests.cpp
    tests/named_tests.cpp
    tests/parallel_tests.cpp
    tests/pipe_tests.cpp
    tests/rules_tests.cpp
    tests/scheduler_tests.cpp
    tests/task_tests.cpp
    tests/tests.cpp
    tests/versioned_tests.cpp
)

# Real MSVC (cl): the STL's invoke_result SFINAE fails under /std:c++23preview -- which is what
# CMAKE_CXX_STANDARD 23 maps to on cl -- but works under /std:c++latest (what the vcxproj uses,
# LanguageStandard=stdcpplatest). Override the C++23 flag mapping to c++latest for cl only;
# clang-cl (COMPILER_ID Clang) keeps CMake's default. Must be set before any target.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "-std:c++latest")
    set(CMAKE_CXX23_EXTENSION_COMPILE_OPTION "-std:c++latest")
endif()

# Exceptions off: strip CMake's default /EHsc (MSVC/clang-cl); Linux uses -fno-exceptions below.
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

# Common private build flags for our own targets (not propagated to consumers).
function(macrame_private_flags target)
    if(WIN32)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            # ConformanceMode + no /EH flag (an explicit /EHs-c- makes cl's <type_traits>
            # mis-specialize invoke_result for the sample lambdas).
            target_compile_options(${target} PRIVATE /permissive- /Zc:__cplusplus)
        else()
            target_compile_options(${target} PRIVATE /EHs-c-)   # clang-cl: exceptions off
        endif()
        # Debug info even in Release so std::stacktrace / MiniDumpWriteDump resolve symbols;
        # /OPT:REF,ICF restores the Release folding that /DEBUG disables (optimized codegen kept).
        target_compile_options(${target} PRIVATE /Zi)
        target_link_options(${target} PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    else()
        target_compile_options(${target} PRIVATE -fno-exceptions -pthread)
        target_link_options(${target} PRIVATE -pthread)
    endif()
endfunction()

# ============================ The consumable static library ============================
add_library(macrame STATIC ${TS_CORE_SOURCES})
add_library(macrame::macrame ALIAS macrame)

target_include_directories(macrame PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# The core .cpp include the dev/profiling tooling headers (tools/graph_introspect.h etc.) in
# their implementation; the public headers only forward-declare them, so this stays PRIVATE.
target_include_directories(macrame PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools)
target_compile_features(macrame PUBLIC cxx_std_23)

# Config-consistency (load-bearing): a consumer's own TUs must compile with the SAME values the
# library was built with, or the layouts diverge (ODR) and the access.h detect_mismatch link
# tripwire fires. Exceptions are off project-wide, which the MSVC STL bakes into template
# specializations, so _HAS_EXCEPTIONS=0 is a PUBLIC requirement too. TS_SAFETY_CHECKS / TS_PROFILING
# are only defined when OFF (their headers default them ON), matching the vcxproj/driver convention.
target_compile_definitions(macrame PUBLIC
    $<$<CXX_COMPILER_ID:MSVC,Clang>:$<$<PLATFORM_ID:Windows>:_HAS_EXCEPTIONS=0>>
    $<$<NOT:$<BOOL:${TS_SAFETY_CHECKS}>>:TS_SAFETY_CHECKS=0>
    $<$<NOT:$<BOOL:${TS_PROFILING}>>:TS_PROFILING=0>)

macrame_private_flags(macrame)

# ThreadSanitizer instrumentation lives on the library too, so ts_tsan catches races in the
# scheduler/pipe core (non-Windows; TSan has no Windows runtime).
if(NOT WIN32)
    option(TS_TSAN "Build with ThreadSanitizer" ON)
    if(TS_TSAN)
        target_compile_options(macrame PUBLIC -fsanitize=thread -O1 -g)
        target_link_options(macrame PUBLIC -fsanitize=thread)
    endif()
endif()

# ============================ Install / export (find_package) ============================
install(TARGETS macrame EXPORT macrame-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/ts DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT macrame-targets
    NAMESPACE macrame::
    FILE macrame-targets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/macrame)

configure_package_config_file(
    cmake/macrame-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/macrame-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/macrame)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/macrame-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/macrame-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/macrame-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/macrame)

# ============================ The dev driver (Windows) ============================
if(WIN32 AND MACRAME_BUILD_DRIVER)
    add_executable(macrame_playground
        ${TS_SAMPLE_SOURCES}
        ${TS_DRIVER_SOURCES}
    )
    target_link_libraries(macrame_playground PRIVATE macrame::macrame)
    target_include_directories(macrame_playground PRIVATE tests benchmarks sample tools)
    target_compile_definitions(macrame_playground PRIVATE _CONSOLE)
    macrame_private_flags(macrame_playground)
endif()

# ============================ The TSan stress driver (non-Windows) ============================
if(NOT WIN32)
    add_executable(ts_tsan ${TS_SAMPLE_SOURCES} tsan/tsan_main.cpp)
    target_link_libraries(ts_tsan PRIVATE macrame::macrame)
    target_include_directories(ts_tsan PRIVATE sample tools tests)
    macrame_private_flags(ts_tsan)
endif()
