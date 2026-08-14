cmake_minimum_required(VERSION 3.28)
project(versioned-document-file LANGUAGES CXX)

# The `.vdoc` save format: one SQLite file holding a document's op DAG, its refs and snapshots, its embedded assets and their blobs, and a disposable workspace.
# See readme.md and docs/format.md.
#
# Only fwd.hh exists so far; milestones 4 through 6 in ../versioned-document/docs/todo/ add the sources here.
add_library(versioned-document-file
    src/versioned-document-file/fwd.cc
)

# Public headers live co-located in src/ for better editor experience.
# FILE_SET models the public API explicitly (install/export, IDEs, etc.).
target_sources(versioned-document-file
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/versioned-document-file/fwd.hh
)

# Require at least C++23 for this target; the final -std is chosen by the top-level project.
target_compile_features(versioned-document-file
    PUBLIC
    cxx_std_23
)

# Expose "src" so '#include <versioned-document-file/...>' works for consumers.
target_include_directories(versioned-document-file
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# versioned-document is the model this persists.
# babel-serializer is the storage engine, through babel::sqlite; it is linked PRIVATE so no sqlite type reaches a public header of ours.
# babel probes its own backend at runtime (babel::sqlite::is_available), so a checkout that has not fetched SQLite still builds and reports the absence when a file is opened.
target_link_libraries(versioned-document-file PUBLIC versioned-document)
target_link_libraries(versioned-document-file PRIVATE babel-serializer)

# Enable conforming preprocessor for MSVC
target_compile_options(versioned-document-file
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# The versioned-document-file-test executable arrives with milestone 4, together with the conformance suite both store implementations must pass.
# See ../versioned-document/docs/todo/milestone-4.md.
