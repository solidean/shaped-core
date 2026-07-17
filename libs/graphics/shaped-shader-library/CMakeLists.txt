cmake_minimum_required(VERSION 3.28)
project(shaped-shader-library LANGUAGES CXX)

# shaped-shader-library (slib::) is the shader package + hot-reload mechanism: any target declares its
# own shader package via sc_add_shader_package (cmake/ShaderPackage.cmake), gets typed C++ symbols for
# its shaders, and gets hot reloading. Shader sources are reached only through a mounted slib::filesystem
# -- real_filesystem is the one adaptor that touches the disk.
add_library(shaped-shader-library
    src/shaped-shader-library/filesystem/memory_filesystem.cc
    src/shaped-shader-library/filesystem/embedded_filesystem.cc
    src/shaped-shader-library/filesystem/real_filesystem.cc
    src/shaped-shader-library/filesystem/mount_table.cc
    src/shaped-shader-library/filesystem/impl/path.cc
)

# Public headers (co-located in src/, grouped by topic). impl/ headers are internal -- reachable via the
# src/ include dir below, but intentionally not part of the public FILE_SET.
target_sources(shaped-shader-library
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-shader-library/fwd.hh
    src/shaped-shader-library/all.hh
    src/shaped-shader-library/filesystem/filesystem.hh
    src/shaped-shader-library/filesystem/memory_filesystem.hh
    src/shaped-shader-library/filesystem/embedded_filesystem.hh
    src/shaped-shader-library/filesystem/real_filesystem.hh
    src/shaped-shader-library/filesystem/mount_table.hh
)

target_compile_features(shaped-shader-library PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-shader-library/...>' works for consumers.
target_include_directories(shaped-shader-library
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# shaped-graphics provides sg::compiled_shader / sg::shader_stage / sg::context (public API surface).
target_link_libraries(shaped-shader-library PUBLIC shaped-graphics)

target_compile_options(shaped-shader-library
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# Test binary. Named *-test so the dev.py runner discovers it.
if(SC_BUILD_TESTS)
    add_executable(shaped-shader-library-test
        tests/main.cc
        tests/filesystem/path-test.cc
        tests/filesystem/filesystem-test.cc
        tests/filesystem/real_filesystem-test.cc
        tests/filesystem/mount_table-test.cc
    )

    target_link_libraries(shaped-shader-library-test PRIVATE shaped-shader-library nexus)

    # Browser test runner (Emscripten only); a no-op on native toolchains.
    sc_add_nexus_web_runner(shaped-shader-library-test)
endif()
