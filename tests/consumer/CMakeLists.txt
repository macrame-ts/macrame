# Standalone consumer of the macrame package. Built OUTSIDE the main tree (against an install
# via find_package, or a build tree) to prove the public include interface + usage requirements
# work for a downstream user. See .github/workflows/ci.yml for the CI wiring.
cmake_minimum_required(VERSION 3.25)
project(macrame_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Nothing here mirrors the library's own toolchain or exception setup on purpose: a consumer
# asks for C++23 and links the package, and everything else the library needs has to travel
# with it as a usage requirement. If that stops being true, this job is where it shows.
find_package(macrame CONFIG REQUIRED)

add_executable(macrame_consumer main.cpp)
target_link_libraries(macrame_consumer PRIVATE macrame::macrame)
