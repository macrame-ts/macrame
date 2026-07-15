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
project(task_system CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TS_CORE_SOURCES
    access.cpp
    fatal.cpp
    guarded.cpp
    scheduler.cpp
    static_task_graph.cpp
    worker_thread.cpp
)

set(TS_SAMPLE_SOURCES
    sample/blackboard.cpp
    sample/engine.cpp
    sample/frame.cpp
    sample/physics.cpp
    sample/systems.cpp
)

if(WIN32)
    add_executable(task_system
        ${TS_CORE_SOURCES}
        ${TS_SAMPLE_SOURCES}
        main.cpp
        mem_profile.cpp
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
    target_include_directories(task_system PRIVATE . tests benchmarks sample)
    target_compile_definitions(task_system PRIVATE _CONSOLE _HAS_EXCEPTIONS=0)
    # Exceptions off: strip CMake's default /EHsc, then disable explicitly (cl and clang-cl).
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    target_compile_options(task_system PRIVATE /EHs-c-)
else()
    # The portable stress driver (Linux/macOS; TSan has no Windows runtime).
    option(TS_TSAN "Build the stress driver with ThreadSanitizer" ON)
    add_executable(ts_tsan
        ${TS_CORE_SOURCES}
        ${TS_SAMPLE_SOURCES}
        tsan/tsan_main.cpp
    )
    target_include_directories(ts_tsan PRIVATE . sample)
    target_compile_options(ts_tsan PRIVATE -fno-exceptions -pthread)
    target_link_options(ts_tsan PRIVATE -pthread)
    if(TS_TSAN)
        # Mirror tsan/run.sh: -O1 -g keeps stacks useful without hiding races behind opt.
        target_compile_options(ts_tsan PRIVATE -fsanitize=thread -O1 -g)
        target_link_options(ts_tsan PRIVATE -fsanitize=thread)
    endif()
endif()
