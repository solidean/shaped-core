cmake_minimum_required(VERSION 3.28)
project(versioned-document LANGUAGES CXX)

# Structured, versioned documents: entities and components materialized from an immutable content-addressed DAG of ops.
# Knows nothing about what a component means — the component set belongs entirely to the application.
# See readme.md and docs/concept.md.
#
# Only fwd.hh exists so far; each milestone in docs/todo/ adds its own sources here.
add_library(versioned-document
    src/versioned-document/fwd.cc
    src/versioned-document/op.cc
    src/versioned-document/op_builder.cc
    src/versioned-document/op_graph.cc
    src/versioned-document/raw_document.cc
    src/versioned-document/value.cc
    src/versioned-document/value_builder.cc
    src/versioned-document/value_debug.cc
)

# Public headers live co-located in src/ for better editor experience.
# FILE_SET models the public API explicitly (install/export, IDEs, etc.).
target_sources(versioned-document
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/versioned-document/fwd.hh
    src/versioned-document/ids.hh
    src/versioned-document/op.hh
    src/versioned-document/op_builder.hh
    src/versioned-document/op_graph.hh
    src/versioned-document/raw_document.hh
    src/versioned-document/value.hh
    src/versioned-document/value_builder.hh
    src/versioned-document/value_debug.hh
)

# Require at least C++23 for this target; the final -std is chosen by the top-level project.
target_compile_features(versioned-document
    PUBLIC
    cxx_std_23
)

# Expose "src" so '#include <versioned-document/...>' works for consumers.
target_include_directories(versioned-document
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# clean-core only, and deliberately so: an in-memory document must not drag a database engine in.
# Persistence lives one library up, in versioned-document-file.
target_link_libraries(versioned-document PUBLIC clean-core)

# Enable conforming preprocessor for MSVC
target_compile_options(versioned-document
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# Test executable
if(SC_BUILD_TESTS)
    add_executable(versioned-document-test
        tests/main.cc

        tests/ids-test.cc
        tests/op-test.cc
        tests/op-fuzz-test.cc
        tests/op_builder-test.cc
        tests/op_graph-test.cc
        tests/value-test.cc
        tests/value-fuzz-test.cc
    )

    target_link_libraries(versioned-document-test
        PRIVATE
        versioned-document
        nexus
    )

    # Browser test runner (Emscripten only); no-op natively.
    sc_add_nexus_web_runner(versioned-document-test)
endif()
