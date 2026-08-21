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
# macrame neither throws nor catches (ts::fatal instead), so it builds with or without
# exception support; MACRAME_NO_EXCEPTIONS below picks. See CLAUDE.md.

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

# Exceptions are the consumer's choice. macrame contains no throw, try or catch: failures go
# to ts::fatal, and an exception escaping a user body is fatal at the seam that invokes it
# (detail::invoke_user_body). The library is therefore correct either way, and by default it is
# built with whatever exception support the toolchain defaults to, so a consumer's own
# translation units are free to use exceptions.
#
# MACRAME_NO_EXCEPTIONS builds it exceptions-off instead, for a program that is exceptions-off
# throughout. That is not a private choice on MSVC: _HAS_EXCEPTIONS=0 rewrites STL
# declarations, so every TU in the program must agree - hence the definition (and, where a
# flag can express it, the flag) becomes a PUBLIC usage requirement, and the access.h tripwire
# turns a mixed link into a link error. On cl the /EHsc removal itself cannot be expressed as
# a target property; it is applied at directory scope here, and a consumer building
# exceptions-off strips it the same way.
option(MACRAME_NO_EXCEPTIONS "Build with exceptions disabled (a whole-program requirement)" OFF)

if(MACRAME_NO_EXCEPTIONS)
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

include(CheckCXXSourceCompiles)

# What `macrame_atomic_flags` below probes with: the deque cell, in miniature.
set(macrame_atomic_probe [=[
#include <atomic>
struct Pair { void* a; void* b; };
int main() { std::atomic<Pair> cell; Pair v = cell.load(); cell.store(v); return 0; }
]=])

# The work-stealing deque stores cells as `std::atomic<Task_entry>` - 16 bytes, lock-free only
# where the target has a double-word CAS (`detail::Task_entry`, scheduler.h). MSVC assumes
# cmpxchg16b on x64; baseline x86-64 for GCC/clang does not, so without -mcx16 the compiler
# lowers those loads and stores to libatomic calls that take a lock - the cells stop being
# lock-free, and the link fails outright unless libatomic is present. Ask for the instruction
# where the architecture has it, and fall back to libatomic where the libcalls remain anyway.
# PUBLIC: a consumer's own TUs instantiate the same atomics.
function(macrame_atomic_flags target)
    if(MSVC)
        return()
    endif()
    set(extra_flags "")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        set(extra_flags "-mcx16")
        target_compile_options(${target} PUBLIC -mcx16)
    endif()
    # Whether the libcalls remain anyway is a property of the target, so ask the compiler
    # rather than guessing: link a 16-byte atomic as configured, and only if that fails try
    # again with -latomic. Asking the driver for the library beats locating the file - the
    # linker-visible libatomic.so is absent on distributions that ship only the runtime.
    set(CMAKE_REQUIRED_FLAGS "${extra_flags}")
    check_cxx_source_compiles("${macrame_atomic_probe}" MACRAME_ATOMIC16_LINKS)
    if(NOT MACRAME_ATOMIC16_LINKS)
        set(CMAKE_REQUIRED_LIBRARIES atomic)
        check_cxx_source_compiles("${macrame_atomic_probe}" MACRAME_ATOMIC16_NEEDS_LIBATOMIC)
        if(MACRAME_ATOMIC16_NEEDS_LIBATOMIC)
            target_link_libraries(${target} PUBLIC atomic)
        else()
            message(WARNING "macrame: a 16-byte std::atomic does not link on this target; "
                            "the scheduler's work-stealing deque needs one")
        endif()
    endif()
endfunction()

# Common private build flags for our own targets (not propagated to consumers).
function(macrame_private_flags target)
    if(WIN32)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /permissive- /Zc:__cplusplus)   # ConformanceMode
        endif()
        # Debug info even in Release so std::stacktrace / MiniDumpWriteDump resolve symbols;
        # /OPT:REF,ICF restores the Release folding that /DEBUG disables (optimized codegen kept).
        target_compile_options(${target} PRIVATE /Zi)
        target_link_options(${target} PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    else()
        target_compile_options(${target} PRIVATE -pthread)
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
# tripwire fires. TS_SAFETY_CHECKS / TS_PROFILING are only defined when OFF (their headers
# default them ON), matching the vcxproj/driver convention.
target_compile_definitions(macrame PUBLIC
    $<$<NOT:$<BOOL:${TS_SAFETY_CHECKS}>>:TS_SAFETY_CHECKS=0>
    $<$<NOT:$<BOOL:${TS_PROFILING}>>:TS_PROFILING=0>)

# The exceptions-off build's usage requirements: _HAS_EXCEPTIONS=0 is what the MSVC STL bakes
# into its declarations, so it has to reach the consumer; the flag half is expressible for
# clang-cl and for Linux, not for cl (see MACRAME_NO_EXCEPTIONS above).
if(MACRAME_NO_EXCEPTIONS)
    target_compile_definitions(macrame PUBLIC
        $<$<CXX_COMPILER_ID:MSVC,Clang>:$<$<PLATFORM_ID:Windows>:_HAS_EXCEPTIONS=0>>)
    target_compile_options(macrame PUBLIC
        $<$<AND:$<PLATFORM_ID:Windows>,$<CXX_COMPILER_ID:Clang>>:/EHs-c->
        $<$<NOT:$<PLATFORM_ID:Windows>>:-fno-exceptions>)
endif()

macrame_private_flags(macrame)
macrame_atomic_flags(macrame)

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
