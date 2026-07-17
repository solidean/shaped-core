cmake_minimum_required(VERSION 3.28)
project(shaped-viewer LANGUAGES CXX)

# shaped-viewer (sv::) is the professional, RTX-enabled visualization renderer with a
# dev-friendly API, built on shaped-rendering. Early-stage skeleton: a single anchor translation
# unit keeps the static target well-formed until the renderer lands.
add_library(shaped-viewer
    src/shaped-viewer/shaped-viewer.cc
)

target_sources(shaped-viewer
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-viewer/fwd.hh
    src/shaped-viewer/all.hh
)

target_compile_features(shaped-viewer PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-viewer/...>' works for consumers.
target_include_directories(shaped-viewer
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(shaped-viewer PUBLIC shaped-rendering)

target_compile_options(shaped-viewer
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# Test executable
if(SC_BUILD_TESTS)
    add_executable(shaped-viewer-test
        tests/main.cc
        tests/smoke-test.cc
    )

    target_link_libraries(shaped-viewer-test
        PRIVATE
        shaped-viewer
        nexus
    )

    # Browser test runner (Emscripten only); a no-op on native toolchains.
    sc_add_nexus_web_runner(shaped-viewer-test)
endif()
