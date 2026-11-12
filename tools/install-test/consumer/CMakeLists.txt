# A project that takes shaped-core installed, the way a package manager's consumer does: find_package, link, nothing else.
# No definition and no flag of shaped-core's is repeated here; tools/install-test/install-test.py builds and runs it.
cmake_minimum_required(VERSION 3.28)
project(shaped-core-install-consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(shaped-core REQUIRED)

add_executable(consumer main.cc)
target_link_libraries(consumer PRIVATE shaped-core::shaped-graphics-language)
