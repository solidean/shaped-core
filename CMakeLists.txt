# Minimum versions and the rationale: docs/requirements.md
cmake_minimum_required(VERSION 3.28)
project(shaped-core LANGUAGES CXX)

# Lock in C++23 for the whole repo.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Vendored dependencies. Each SC_USE_VENDORED_<LIB> defaults ON and builds the
# in-tree copy under extern/<lib>/; turning one OFF means the embedding project
# must already provide a target of that name.
option(SC_USE_VENDORED_MIMALLOC "Build the in-tree vendored mimalloc (OFF expects an external `mimalloc` target)" ON)

if(SC_USE_VENDORED_MIMALLOC)
    add_subdirectory(extern/mimalloc)
endif()

# clean-core must come first: nexus depends on it.
add_subdirectory(libs/base/clean-core)
add_subdirectory(libs/base/nexus)
