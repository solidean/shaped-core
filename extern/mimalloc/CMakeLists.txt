cmake_minimum_required(VERSION 3.28)
project(mimalloc LANGUAGES CXX)

# Vendored mimalloc (see vendor-mimalloc.py for the pinned version). We build a
# single static target from upstream's single-TU entry point; the build is not
# configurable for now. The whole src/ tree is on disk because static.c #includes
# the rest of it.
set(MIMALLOC_DIR ${CMAKE_CURRENT_SOURCE_DIR})

add_library(mimalloc STATIC ${MIMALLOC_DIR}/src/static.c)

# Compile the single TU as C++ (upstream's MI_USE_CXX path: modern atomics, and
# the supported mode on MSVC).
set_source_files_properties(${MIMALLOC_DIR}/src/static.c PROPERTIES LANGUAGE CXX)

# SYSTEM so mimalloc.h does not trip our warnings in consumers (incl. tests).
target_include_directories(mimalloc SYSTEM PUBLIC ${MIMALLOC_DIR}/include)

target_compile_definitions(mimalloc PRIVATE
    MI_STATIC_LIB
    $<$<CONFIG:Debug>:MI_DEBUG=2>
)

# OS libraries the static build links against (from upstream's CMakeLists).
# mimalloc builds for WebAssembly via its emscripten/wasi prim backends (src/prim/{emscripten,wasi}).
# Single-threaded wasm has no pthreads to find, so skip the Threads dependency there; once we add the
# multithreaded wasm preset it will compile with -pthread and emscripten provides the threading runtime.
if (WIN32)
    target_link_libraries(mimalloc PUBLIC psapi shell32 user32 advapi32 bcrypt)
elseif (EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "WASI")
    # no OS threading library to link against
else()
    find_package(Threads REQUIRED)
    target_link_libraries(mimalloc PUBLIC Threads::Threads)
endif()

# Third-party code: silence its warnings, hide its symbols (statically absorbed).
# (RelWithDebInfo /Ob2 is applied project-wide in the root CMakeLists; mimalloc measured ~1.6x faster on
# small allocations with it, vs xxHash's ~11x — see libs/base/clean-core/docs/benchmarks/.)
if (MSVC)
    target_compile_options(mimalloc PRIVATE /W0 /Zc:__cplusplus)
else()
    target_compile_options(mimalloc PRIVATE -w -fvisibility=hidden)
endif()

set_property(TARGET mimalloc PROPERTY POSITION_INDEPENDENT_CODE ON)
