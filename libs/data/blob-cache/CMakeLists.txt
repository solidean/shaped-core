cmake_minimum_required(VERSION 3.28)
project(blob-cache LANGUAGES CXX)

# A persistent, multi-process blob cache: (namespace, key, version) -> content-addressed bytes on local disk.
# See readme.md and docs/design.md.
add_library(blob-cache
    src/blob-cache/fwd.cc
    src/blob-cache/keys.cc
    src/blob-cache/blob_cache.cc
    src/blob-cache/impl/singleflight.cc
    src/blob-cache/impl/cache_schema.cc
    src/blob-cache/impl/cache_io.cc
    src/blob-cache/impl/cache_gc.cc
    src/blob-cache/impl/cache_actor.cc
)

# Public headers live co-located in src/ for better editor experience.
# FILE_SET models the public API explicitly (install/export, IDEs, etc.).
target_sources(blob-cache
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/blob-cache/fwd.hh
    src/blob-cache/keys.hh
    src/blob-cache/blob_cache.hh
)

# Require at least C++23 for this target; the final -std is chosen by the top-level project.
target_compile_features(blob-cache
    PUBLIC
    cxx_std_23
)

# Expose "src" so '#include <blob-cache/...>' works for consumers.
target_include_directories(blob-cache
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# babel-serializer is the storage engine, through babel::sqlite; it is linked PRIVATE so no sqlite type reaches a public header of ours.
# babel probes its own backend at runtime (babel::sqlite::is_available), so a checkout that has not fetched SQLite still builds — the cache then simply misses on everything.
target_link_libraries(blob-cache PUBLIC clean-core)
target_link_libraries(blob-cache PRIVATE babel-serializer)

# Enable conforming preprocessor for MSVC
target_compile_options(blob-cache
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

if(SC_BUILD_TESTS)
    add_executable(blob-cache-test
        tests/main.cc
        tests/cache_fixture.cc
        tests/put-get-test.cc
        tests/first-writer-test.cc
        tests/singleflight-test.cc
        tests/keys-test.cc
        tests/expiry-test.cc
        tests/gc-test.cc
        tests/access-batching-test.cc
        tests/reopen-test.cc
        tests/schema-test.cc
        tests/degraded-test.cc
        tests/threaded-test.cc
    )

    # babel-serializer is linked for the schema test alone, which fabricates an incompatible file on purpose.
    # No library source exposes it, which is what keeps .shaped-lint.yml free of a sqlite entry.
    target_link_libraries(blob-cache-test PRIVATE blob-cache babel-serializer nexus)

    sc_add_nexus_web_runner(blob-cache-test)
endif()
