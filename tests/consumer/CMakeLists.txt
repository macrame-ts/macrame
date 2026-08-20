# Standalone consumer of the macrame package. Built OUTSIDE the main tree (against an install
# via find_package, or a build tree) to prove the public include interface + usage requirements
# work for a downstream user. See .github/workflows/ci.yml for the CI wiring.
cmake_minimum_required(VERSION 3.25)
project(macrame_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# macrame is exceptions-off and needs c++latest on cl (same as the library's own build) - a
# consumer must match. These mirror the library's private build setup.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "-std:c++latest")
    set(CMAKE_CXX23_EXTENSION_COMPILE_OPTION "-std:c++latest")
endif()
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

find_package(macrame CONFIG REQUIRED)

add_executable(macrame_consumer main.cpp)
target_link_libraries(macrame_consumer PRIVATE macrame::macrame)
if(WIN32 AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(macrame_consumer PRIVATE /EHs-c-)   # clang-cl: exceptions off
endif()
