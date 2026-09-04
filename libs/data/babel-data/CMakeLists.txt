cmake_minimum_required(VERSION 3.28)
project(babel-data LANGUAGES CXX)

# babel is ONE NAMESPACE ACROSS TWO LIBRARIES, and the split is by DEPENDENCY rather than by topic.
#
# babel-data is the base: the formats that are pure parsing and need nothing but clean-core. A consumer that wants
# JSON should not link an image decoder, a database engine or typed-geometry to get it — and it is why `data/` spans
# both libraries, sqlite living next door because of what it needs, not because of what it is.
#
# babel-serializer stays THE target: everything else, and the one to link when in doubt. Spin off a third only when a
# format arrives with a dependency nobody should pay for by accident — a cursed SDK, a heavyweight decoder — and give
# it the same treatment.
#
# The namespace never splits with the libraries. `babel::json` is `babel::json` wherever it is linked from, and
# fwd.hh LAYERS rather than forks: this one declares `namespace babel` itself plus the formats here, and
# babel-serializer's includes it and adds the rest.
add_library(babel-data
    src/babel-data/fwd.cc

    src/babel-data/data/base64.cc
    src/babel-data/data/json.cc
    src/babel-data/data/json_writer.cc
    src/babel-data/data/markdown.cc
)

# Public headers live co-located in src/ for better editor experience.
# FILE_SET models the public API explicitly (install/export, IDEs, etc.).
target_sources(babel-data
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/babel-data/all.hh
    src/babel-data/fwd.hh

    src/babel-data/data/base64.hh
    src/babel-data/data/json.hh
    src/babel-data/data/markdown.hh
)

# Require at least C++23 for this target; the final -std is chosen by the top-level project.
target_compile_features(babel-data PUBLIC cxx_std_23)

# Expose "src" so '#include <babel-data/...>' works for consumers.
target_include_directories(babel-data PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

# clean-core and nothing else, and that is the library's whole contract.
# A format needing more does not go here.
target_link_libraries(babel-data PUBLIC clean-core)

# Enable conforming preprocessor for MSVC
target_compile_options(babel-data PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)

sc_target_pch(babel-data CC_ALL) # the same measured tier as babel-serializer

# Test executable
if(SC_BUILD_TESTS)
    add_executable(babel-data-test
        tests/main.cc

        tests/babel-data-recording-test.cc

        tests/data/base64-test.cc
        tests/data/json-test.cc
        tests/data/markdown-test.cc
    )

    target_link_libraries(babel-data-test
        PRIVATE
        babel-data
        nexus
    )

    sc_target_pch(babel-data-test CC_ALL NEXUS) # inherits the library's measured tier

    # Browser test runner (Emscripten only); no-op natively.
    sc_add_nexus_web_runner(babel-data-test)
endif()

# Examples: how the API reads in practice. See docs/guides/examples.md.
add_subdirectory(examples)
