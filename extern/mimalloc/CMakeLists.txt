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
if (WIN32)
    target_link_libraries(mimalloc PUBLIC psapi shell32 user32 advapi32 bcrypt)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(mimalloc PUBLIC Threads::Threads)
endif()

# Third-party code: silence its warnings, hide its symbols (statically absorbed).
if (MSVC)
    target_compile_options(mimalloc PRIVATE /W0 /Zc:__cplusplus)
else()
    target_compile_options(mimalloc PRIVATE -w -fvisibility=hidden)
endif()

set_property(TARGET mimalloc PROPERTY POSITION_INDEPENDENT_CODE ON)
