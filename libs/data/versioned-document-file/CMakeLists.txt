cmake_minimum_required(VERSION 3.28)
project(versioned-document-file LANGUAGES CXX)

# The `.vdoc` save format: one SQLite file holding a document's op DAG, its refs and snapshots, its embedded assets and their blobs, and a disposable workspace.
# See readme.md and docs/format.md.
#
# Milestones 4 and 5 land the store, its two implementations, the loader and the content store; snapshots follow in milestone 6.
# See ../versioned-document/docs/todo/.
add_library(versioned-document-file
    src/versioned-document-file/fwd.cc
    src/versioned-document-file/diagnostics.cc
    src/versioned-document-file/assets.cc
    src/versioned-document-file/store.cc
    src/versioned-document-file/impl/payload_codec.cc
    src/versioned-document-file/impl/snapshot_write.cc
    src/versioned-document-file/impl/snapshot_codec.cc
    src/versioned-document-file/impl/blob_fetch.cc
    src/versioned-document-file/impl/load.cc
    src/versioned-document-file/impl/publish.cc
    src/versioned-document-file/impl/reclaim.cc
    src/versioned-document-file/impl/recovery.cc
    src/versioned-document-file/impl/store_memory.cc
    src/versioned-document-file/impl/sqlite_schema.cc
    src/versioned-document-file/impl/sqlite_io.cc
    src/versioned-document-file/impl/sqlite_actor.cc
    src/versioned-document-file/impl/store_sqlite.cc
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
    src/versioned-document-file/diagnostics.hh
    src/versioned-document-file/assets.hh
    src/versioned-document-file/workspace.hh
    src/versioned-document-file/memory_image.hh
    src/versioned-document-file/store.hh
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

if(SC_BUILD_TESTS)
    # The conformance suite is the centre of this binary: one set of tests, parametrized over both store implementations.
    add_executable(versioned-document-file-test
        tests/main.cc
        tests/assets-test.cc
        tests/diagnostics-test.cc
        tests/snapshot_codec-test.cc
        tests/snapshot_codec-test.cc
        tests/conformance/store_fixture.cc
        tests/conformance/sqlite_medium.cc
        tests/conformance/drivers.cc
        tests/conformance/conformance-test.cc
        tests/conformance/assets-conformance-test.cc
        tests/conformance/recovery-conformance-test.cc
        tests/conformance/snapshots-conformance-test.cc
    )

    # babel-serializer is linked for the test fixture alone, which fabricates a damaged file on purpose.
    # No library source here includes it, which is what keeps .shaped-lint.yml free of a sqlite entry.
    target_link_libraries(versioned-document-file-test PRIVATE versioned-document-file babel-serializer nexus)

    sc_add_nexus_web_runner(versioned-document-file-test)
endif()
