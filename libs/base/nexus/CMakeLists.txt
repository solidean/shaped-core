cmake_minimum_required(VERSION 3.28)
project(nexus LANGUAGES CXX)

# Define the library and its sources
add_library(nexus
    src/nexus/run.cc
    src/nexus/test.cc
    src/nexus/tests/check.cc
    src/nexus/tests/config.cc
    src/nexus/tests/execute.cc
    src/nexus/tests/registry.cc
    src/nexus/tests/schedule.cc
    src/nexus/tests/export/xml.cc
    src/nexus/tests/export/catch2.cc
    src/nexus/tests/export/junit.cc
    src/nexus/fuzz/machine.cc
    src/nexus/fuzz/runner.cc
    src/nexus/fuzz/run.cc
    src/nexus/fuzz/test.cc
)

# Public headers live co-located in src/ for better editor experience.
# FILE_SET models the public API explicitly (install/export, IDEs, etc.).
target_sources(nexus
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/nexus/fwd.hh
    src/nexus/run.hh
    src/nexus/test.hh
    src/nexus/tests/check.hh
    src/nexus/tests/config.hh
    src/nexus/tests/execute.hh
    src/nexus/tests/registry.hh
    src/nexus/tests/schedule.hh
    src/nexus/tests/export/xml.hh
    src/nexus/tests/export/catch2.hh
    src/nexus/tests/export/junit.hh

    src/nexus/fuzz/fwd.hh
    src/nexus/fuzz/value.hh
    src/nexus/fuzz/signature.hh
    src/nexus/fuzz/operation.hh
    src/nexus/fuzz/executed_operation.hh
    src/nexus/fuzz/machine.hh
    src/nexus/fuzz/runner.hh
    src/nexus/fuzz/run.hh
    src/nexus/fuzz/regression_dialect.hh
    src/nexus/fuzz/replay_random.hh
    src/nexus/fuzz/test.hh
    src/nexus/fuzz/fuzz.hh
)

# Libraries should not set a global C++ standard here.
# Instead, declare a minimum on the target so parents stay in control.
# Require at least C++23 for this target; the final -std is chosen by the top-level project.
target_compile_features(nexus
    PUBLIC
    cxx_std_23
)

# Expose "src" so '#include <nexus/dummy.hh>' works for consumers.
target_include_directories(nexus
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(nexus
    PUBLIC
    clean-core
)

# Test executable
add_executable(nexus-test
    tests/main.cc
    tests/test-api-test.cc
    tests/test-check-test.cc
    tests/test-export-test.cc
    tests/test-registry-test.cc
    tests/test-section-test.cc
    tests/fuzz-value-test.cc
    tests/fuzz-engine-test.cc
    tests/fuzz-minimize-test.cc
    tests/fuzz-regression-test.cc
)

target_link_libraries(nexus-test
    PRIVATE
    nexus
)
