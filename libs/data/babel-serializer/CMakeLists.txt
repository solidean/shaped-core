cmake_minimum_required(VERSION 3.28)
project(babel-serializer LANGUAGES CXX)

# Serialization / deserialization of various formats. Each format parses into an unopinionated, read-once
# structure; opinionated aggregators sit on top later. See readme.md and docs/structure.md.
add_library(babel-serializer
    src/babel-serializer/fwd.cc

    src/babel-serializer/data/base64.cc
    src/babel-serializer/data/json.cc
    src/babel-serializer/data/markdown.cc
    src/babel-serializer/data/sqlite.cc
    src/babel-serializer/geometry/gltf.cc
    src/babel-serializer/geometry/obj.cc

    src/babel-serializer/image/image.cc
    src/babel-serializer/image/jpg.cc
    src/babel-serializer/image/png.cc
    src/babel-serializer/image/impl/stb_backend.cc

    src/babel-serializer/trace/chrome_trace.cc
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

    src/babel-serializer/data/base64.hh
    src/babel-serializer/data/json.hh
    src/babel-serializer/data/markdown.hh
    src/babel-serializer/data/sqlite.hh

    src/babel-serializer/geometry/gltf.hh
    src/babel-serializer/geometry/obj.hh

    # image/impl/*.hh is internal (the stb backend seam) — deliberately NOT in the public FILE_SET.
    src/babel-serializer/image/image.hh
    src/babel-serializer/image/jpg.hh
    src/babel-serializer/image/png.hh

    src/babel-serializer/trace/chrome_trace.hh
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

# clean-core provides streams / containers / result.
# typed-geometry provides vec / pos / mat / quat for the geometry formats.
target_link_libraries(babel-serializer PUBLIC clean-core typed-geometry)

# The SQLite backend is fetched on demand (extern/sqlite), so the target exists only once it has been fetched.
# BABEL_HAS_SQLITE stays PRIVATE and never leaves sqlite.cc, and sqlite3.h never reaches a public header.
# docs/coding-guidelines.md has both rules.
# BABEL_HAS_SQLITE and sqlite3.h both stay PRIVATE, per the header non-leak rule in docs/coding-guidelines.md.
if(TARGET sqlite)
    target_compile_definitions(babel-serializer PRIVATE BABEL_HAS_SQLITE=1)
    target_link_libraries(babel-serializer PRIVATE sqlite)
else()
    target_compile_definitions(babel-serializer PRIVATE BABEL_HAS_SQLITE=0)
endif()

# The stb image backend is committed in-source (extern/stb), so the target always exists and the link is unconditional.
# No if(TARGET stb) and no availability gate, unlike sqlite above.
# stb is linked PRIVATE: the stb headers live only inside image/impl/stb_backend.cc.
target_link_libraries(babel-serializer PRIVATE stb)

# Enable conforming preprocessor for MSVC
target_compile_options(babel-serializer
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# CC_ALL rather than CC_STD, and measured: babel's readers work against cc::read_stream, not the heavy STL headers.
sc_target_pch(babel-serializer CC_ALL) # 0.54

# Test executable
if(SC_BUILD_TESTS)
    add_executable(babel-serializer-test
        tests/main.cc

        tests/data/base64-test.cc
        tests/data/json-test.cc
        tests/data/markdown-test.cc
        tests/data/sqlite-test.cc

        tests/geometry/gltf-test.cc
        tests/geometry/obj-test.cc

        tests/image/image-test.cc

        tests/trace/chrome_trace-test.cc
    )

    target_link_libraries(babel-serializer-test
        PRIVATE
        babel-serializer
        nexus
    )

    sc_target_pch(babel-serializer-test CC_ALL NEXUS) # inherits the library's measured tier

    # Browser test runner (Emscripten only); no-op natively.
    sc_add_nexus_web_runner(babel-serializer-test)
endif()

# Examples: how the API reads in practice. See docs/guides/examples.md.
add_subdirectory(examples)
