cmake_minimum_required(VERSION 3.28)
project(shaped-shader-library LANGUAGES CXX)

# shaped-shader-library (slib::) is the shader package + hot-reload mechanism.
# Any target declares its own shader package via sc_add_shader_package (cmake/ShaderPackage.cmake), gets
# typed C++ symbols for its shaders, and gets hot reloading.
# Shader sources are reached only through a mounted slib::filesystem -- real_filesystem is the one adaptor
# that touches the disk.
add_library(shaped-shader-library
    src/shaped-shader-library/filesystem/memory_filesystem.cc
    src/shaped-shader-library/filesystem/embedded_filesystem.cc
    src/shaped-shader-library/filesystem/real_filesystem.cc
    src/shaped-shader-library/filesystem/mount_table.cc
    src/shaped-shader-library/filesystem/impl/path.cc
    src/shaped-shader-library/filesystem/impl/watch_registry.cc
    src/shaped-shader-library/shader_asset.cc
    src/shaped-shader-library/shader_library.cc
    src/shaped-shader-library/impl/reload_watcher.cc
)

# The portable-HLSL prelude is authored as HLSL (shaders/sc/portable.hlsli) and baked into the library, so
# `#include "sc/portable.hlsli"` resolves with no per-application mount and a shipped binary carries it.
# Generated at configure time rather than by a build rule: the file is one small header that changes rarely,
# and CMAKE_CONFIGURE_DEPENDS is what makes editing it re-run this.
set(_slib_prelude_source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sc/portable.hlsli")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_slib_prelude_source}")
file(READ "${_slib_prelude_source}" _slib_prelude_text)

# The delimiter matches the package generator's, and nothing in HLSL can close it accidentally.
set(_slib_prelude_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/portable_prelude.generated.cc")
file(WRITE "${_slib_prelude_generated}"
"// Generated from shaders/sc/portable.hlsli by shaped-shader-library's CMakeLists. Do not edit.\n"
"#include <shaped-shader-library/impl/portable_prelude.hh>\n"
"\n"
"namespace\n"
"{\n"
"constexpr char const* k_portable_prelude = R\"slibsrc(${_slib_prelude_text})slibsrc\";\n"
"\n"
"constexpr slib::embedded_file k_files[] = {\n"
"    {.path = \"portable.hlsli\", .text = k_portable_prelude},\n"
"};\n"
"} // namespace\n"
"\n"
"cc::span<slib::embedded_file const> slib::impl::portable_prelude_files() { return k_files; }\n"
)
target_sources(shaped-shader-library PRIVATE "${_slib_prelude_generated}")

# Public headers (co-located in src/, grouped by topic).
# impl/ headers are internal -- reachable via the src/ include dir below, but intentionally not part of
# the public FILE_SET.
target_sources(shaped-shader-library
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-shader-library/fwd.hh
    src/shaped-shader-library/all.hh
    src/shaped-shader-library/filesystem/filesystem.hh
    src/shaped-shader-library/filesystem/watch.hh
    src/shaped-shader-library/filesystem/memory_filesystem.hh
    src/shaped-shader-library/filesystem/embedded_filesystem.hh
    src/shaped-shader-library/filesystem/real_filesystem.hh
    src/shaped-shader-library/filesystem/mount_table.hh
    src/shaped-shader-library/compiler/shader_compiler.hh
    src/shaped-shader-library/shader_package.hh
    src/shaped-shader-library/shader_asset.hh
    src/shaped-shader-library/shader_library.hh
)

# The OS file-watch backend behind real_filesystem::watch.
# real_filesystem and these are the only files in slib that reach the OS.
# A platform without one gets watch_backend_none: watch() reports that it cannot notify, and its caller polls.
# No compile definition goes with it -- nothing is gated on the answer, so nothing needs to agree on it.
if(WIN32)
    target_sources(shaped-shader-library PRIVATE src/shaped-shader-library/filesystem/impl/watch_backend_windows.cc)
else()
    target_sources(shaped-shader-library PRIVATE src/shaped-shader-library/filesystem/impl/watch_backend_none.cc)
endif()

target_compile_features(shaped-shader-library PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-shader-library/...>' works for consumers.
target_include_directories(shaped-shader-library
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# shaped-graphics provides sg::compiled_shader / sg::shader_stage / sg::context (public API surface).
target_link_libraries(shaped-shader-library PUBLIC shaped-graphics)

sc_target_pch(shaped-shader-library SG)

target_compile_options(shaped-shader-library
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# The HLSL -> DXIL compiler is optional: DXC is Windows-only and fetched on demand, so slib's own surface
# -- packages, mounts, reload -- builds and is tested everywhere without it.
# SLIB_HAS_DXC is PUBLIC because dxc_compiler.hh gates its declaration on it, so a consumer must see the
# same answer.
# It changes no type's layout, only whether one factory exists.
if(SC_HAS_DXC_COMPILER)
    target_sources(shaped-shader-library PRIVATE src/shaped-shader-library/compiler/dxc_compiler.cc)
    target_sources(shaped-shader-library
        PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
        FILES src/shaped-shader-library/compiler/dxc_compiler.hh
    )
    target_link_libraries(shaped-shader-library PRIVATE shaped-shader-compiler-dxc)
    target_compile_definitions(shaped-shader-library PUBLIC SLIB_HAS_DXC=1)
else()
    target_compile_definitions(shaped-shader-library PUBLIC SLIB_HAS_DXC=0)
endif()

# Test binary. Named *-test so the dev.py runner discovers it.
if(SC_BUILD_TESTS)
    add_executable(shaped-shader-library-test
        tests/main.cc
        tests/filesystem/path-test.cc
        tests/filesystem/filesystem-test.cc
        tests/filesystem/real_filesystem-test.cc
        tests/filesystem/mount_table-test.cc
        tests/filesystem/watch-test.cc
        tests/shader_library-test.cc
        tests/compile_source-test.cc
        tests/hot_reload-test.cc
        tests/shader_package-test.cc
        tests/dxc_compiler-test.cc
    )

    target_link_libraries(shaped-shader-library-test PRIVATE shaped-shader-library nexus)

    sc_target_pch(shaped-shader-library-test SG NEXUS)

    # slib declares a package of its own: the codegen has to be exercised by something, and the library that
    # ships sc_add_shader_package is the honest place for it.
    sc_add_shader_package(
        TARGET     shaped-shader-library-test
        NAME       slib_test_shaders
        NAMESPACE  slib_test::shaders
        SOURCE_DIR tests/shaders
        LANGUAGE   hlsl
        SHADERS
            invert.hlsl:compute:main
            blit.hlsl:vertex:main_vs
            blit.hlsl:fragment:main_ps
    )

    # Browser test runner (Emscripten only); a no-op on native toolchains.
    sc_add_nexus_web_runner(shaped-shader-library-test)
endif()
