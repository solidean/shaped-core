cmake_minimum_required(VERSION 3.28)
project(babel-serializer LANGUAGES CXX)

# Serialization / deserialization of various formats. Each format parses into an unopinionated, read-once
# structure; opinionated aggregators sit on top later. See readme.md and docs/structure.md.
add_library(babel-serializer
    src/babel-serializer/data/json.cc
    src/babel-serializer/geometry/obj.cc
)

# Public headers live co-located in src/ for better editor experience.
# FILE_SET models the public API explicitly (install/export, IDEs, etc.).
target_sources(babel-serializer
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/babel-serializer/fwd.hh
    src/babel-serializer/all.hh

    src/babel-serializer/data/json.hh

    src/babel-serializer/geometry/obj.hh
)

# Require at least C++23 for this target; the final -std is chosen by the top-level project.
target_compile_features(babel-serializer
    PUBLIC
    cxx_std_23
)

# Expose "src" so '#include <babel-serializer/...>' works for consumers.
target_include_directories(babel-serializer
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# clean-core provides streams / containers / result; typed-geometry provides vec / pos for the geometry formats.
target_link_libraries(babel-serializer PUBLIC clean-core typed-geometry)

# Enable conforming preprocessor for MSVC
target_compile_options(babel-serializer
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# Test executable
if(SC_BUILD_TESTS)
    add_executable(babel-serializer-test
        tests/main.cc

        tests/data/json-test.cc

        tests/geometry/obj-test.cc
    )

    target_link_libraries(babel-serializer-test
        PRIVATE
        babel-serializer
        nexus
    )

    # Browser test runner (Emscripten only); no-op natively.
    sc_add_nexus_web_runner(babel-serializer-test)
endif()
