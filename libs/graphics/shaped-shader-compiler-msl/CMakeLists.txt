cmake_minimum_required(VERSION 3.28)
project(shaped-shader-compiler-msl LANGUAGES CXX)

# shaped-shader-compiler-msl (ssc::msl) turns MSL into an sg::compiled_shader: a metallib or the source itself,
# plus the bindings reflected out of the text.
#
# Unlike shaped-shader-compiler-dxc this links no compiler and so has nothing to gate on at configure time.
# `xcrun metal` is resolved at RUN time, and the MSL-source arm needs no toolchain at all — so a machine without
# Apple's separately-installed Metal toolchain still gets a working library, which is the whole point of having
# two arms. The enclosing add_subdirectory (root CMakeLists) gates this on APPLE alone.
add_library(shaped-shader-compiler-msl
    src/shaped-shader-compiler-msl/compiler.cc
    src/shaped-shader-compiler-msl/impl/metal_driver.cc
    src/shaped-shader-compiler-msl/impl/msl_reflection.cc
    src/shaped-shader-compiler-msl/shader_cache.cc
)

# Public headers (co-located in src/). impl/ headers are internal — reachable via the src/ include
# dir below, but intentionally not part of the public FILE_SET.
target_sources(shaped-shader-compiler-msl
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-shader-compiler-msl/fwd.hh
    src/shaped-shader-compiler-msl/compile_options.hh
    src/shaped-shader-compiler-msl/shader_description.hh
    src/shaped-shader-compiler-msl/compiler.hh
    src/shaped-shader-compiler-msl/shader_cache.hh
    src/shaped-shader-compiler-msl/all.hh
)

target_compile_features(shaped-shader-compiler-msl PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-shader-compiler-msl/...>' works for consumers.
target_include_directories(shaped-shader-compiler-msl
    PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)

# shaped-graphics provides sg::compiled_shader, which is this library's whole output type.
target_link_libraries(shaped-shader-compiler-msl PUBLIC shaped-graphics)

# What the persistent shader cache keys on beside the toolchain: a stored shader holds this library's reflection, so an
# edit to it has to miss, and sg's binding headers too, since the stored bytes hold their enums by number.
file(GLOB_RECURSE _ssc_msl_fingerprinted CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hh"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shaped-graphics/src/shaped-graphics/binding/*.hh"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shaped-graphics/src/shaped-graphics/binding/impl/*.hh")
sc_add_source_fingerprint(shaped-shader-compiler-msl
    FUNCTION ssc::msl::impl::source_fingerprint
    HEADER shaped-shader-compiler-msl/shader_cache.hh
    FILES ${_ssc_msl_fingerprinted})

# Test binary. Named *-test so the dev.py runner discovers it.
if(SC_BUILD_TESTS)
    add_executable(shaped-shader-compiler-msl-test
        tests/main.cc
        tests/metal_driver-test.cc
        tests/reflect-test.cc
        tests/compile-test.cc
        tests/shader_cache-test.cc
    )
    sc_nexus_binary(shaped-shader-compiler-msl-test KINDS tests)

    target_link_libraries(shaped-shader-compiler-msl-test PRIVATE shaped-shader-compiler-msl nexus)

    # The metal backend is added before this library (root CMakeLists order), so its target exists here and the whole
    # chain can be tested: MSL text -> reflect -> layout -> dispatch -> read back.
    # That is the pairing nothing covered before — the backend's own fixtures carry reflection written by hand.
    if(TARGET shaped-graphics-metal)
        target_sources(shaped-shader-compiler-msl-test PRIVATE tests/end-to-end-compute-test.cc)
        target_link_libraries(shaped-shader-compiler-msl-test PRIVATE shaped-graphics-metal)
        target_compile_definitions(shaped-shader-compiler-msl-test PRIVATE SSC_MSL_TEST_HAS_METAL=1)
    endif()
endif()
