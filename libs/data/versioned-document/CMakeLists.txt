cmake_minimum_required(VERSION 3.28)
project(versioned-document LANGUAGES CXX)

# Structured, versioned documents: entities and components materialized from an immutable content-addressed DAG of ops.
# Knows nothing about what a component means — the component set belongs entirely to the application.
# See readme.md and docs/concept.md.
#
# Only fwd.hh exists so far; each milestone in docs/todo/ adds its own sources here.
add_library(versioned-document
    src/versioned-document/fwd.cc
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

# The versioned-document-test executable arrives with milestone 1, which lands the first testable surface (the value codec).
# See docs/todo/milestone-1.md.
