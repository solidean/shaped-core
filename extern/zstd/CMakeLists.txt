cmake_minimum_required(VERSION 3.28)
project(zstd LANGUAGES C)

# Vendored Zstandard (see vendor-zstd.py for the pinned version, and for why src/
# mirrors upstream's lib/ rather than splitting into include/ + src/).
set(ZSTD_DIR ${CMAKE_CURRENT_SOURCE_DIR})

file(GLOB ZSTD_SOURCES
    ${ZSTD_DIR}/src/common/*.c
    ${ZSTD_DIR}/src/compress/*.c
    ${ZSTD_DIR}/src/decompress/*.c
    ${ZSTD_DIR}/src/dictBuilder/*.c
)

# The one hand-written assembly TU upstream ships: a faster Huffman decode loop for x86-64.
# It is GNU-syntax .S, so it needs both a GNU-style driver and the ASM language enabled;
# where either is missing, ZSTD_DISABLE_ASM below routes zstd to its C implementation instead.
# zstd's own portability_macros.h already refuses the assembly under an MSVC frontend — this only makes
# sure CMake does not hand the file to a compiler that cannot read it in the first place.
set(ZSTD_WITH_ASM OFF)
if (SC_ARCH_X64 AND NOT CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" AND NOT MSVC)
    set(ZSTD_WITH_ASM ON)
endif()

if (ZSTD_WITH_ASM)
    enable_language(ASM)
    list(APPEND ZSTD_SOURCES ${ZSTD_DIR}/src/decompress/huf_decompress_amd64.S)
endif()

add_library(zstd STATIC ${ZSTD_SOURCES})

# SYSTEM so zstd.h does not trip our warnings in the consuming TU.
# src/ itself is the public include dir: the only headers directly in it are zstd.h, zstd_errors.h and zdict.h,
# and everything internal sits one level down where nothing outside can name it.
target_include_directories(zstd SYSTEM PUBLIC ${ZSTD_DIR}/src)

if (NOT ZSTD_WITH_ASM)
    target_compile_definitions(zstd PRIVATE ZSTD_DISABLE_ASM)
endif()

# legacy/ is not vendored, so make sure nothing references it. Upstream defaults this to 0 already;
# stating it means a future default flip fails here rather than at link time.
target_compile_definitions(zstd PRIVATE ZSTD_LEGACY_SUPPORT=0)

# ZSTD_MULTITHREAD is deliberately NOT defined. cc::compress is synchronous by contract, and a caller wanting
# parallelism chunks the work over cc::async rather than handing zstd its own worker pool.
# zstdmt_compress.c and common/pool.c are still compiled: both are entirely inside that guard, so they cost
# nothing here and keep the copy map "upstream's lib/, minus legacy" rather than a hand-curated file list.

# Third-party code: silence its warnings, hide its symbols (statically absorbed).
if (MSVC)
    target_compile_options(zstd PRIVATE /W0)
else()
    target_compile_options(zstd PRIVATE -w -fvisibility=hidden)
endif()

set_property(TARGET zstd PROPERTY POSITION_INDEPENDENT_CODE ON)
