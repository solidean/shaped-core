cmake_minimum_required(VERSION 3.28)
project(shader-analysis LANGUAGES CXX)

# shader-analysis (shan): what a shader binary costs, read off the binary alone.
# babel reads the formats as they are stored, and this library says what they mean: operation kinds, register pressure,
# critical paths, and the difference between two modules of one entry point. See readme.md.
add_library(shader-analysis
    src/shader-analysis/fwd.cc
    src/shader-analysis/op_kind.cc
    src/shader-analysis/cost.cc
    src/shader-analysis/spirv_metrics.cc
    src/shader-analysis/dxil_metrics.cc
    src/shader-analysis/impl/graph.cc
    src/shader-analysis/diff.cc
)

# Public headers live co-located in src/ for better editor experience.
# FILE_SET models the public API explicitly (install/export, IDEs, etc.).
target_sources(shader-analysis
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shader-analysis/fwd.hh
    src/shader-analysis/op_kind.hh
    src/shader-analysis/cost.hh
    src/shader-analysis/metrics.hh
    src/shader-analysis/diff.hh
)

# Require at least C++23 for this target; the final -std is chosen by the top-level project.
target_compile_features(shader-analysis PUBLIC cxx_std_23)

# Expose "src" so '#include <shader-analysis/...>' works for consumers.
target_include_directories(shader-analysis PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

# babel-data reads the binaries, and a public header names its module type, so it is linked PUBLIC.
target_link_libraries(shader-analysis PUBLIC clean-core babel-data)

# Enable conforming preprocessor for MSVC
target_compile_options(shader-analysis PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)

if(SC_BUILD_TESTS)
    add_executable(shader-analysis-test
        tests/main.cc
        tests/recording-test.cc
        tests/spirv-builder.hh
        tests/op-kind-test.cc
        tests/spirv-metrics-test.cc
        tests/diff-test.cc
        tests/dxil-metrics-test.cc
        tests/uniformity-test.cc
        tests/cost-test.cc
    )
    sc_nexus_binary(shader-analysis-test KINDS tests)
    target_link_libraries(shader-analysis-test PRIVATE shader-analysis nexus)
    # DXC's output babel-data's tests read, which these measure
    sc_target_source_paths(shader-analysis-test PRIVATE SHAN_DXIL_DATA_DIR ../../data/babel-data/tests/shader/data)
    sc_add_nexus_web_runner(shader-analysis-test)
endif()

# `shan`, the command line: `shan analyze <module>` and `shan diff <a> <b>`.
if(SC_BUILD_TOOLS)
    add_executable(shan
        tools/shan/main.cc
        tools/shan/commands.cc
    )
    sc_nexus_binary(shan KINDS tool)
    target_link_libraries(shan PRIVATE shader-analysis nexus)
endif()
