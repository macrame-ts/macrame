# CMake build for task_system, alongside the Visual Studio solution (task_system.slnx /
# task_system.vcxproj -- both stay authoritative for VS-IDE work; keep the file lists in
# sync when adding sources). Two targets:
#   - task_system (Windows): the full driver -- tests + benchmarks + samples via main.cpp
#     (the test harness, tests/harness.cpp, is Windows-only: <windows.h> death tests).
#   - ts_tsan (non-Windows): the portable ThreadSanitizer stress driver from
#     tsan/tsan_main.cpp, mirroring tsan/run.sh (which remains the canonical way to run it;
#     configure with -DTS_TSAN=OFF for a plain build of the same driver).
# Exceptions are disabled project-wide (ts::fatal instead); see CLAUDE.md.

cmake_minimum_required(VERSION 3.25)
project(task_system VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The access-safety harness (TS_CHECK_ACCESS) is compiled in by default. A shipping build is a
# Release build with -DTS_SAFETY_CHECKS=OFF, which defines TS_SAFETY_CHECKS=0 and compiles the
# harness out (mirrors the vcxproj "Shipping" configuration). Note: with the harness off, the
# access-violation death tests no longer fire, so the full --tests run is not expected to pass
# under a shipping build -- it is a release/ship artifact, not a test build.
option(TS_SAFETY_CHECKS "Compile in the access-safety harness" ON)
# Profiling/introspection instrumentation (DOT dump, runtime trace). OFF mirrors the
# vcxproj Shipping configuration (TS_PROFILING=0).
option(TS_PROFILING "Compile in graph profiling instrumentation" ON)

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
    sample/game_frame.cpp
    sample/physics.cpp
)

if(WIN32)
    # Real MSVC (cl): the STL's invoke_result SFINAE fails under /std:c++23preview -- which is
    # what CMAKE_CXX_STANDARD 23 maps to on cl -- but works under /std:c++latest, which is what
    # the vcxproj uses (LanguageStandard=stdcpplatest) and what passes. Override the C++23 flag
    # mapping to c++latest for cl only; clang-cl (COMPILER_ID Clang) keeps CMake's default and is
    # unaffected. Must be set before the target so the standard flag is generated from it.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "-std:c++latest")
        set(CMAKE_CXX23_EXTENSION_COMPILE_OPTION "-std:c++latest")
    endif()

    add_executable(task_system
        ${TS_CORE_SOURCES}
        ${TS_SAMPLE_SOURCES}
        src/main.cpp
        src/mem_profile.cpp
        benchmarks/benchmarks.cpp
        tests/access_tests.cpp
        tests/coroutine_tests.cpp
        tests/deferred_tests.cpp
        tests/graph_tests.cpp
        tests/guarded_tests.cpp
        tests/harness.cpp
        tests/integration_tests.cpp
        tests/parallel_tests.cpp
        tests/scheduler_tests.cpp
        tests/task_tests.cpp
        tests/tests.cpp
        tests/versioned_tests.cpp
    )
    target_include_directories(task_system PRIVATE include tests benchmarks sample tools)
    target_compile_definitions(task_system PRIVATE _CONSOLE _HAS_EXCEPTIONS=0)
    if(NOT TS_SAFETY_CHECKS)
        target_compile_definitions(task_system PRIVATE TS_SAFETY_CHECKS=0)
    endif()
    if(NOT TS_PROFILING)
        target_compile_definitions(task_system PRIVATE TS_PROFILING=0)
    endif()
    # Exceptions off: strip CMake's default /EHsc.
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        # Match the vcxproj: ConformanceMode + NO /EH flag (ExceptionHandling=false). An explicit
        # /EHs-c- makes cl's <type_traits> mis-specialize invoke_result for the sample's lambdas.
        target_compile_options(task_system PRIVATE /permissive- /Zc:__cplusplus)
    else()
        # clang-cl: disable exceptions explicitly.
        target_compile_options(task_system PRIVATE /EHs-c-)
    endif()
else()
    # The portable stress driver (Linux/macOS; TSan has no Windows runtime).
    option(TS_TSAN "Build the stress driver with ThreadSanitizer" ON)
    add_executable(ts_tsan
        ${TS_CORE_SOURCES}
        ${TS_SAMPLE_SOURCES}
        tsan/tsan_main.cpp
    )
    target_include_directories(ts_tsan PRIVATE include sample tools)
    if(NOT TS_SAFETY_CHECKS)
        target_compile_definitions(ts_tsan PRIVATE TS_SAFETY_CHECKS=0)
    endif()
    if(NOT TS_PROFILING)
        target_compile_definitions(ts_tsan PRIVATE TS_PROFILING=0)
    endif()
    target_compile_options(ts_tsan PRIVATE -fno-exceptions -pthread)
    target_link_options(ts_tsan PRIVATE -pthread)
    if(TS_TSAN)
        # Mirror tsan/run.sh: -O1 -g keeps stacks useful without hiding races behind opt.
        target_compile_options(ts_tsan PRIVATE -fsanitize=thread -O1 -g)
        target_link_options(ts_tsan PRIVATE -fsanitize=thread)
    endif()
endif()
