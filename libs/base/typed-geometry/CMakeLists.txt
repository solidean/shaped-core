cmake_minimum_required(VERSION 3.28)
project(typed-geometry LANGUAGES CXX)

# typed-geometry is header-dominant but not header-only: it is a normal static library so that
# concrete (non-template) algorithms can land in .cc files as the library grows. For now a single
# anchor translation unit keeps the static target well-formed.
add_library(typed-geometry
    src/typed-geometry/typed-geometry.cc
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
    src/typed-geometry/scalar/all.hh

    src/typed-geometry/linalg/fwd.hh
    src/typed-geometry/linalg/comp.hh
    src/typed-geometry/linalg/comp_ops.hh
    src/typed-geometry/linalg/vec.hh
    src/typed-geometry/linalg/vec_ops.hh
    src/typed-geometry/linalg/pos.hh
    src/typed-geometry/linalg/pos_ops.hh
    src/typed-geometry/linalg/bivec.hh
    src/typed-geometry/linalg/cross.hh
    src/typed-geometry/linalg/mat.hh
    src/typed-geometry/linalg/quat.hh
    src/typed-geometry/linalg/linalg.hh
    src/typed-geometry/linalg/all.hh

    src/typed-geometry/geometry/fwd.hh
    src/typed-geometry/geometry/traits.hh
    src/typed-geometry/geometry/primitives/aabb.hh
    src/typed-geometry/geometry/primitives/triangle.hh
    src/typed-geometry/geometry/primitives/segment.hh
    src/typed-geometry/geometry/primitives/ray.hh
    src/typed-geometry/geometry/primitives/line.hh
    src/typed-geometry/geometry/primitives/plane.hh
    src/typed-geometry/geometry/primitives/primitives.hh
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

# Enable conforming preprocessor for MSVC
target_compile_options(typed-geometry
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# Test executable
if(SC_BUILD_TESTS)
    add_executable(typed-geometry-test
        tests/main.cc

        tests/scalar/scalar-test.cc
        tests/scalar/angle-test.cc

        tests/linalg/comp-test.cc
        tests/linalg/vec-test.cc
        tests/linalg/pos-test.cc
        tests/linalg/bivec-test.cc
        tests/linalg/mat-test.cc
        tests/linalg/quat-test.cc

        tests/geometry/aabb-test.cc
        tests/geometry/triangle-test.cc
        tests/geometry/segment-test.cc
        tests/geometry/ray-test.cc
        tests/geometry/line-test.cc
        tests/geometry/plane-test.cc
        tests/geometry/object-traits-test.cc
    )

    target_link_libraries(typed-geometry-test
        PRIVATE
        typed-geometry
        nexus
    )

    # Browser test runner (Emscripten only): typed-geometry-test-web.html drives these tests one per frame.
    sc_add_nexus_web_runner(typed-geometry-test)
endif()
