cmake_minimum_required(VERSION 3.28)
project(clean-simd LANGUAGES CXX)

# SIMD for shaped-core: fixed-width lane types templated on a kernel, so one binary can dispatch between instruction sets.
# See readme.md and docs/design.md.
#
# The types are generated (tools/gen-simd.py, committed output under src/clean-simd/generated/), and a kernel's code is
# compiled only where the TU's flags allow it — this target sets no ISA flag of its own, ever.
add_library(clean-simd INTERFACE)

target_sources(clean-simd
    INTERFACE
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/clean-simd/fwd.hh
    src/clean-simd/all.hh
    src/clean-simd/kernel.hh
    src/clean-simd/simd.hh
    src/clean-simd/impl/intrinsics.hh
    src/clean-simd/generated/scalar.hh
    src/clean-simd/generated/sse2.hh
    src/clean-simd/generated/sse42.hh
    src/clean-simd/generated/avx2.hh
    src/clean-simd/generated/avx512.hh
    src/clean-simd/generated/neon.hh
    src/clean-simd/generated/simd128.hh
    src/clean-simd/generated/fixed/masks.hh
    src/clean-simd/generated/fixed/f32.hh
    src/clean-simd/generated/fixed/i32.hh
    src/clean-simd/generated/fixed/u32.hh
)

target_compile_features(clean-simd INTERFACE cxx_std_23)
target_include_directories(clean-simd INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(clean-simd INTERFACE clean-core)

if(SC_BUILD_TESTS)
    add_executable(clean-simd-test
        tests/main.cc
        tests/simd-test.cc
    )
    sc_nexus_binary(clean-simd-test KINDS tests)
    target_link_libraries(clean-simd-test PRIVATE clean-simd nexus)
    sc_add_nexus_web_runner(clean-simd-test)
endif()
