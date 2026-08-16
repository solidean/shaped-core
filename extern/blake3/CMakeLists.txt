cmake_minimum_required(VERSION 3.28)
project(blake3 LANGUAGES C)

# Vendored BLAKE3 (see vendor-blake3.py for the pinned version). Upstream's C
# implementation: one portable TU plus per-ISA TUs selected at runtime by
# blake3_dispatch.c.
set(BLAKE3_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Which per-ISA TUs exist must agree with what blake3_impl.h thinks is available,
# or dispatch references a symbol nothing compiled. Upstream autodetects from the
# compiler's own arch macros; we mirror that with CMAKE_SYSTEM_PROCESSOR (via
# SC_ARCH_X64) and define the BLAKE3_NO_* escape hatches on everything else, so a
# disagreement is a compile error here rather than a link error downstream.
set(BLAKE3_SOURCES
    ${BLAKE3_DIR}/src/blake3.c
    ${BLAKE3_DIR}/src/blake3_dispatch.c
    ${BLAKE3_DIR}/src/blake3_portable.c
)

set(BLAKE3_WITH_NEON OFF)
if (CMAKE_SYSTEM_PROCESSOR MATCHES "^([Aa][Aa][Rr][Cc][Hh]64|[Aa][Rr][Mm]64)$")
    set(BLAKE3_WITH_NEON ON)
elseif (APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    # An Apple cross build — iOS above all — leaves CMAKE_SYSTEM_PROCESSOR naming the HOST, so the architecture list
    # is the only thing that says what is actually being targeted.
    set(BLAKE3_WITH_NEON ON)
endif()

if (SC_ARCH_X64)
    list(APPEND BLAKE3_SOURCES
        ${BLAKE3_DIR}/src/blake3_sse2.c
        ${BLAKE3_DIR}/src/blake3_sse41.c
        ${BLAKE3_DIR}/src/blake3_avx2.c
        ${BLAKE3_DIR}/src/blake3_avx512.c
    )
elseif (BLAKE3_WITH_NEON)
    list(APPEND BLAKE3_SOURCES ${BLAKE3_DIR}/src/blake3_neon.c)
endif()

add_library(blake3 STATIC ${BLAKE3_SOURCES})

# SYSTEM so blake3.h does not trip our warnings in the consuming TU.
target_include_directories(blake3 SYSTEM PUBLIC ${BLAKE3_DIR}/include)

# blake3_impl.h is upstream-internal and stays out of the public include dir, but
# every TU here includes it — and it in turn includes "blake3.h", resolved above.
target_include_directories(blake3 PRIVATE ${BLAKE3_DIR}/src)

if (SC_ARCH_X64)
    # Each per-ISA TU is compiled for its own instruction set; blake3_dispatch.c
    # picks between them at runtime, so the flags never reach the entry point.
    # The intrinsics implementations only — none of upstream's hand-written
    # assembly is vendored, which keeps this a pure C build with no ASM language.
    if (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        # clang-cl takes the -m spellings too, so this covers the Windows clang build.
        set_source_files_properties(${BLAKE3_DIR}/src/blake3_sse2.c PROPERTIES COMPILE_OPTIONS "-msse2")
        set_source_files_properties(${BLAKE3_DIR}/src/blake3_sse41.c PROPERTIES COMPILE_OPTIONS "-msse4.1")
        set_source_files_properties(${BLAKE3_DIR}/src/blake3_avx2.c PROPERTIES COMPILE_OPTIONS "-mavx2")
        set_source_files_properties(${BLAKE3_DIR}/src/blake3_avx512.c PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512vl")
    elseif (MSVC)
        # SSE2 and SSE4.1 intrinsics need no flag on x64; AVX does.
        set_source_files_properties(${BLAKE3_DIR}/src/blake3_avx2.c PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
        set_source_files_properties(${BLAKE3_DIR}/src/blake3_avx512.c PROPERTIES COMPILE_OPTIONS "/arch:AVX512")
    endif()
else()
    # Nothing x86 was compiled, so make sure nothing x86 is referenced either.
    target_compile_definitions(blake3 PRIVATE BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512)
endif()

# The NEON counterpart, and it is NOT optional.
#
# Upstream autodetects BLAKE3_USE_NEON to 1 on any AArch64 target, so leaving it alone is only safe where the detection
# above agreed and compiled blake3_neon.c.
# Where it did not, dispatch still calls blake3_hash_many_neon and the failure lands at LINK time, in whatever binary
# happens to pull blake3 in — which is how an iOS build reported an undefined symbol out of clean-core-test.
# Pinning it here means a platform this file fails to recognize degrades to the portable path instead.
if (NOT BLAKE3_WITH_NEON)
    target_compile_definitions(blake3 PRIVATE BLAKE3_USE_NEON=0)
endif()

# Third-party code: silence its warnings, hide its symbols (statically absorbed).
if (MSVC)
    target_compile_options(blake3 PRIVATE /W0)
else()
    target_compile_options(blake3 PRIVATE -w -fvisibility=hidden)
endif()

set_property(TARGET blake3 PROPERTY POSITION_INDEPENDENT_CODE ON)
