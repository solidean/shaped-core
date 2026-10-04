cmake_minimum_required(VERSION 3.28)
project(clean-simd LANGUAGES CXX)

# SIMD for shaped-core: fixed-width lane types templated on a kernel, so one binary can dispatch between instruction sets.
# See readme.md and docs/design.md.
#
# The types are generated (tools/gen-simd.py, committed output under src/clean-simd/generated/), and a kernel's code is
# compiled only where the TU's flags allow it — this target sets no ISA flag of its own, ever.
add_library(clean-simd
    src/clean-simd/dispatch.cc
)

target_sources(clean-simd
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/clean-simd/fwd.hh
    src/clean-simd/all.hh
    src/clean-simd/kernel.hh
    src/clean-simd/simd.hh
    src/clean-simd/dispatch.hh
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

target_compile_features(clean-simd PUBLIC cxx_std_23)
target_include_directories(clean-simd PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(clean-simd PUBLIC clean-core)

# cimd_dispatch(), for any target added after this one.
include(cmake/Dispatch.cmake)

if(SC_BUILD_TESTS)
    # The acceptance fixture: an 8-wide BVH whose query is dispatched from inside a static library, which is the link
    # case the kernel-library ordering has to hold for.
    add_library(clean-simd-bvh-fixture STATIC tests/bvh/bvh8.cc)
    target_link_libraries(clean-simd-bvh-fixture PUBLIC clean-simd)
    target_include_directories(clean-simd-bvh-fixture PUBLIC tests/bvh)
    cimd_dispatch(clean-simd-bvh-fixture NAME bvh8_query_dispatched HEADER tests/bvh/bvh8-query.hh FUNCTION bvh8_query)

    add_executable(clean-simd-test
        tests/main.cc
        tests/simd-test.cc
        tests/dispatch/dispatch-test.cc
        tests/bvh/bvh-test.cc
    )
    sc_nexus_binary(clean-simd-test KINDS tests)
    target_link_libraries(clean-simd-test PRIVATE clean-simd clean-simd-bvh-fixture nexus)
    target_include_directories(clean-simd-test PRIVATE tests/dispatch)
    cimd_dispatch(clean-simd-test NAME cimd_battery HEADER tests/dispatch/battery.hh FUNCTION battery)
    cimd_check_link_map(clean-simd-test)
    sc_add_nexus_web_runner(clean-simd-test)
endif()
