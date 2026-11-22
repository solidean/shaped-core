cmake_minimum_required(VERSION 3.28)

# Before project(): the developer environment a build dir keeps, which the file explains.
include("${CMAKE_CURRENT_SOURCE_DIR}/extern/shaped-core/tools/cmake/ConfigureEnvironment.cmake")

project(__project__ LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# The shaped-core libraries this project links, each with its own dependencies; nexus is the test harness.
# Plain variables, not cache entries, so an edit here reaches a build folder that is already configured.
set(SC_LIBRARIES "clean-core;nexus")
# shaped-linter, which `dev.py lint shaped` and the shaped-lint gate build by name, and none of shaped-core's other tools.
set(SC_BUILD_LINTER ON)
add_subdirectory(extern/shaped-core EXCLUDE_FROM_ALL)

add_subdirectory(libs/base/__lib__)
