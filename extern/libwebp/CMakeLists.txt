cmake_minimum_required(VERSION 3.28)
project(webp LANGUAGES C)

# Vendored libwebp (see vendor-libwebp.py for the pinned version, and for what is dropped).
# src/ and sharpyuv/ keep upstream's paths: the sources include each other as "src/dsp/dsp.h" and "sharpyuv/sharpyuv.h".
set(WEBP_DIR ${CMAKE_CURRENT_SOURCE_DIR})

file(GLOB WEBP_SOURCES
    ${WEBP_DIR}/src/dec/*.c
    ${WEBP_DIR}/src/enc/*.c
    ${WEBP_DIR}/src/dsp/*.c
    ${WEBP_DIR}/src/utils/*.c
    ${WEBP_DIR}/sharpyuv/*.c
)

add_library(webp STATIC ${WEBP_SOURCES})

# SYSTEM so webp's headers do not trip our warnings in the consuming TU.
# src/ is the public include dir, so a consumer writes <webp/encode.h> and <webp/decode.h>, as upstream's install does.
target_include_directories(webp SYSTEM PUBLIC ${WEBP_DIR}/src)

# The repository root, for the sources' own "src/..." and "sharpyuv/..." includes; private to webp.
target_include_directories(webp PRIVATE ${WEBP_DIR})

# SIMD is chosen at run time: src/dsp/cpu.c probes the CPU and each Init() installs the best variant compiled in.
# SSE2 and NEON are baseline on every architecture we target and need no flag.
# The SSE4.1 and AVX2 variants use intrinsics a GNU-style clang or gcc only compiles for a target that has them, so those
# files alone get the flag; the run-time probe keeps them off a CPU without the instructions.
# MSVC compiles the intrinsics without a flag, and cpu.h enables the variants for it by its own version check.
if (SC_ARCH_X64)
    file(GLOB WEBP_SSE41 ${WEBP_DIR}/src/dsp/*_sse41.c ${WEBP_DIR}/sharpyuv/*_sse41.c)
    file(GLOB WEBP_AVX2 ${WEBP_DIR}/src/dsp/*_avx2.c ${WEBP_DIR}/sharpyuv/*_avx2.c)
    if (CMAKE_C_COMPILER_ID MATCHES "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set_source_files_properties(${WEBP_SSE41} PROPERTIES COMPILE_OPTIONS "/clang:-msse4.1")
        set_source_files_properties(${WEBP_AVX2} PROPERTIES COMPILE_OPTIONS "/clang:-mavx2")
    elseif (NOT MSVC)
        set_source_files_properties(${WEBP_SSE41} PROPERTIES COMPILE_OPTIONS "-msse4.1")
        set_source_files_properties(${WEBP_AVX2} PROPERTIES COMPILE_OPTIONS "-mavx2")
    endif()
endif()

# WEBP_USE_THREAD is deliberately NOT defined: babel's codecs are synchronous, and a caller wanting parallelism runs
# several encodes over cc::async rather than handing libwebp threads of its own.

# Third-party code: silence its warnings, hide its symbols (statically absorbed).
if (MSVC)
    target_compile_options(webp PRIVATE /W0)
else()
    target_compile_options(webp PRIVATE -w -fvisibility=hidden)
endif()

set_property(TARGET webp PROPERTY POSITION_INDEPENDENT_CODE ON)
