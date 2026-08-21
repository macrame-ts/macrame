# Standalone consumer of the macrame package. Built OUTSIDE the main tree (against an install
# via find_package, or a build tree) to prove the public include interface + usage requirements
# work for a downstream user. See .github/workflows/ci.yml for the CI wiring.
cmake_minimum_required(VERSION 3.25)
project(macrame_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# macrame needs c++latest on cl (same as the library's own build) - a consumer must match.
# Nothing here mirrors the library's exception setup on purpose: exceptions are the consumer's
# choice, so this project compiles with the toolchain default and the package has to carry
# whatever it actually requires. If that stops being true, this job is where it shows.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "-std:c++latest")
    set(CMAKE_CXX23_EXTENSION_COMPILE_OPTION "-std:c++latest")
endif()

find_package(macrame CONFIG REQUIRED)

add_executable(macrame_consumer main.cpp)
target_link_libraries(macrame_consumer PRIVATE macrame::macrame)
