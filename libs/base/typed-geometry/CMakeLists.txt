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

    src/typed-geometry/scalar/traits.hh
    src/typed-geometry/scalar/constants.hh
    src/typed-geometry/scalar/scalar.hh
    src/typed-geometry/scalar/angle.hh
    src/typed-geometry/scalar/all.hh

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
add_executable(typed-geometry-test
    tests/main.cc
    tests/comp-test.cc
    tests/vec-test.cc
    tests/pos-test.cc
    tests/scalar-test.cc
    tests/angle-test.cc
    tests/bivec-test.cc
    tests/mat-test.cc
    tests/quat-test.cc
)

target_link_libraries(typed-geometry-test
    PRIVATE
    typed-geometry
    nexus
)

# Browser test runner (Emscripten only): typed-geometry-test-web.html drives these tests one per frame.
sc_add_nexus_web_runner(typed-geometry-test)
