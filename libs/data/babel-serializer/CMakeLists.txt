cmake_minimum_required(VERSION 3.28)
project(babel-serializer LANGUAGES CXX)

# Serialization / deserialization of various formats. Each format parses into an unopinionated, read-once
# structure; opinionated aggregators sit on top later. See readme.md and docs/structure.md.
add_library(babel-serializer
    src/babel-serializer/fwd.cc

    src/babel-serializer/data/sqlite.cc
    src/babel-serializer/geometry/gltf.cc
    src/babel-serializer/geometry/obj.cc
    src/babel-serializer/geometry/stl.cc

    src/babel-serializer/image/hdr.cc
    src/babel-serializer/image/image.cc
    src/babel-serializer/image/jpg.cc
    src/babel-serializer/image/pfm.cc
    src/babel-serializer/image/png.cc
    src/babel-serializer/image/impl/spng_backend.cc
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
    src/babel-serializer/all.hh
    src/babel-serializer/fwd.hh

    src/babel-serializer/data/sqlite.hh

    src/babel-serializer/geometry/gltf.hh
    src/babel-serializer/geometry/obj.hh
    src/babel-serializer/geometry/stl.hh

    # image/impl/*.hh is internal (the libspng and stb backend seams) — deliberately NOT in the public FILE_SET.
    src/babel-serializer/image/hdr.hh
    src/babel-serializer/image/image.hh
    src/babel-serializer/image/jpg.hh
    src/babel-serializer/image/pfm.hh
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

# babel-data is the sibling library next door (libs/data/babel-data): base64, JSON and markdown over clean-core.
# PUBLIC, so linking babel-serializer still gets `babel::json` and everything else the umbrella name implies.
# typed-geometry provides vec / pos / mat / quat for the geometry formats.
target_link_libraries(babel-serializer PUBLIC babel-data typed-geometry)

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

# Both image backends are committed in-source (extern/libspng, extern/stb), so the targets always exist and the links are unconditional.
# No if(TARGET ...) and no availability gate, unlike sqlite above.
# Both are PRIVATE, and each third-party header lives in exactly one TU: spng.h in image/impl/spng_backend.cc, stb's in image/impl/stb_backend.cc.
# libspng is the PNG codec; stb is what JPEG still decodes and encodes through.
target_link_libraries(babel-serializer PRIVATE spng stb)

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

        tests/babel-recording-test.cc

        tests/data/sqlite-test.cc

        tests/geometry/gltf-test.cc
        tests/geometry/obj-test.cc
        tests/geometry/stl-test.cc

        tests/image/hdr-test.cc
        tests/image/image-test.cc
        tests/image/pfm-test.cc

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
