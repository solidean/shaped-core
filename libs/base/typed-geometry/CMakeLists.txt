cmake_minimum_required(VERSION 3.28)
project(typed-geometry LANGUAGES CXX)

# typed-geometry is header-dominant but not header-only: it is a normal static library so that
# concrete (non-template) algorithms can land in .cc files next to the header they implement.
add_library(typed-geometry
    src/typed-geometry/typed-geometry.cc
    src/typed-geometry/scalar/half_float.cc
)

# Public headers live co-located in src/ for better editor experience.
# FILE_SET models the public API explicitly (install/export, IDEs, etc.).
target_sources(typed-geometry
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/typed-geometry/fwd.hh
    src/typed-geometry/all.hh

    src/typed-geometry/scalar/fwd.hh
    src/typed-geometry/scalar/traits.hh
    src/typed-geometry/scalar/constants.hh
    src/typed-geometry/scalar/scalar.hh
    src/typed-geometry/scalar/angle.hh
    src/typed-geometry/scalar/half_float.hh
    src/typed-geometry/scalar/impl/half_float_convert.hh
    src/typed-geometry/scalar/impl/half_float_ops.hh
    src/typed-geometry/scalar/all.hh
    src/typed-geometry/scalar/fixed_int/impl/core.hh
    src/typed-geometry/scalar/fixed_int/generated/all.hh
    src/typed-geometry/scalar/fixed_int/generated/w32.hh
    src/typed-geometry/scalar/fixed_int/generated/w64.hh
    src/typed-geometry/scalar/fixed_int/generated/w128.hh
    src/typed-geometry/scalar/fixed_int/generated/w192.hh
    src/typed-geometry/scalar/fixed_int/generated/w256.hh
    src/typed-geometry/scalar/fixed_int/fixed_int.hh
    src/typed-geometry/scalar/fixed_int/fixed_arith.hh

    src/typed-geometry/linalg/fwd.hh
    src/typed-geometry/linalg/comp.hh
    src/typed-geometry/linalg/comp_ops.hh
    src/typed-geometry/linalg/vec.hh
    src/typed-geometry/linalg/vec_ops.hh
    src/typed-geometry/linalg/pos.hh
    src/typed-geometry/linalg/bivec.hh
    src/typed-geometry/linalg/cross.hh
    src/typed-geometry/linalg/mat.hh
    src/typed-geometry/linalg/quat.hh
    src/typed-geometry/linalg/linalg.hh
    src/typed-geometry/linalg/all.hh

    src/typed-geometry/transform/fwd.hh
    src/typed-geometry/transform/transform_flags.hh
    src/typed-geometry/transform/impl/rotation_representation.hh
    src/typed-geometry/transform/impl/transform_representation.hh
    src/typed-geometry/transform/impl/transform_representation_access.hh
    src/typed-geometry/transform/homogeneous_transform.hh

    src/typed-geometry/transform/composed_transform.hh
    src/typed-geometry/transform/compose.hh
    src/typed-geometry/transform/transform.hh
    src/typed-geometry/transform/all.hh

    src/typed-geometry/geometry/fwd.hh
    src/typed-geometry/geometry/traits.hh
    src/typed-geometry/geometry/primitives/aabb.hh
    src/typed-geometry/geometry/primitives/triangle.hh
    src/typed-geometry/geometry/primitives/segment.hh
    src/typed-geometry/geometry/primitives/ray.hh
    src/typed-geometry/geometry/primitives/line.hh
    src/typed-geometry/geometry/primitives/plane.hh
    src/typed-geometry/geometry/primitives/halfspace.hh
    src/typed-geometry/geometry/primitives/box.hh
    src/typed-geometry/geometry/primitives/sphere.hh
    src/typed-geometry/geometry/primitives/ellipsoid.hh
    src/typed-geometry/geometry/primitives/primitives.hh
    src/typed-geometry/geometry/query/closest_points.hh
    src/typed-geometry/geometry/query/contains.hh
    src/typed-geometry/geometry/query/distance.hh
    src/typed-geometry/geometry/query/impl/kernels.hh
    src/typed-geometry/geometry/query/impl/kernels/aabb.hh
    src/typed-geometry/geometry/query/impl/kernels/box.hh
    src/typed-geometry/geometry/query/impl/kernels/constructive.hh
    src/typed-geometry/geometry/query/impl/kernels/halfspace.hh
    src/typed-geometry/geometry/query/impl/kernels/hot_pairs.hh
    src/typed-geometry/geometry/query/impl/kernels/linear.hh
    src/typed-geometry/geometry/query/impl/kernels/parameters.hh
    src/typed-geometry/geometry/query/impl/kernels/plane.hh
    src/typed-geometry/geometry/query/impl/kernels/pos.hh
    src/typed-geometry/geometry/query/impl/kernels/sphere.hh
    src/typed-geometry/geometry/query/impl/kernels/triangle.hh
    src/typed-geometry/geometry/query/impl/epa.hh
    src/typed-geometry/geometry/query/impl/gjk.hh
    src/typed-geometry/geometry/query/impl/ops.hh
    src/typed-geometry/geometry/query/impl/special_case.hh
    src/typed-geometry/geometry/query/hits.hh
    src/typed-geometry/geometry/query/intersection.hh
    src/typed-geometry/geometry/query/intersects.hh
    src/typed-geometry/geometry/query/parameter.hh
    src/typed-geometry/geometry/query/project.hh
    src/typed-geometry/geometry/query/separation.hh
    src/typed-geometry/geometry/query/query.hh
    src/typed-geometry/geometry/impl/bounds_of.hh
    src/typed-geometry/geometry/impl/sampling.hh
    src/typed-geometry/geometry/geometry.hh
    src/typed-geometry/geometry/all.hh
)

# Require at least C++23 for this target; the final -std is chosen by the top-level project.
target_compile_features(typed-geometry
    PUBLIC
    cxx_std_23
)

# Expose "src" so '#include <typed-geometry/...>' works for consumers.
target_include_directories(typed-geometry
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# clean-core provides the vocabulary types (i32/f32/isize) and assertions used across the public API.
target_link_libraries(typed-geometry PUBLIC clean-core)

# The root CMakeLists declares SC_CHECK_WIDE_ARITH; this is only the fallback for a consumer that adds tg alone.
if(NOT DEFINED SC_CHECK_WIDE_ARITH)
    set(SC_CHECK_WIDE_ARITH OFF)
endif()
# The input define; scalar/fixed_int/impl/core.hh turns it into TG_CHECK_WIDE_ARITH (always 0 or 1).
target_compile_definitions(typed-geometry
    PUBLIC
    $<$<BOOL:${SC_CHECK_WIDE_ARITH}>:TG_CHECK_WIDE_ARITH_REQUESTED>
)

# Same pattern for SC_CHECK_GEOMETRY_SPECIAL_CASES; geometry/query/impl/special_case.hh turns it into TG_CHECK_SPECIAL_CASES.
if(NOT DEFINED SC_CHECK_GEOMETRY_SPECIAL_CASES)
    set(SC_CHECK_GEOMETRY_SPECIAL_CASES OFF)
endif()
target_compile_definitions(typed-geometry
    PUBLIC
    $<$<BOOL:${SC_CHECK_GEOMETRY_SPECIAL_CASES}>:TG_CHECK_SPECIAL_CASES_REQUESTED>
)

# Enable conforming preprocessor for MSVC
target_compile_options(typed-geometry
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

sc_declare_pch_tier(TG EXTENDS CC_STD HEADERS <typed-geometry/all.hh>)

sc_target_pch(typed-geometry TG)

# Test executable
if(SC_BUILD_TESTS)
    add_executable(typed-geometry-test
        tests/main.cc

        tests/scalar/scalar-test.cc
        tests/scalar/angle-test.cc
        tests/scalar/half_float-test.cc
        tests/scalar/fixed_int-test.cc
        tests/scalar/fixed_int-golden.gen.cc

        tests/benchmarks/fixed_int-benchmark.cc
        tests/benchmarks/half_float-benchmark.cc
        tests/benchmarks/sampling-benchmark.cc
        tests/benchmarks/query-benchmark.cc

        tests/linalg/comp-test.cc
        tests/linalg/vec-test.cc
        tests/linalg/pos-test.cc
        tests/linalg/bivec-test.cc
        tests/linalg/mat-test.cc
        tests/linalg/quat-test.cc

        tests/transform/transform-flags-test.cc
        tests/transform/homogeneous-transform-test.cc
        tests/transform/compose-test.cc
        tests/transform/inverse-test.cc
        tests/transform/transform-ops-test.cc
        tests/transform/dispatch-test.cc
        tests/transform/projective-test.cc

        tests/geometry/aabb-test.cc
        tests/geometry/sphere-test.cc
        tests/geometry/ellipsoid-test.cc
        tests/geometry/triangle-test.cc
        tests/geometry/segment-test.cc
        tests/geometry/ray-test.cc
        tests/geometry/line-test.cc
        tests/geometry/plane-test.cc
        tests/geometry/halfspace-test.cc
        tests/geometry/box-test.cc
        tests/geometry/object-traits-test.cc
        tests/geometry/measures-test.cc
        tests/geometry/sampling-test.cc
        tests/geometry/query/ladder-test.cc
        tests/geometry/query/point-queries-test.cc
        tests/geometry/query/special-case-test.cc
        tests/geometry/query/gjk-test.cc
        tests/geometry/query/wave1-queries-test.cc
        tests/geometry/query/intersection-test.cc
        tests/geometry/query/hot-pairs-test.cc
    )
    sc_nexus_binary(typed-geometry-test KINDS tests)

    target_link_libraries(typed-geometry-test
        PRIVATE
        typed-geometry
        nexus
    )

    sc_target_pch(typed-geometry-test TG NEXUS) # 0.73 — the least include-bound target measured

    # Browser test runner (Emscripten only): typed-geometry-test-web.html drives these tests one per frame.
    sc_add_nexus_web_runner(typed-geometry-test)
endif()
