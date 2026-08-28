cmake_minimum_required(VERSION 3.28)
project(spng LANGUAGES C)

# Vendored libspng (see vendor-libspng.py for the pinned version, and for what is dropped).
# One TU: upstream is spng.c and spng.h and nothing else.
# The target is `spng` rather than `libspng` — that is the name upstream's own pkg-config and header carry.
set(SPNG_DIR ${CMAKE_CURRENT_SOURCE_DIR})

add_library(spng STATIC
    ${SPNG_DIR}/src/spng.c
)

# SYSTEM so spng.h does not trip our warnings in the consuming TU.
# include/ holds spng.h and nothing else, which is exactly the public API.
target_include_directories(spng SYSTEM PUBLIC ${SPNG_DIR}/include)

# spng.c reaches spng.h with a quote include. That misses in src/ and then resolves against
# include/ above, so src/ only has to be visible to spng's own TU.
target_include_directories(spng PRIVATE ${SPNG_DIR}/src)

# PUBLIC, and it must stay that way: without it spng.h declares its API __declspec(dllimport) on Windows,
# so a consumer would link against an import stub that no DLL exists to satisfy.
target_compile_definitions(spng PUBLIC SPNG_STATIC)

# libspng carries no inflate/deflate of its own; zlib is the alternative to upstream's SPNG_USE_MINIZ,
# and the one we take because zlib is already in the tree for cc::compress.
# PRIVATE: zlib.h is included by spng.c, never by spng.h, so it stops at this target.
target_link_libraries(spng PRIVATE zlib)

# SIMD needs no configuration: spng.c selects SSE2 on x86 and NEON on arm64 from the predefined macros,
# and both are baseline on every architecture we target.

# Third-party code: silence its warnings, hide its symbols (statically absorbed).
if (MSVC)
    target_compile_options(spng PRIVATE /W0)
else()
    target_compile_options(spng PRIVATE -w -fvisibility=hidden)
endif()

set_property(TARGET spng PROPERTY POSITION_INDEPENDENT_CODE ON)
