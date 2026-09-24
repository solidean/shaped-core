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
    src/shaped-shader-library/binding/binding_groups.cc
    src/shaped-shader-library/binding/wgsl_declarations.cc
    src/shaped-shader-library/compiler/wgsl_compiler.cc
    src/shaped-shader-library/compiler/sgl_compiler.cc
    src/shaped-shader-library/binding/impl/hlsl_binding_types.cc
    src/shaped-shader-library/binding/impl/hlsl_sampler_state.cc
    src/shaped-shader-library/binding/impl/hlsl_value_types.cc
    src/shaped-shader-library/binding/impl/hlsl_tokens.cc
    src/shaped-shader-library/shader_asset.cc
    src/shaped-shader-library/pipeline.cc
    src/shaped-shader-library/shader_library.cc
    src/shaped-shader-library/impl/reload_watcher.cc
)

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
    src/shaped-shader-library/binding/binding_groups.hh
    src/shaped-shader-library/binding/wgsl_declarations.hh
    src/shaped-shader-library/compiler/wgsl_compiler.hh
    src/shaped-shader-library/compiler/sgl_compiler.hh
    src/shaped-shader-library/compiler/shader_compiler.hh
    src/shaped-shader-library/shader_package.hh
    src/shaped-shader-library/shader_asset.hh
    src/shaped-shader-library/shader_library.hh
    src/shaped-shader-library/pipeline.hh
    src/shaped-shader-library/impl/pipeline_field_support.hh
    src/shaped-shader-library/impl/pipeline_fields.hh
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

# shaped-graphics-language is what the SGL compiler edge runs, and no header of slib names it.
# It is over clean-core alone, so it costs a consumer no graphics API and no toolchain.
target_link_libraries(shaped-shader-library PRIVATE shaped-graphics-language)

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

# The MSL -> metallib compiler, on the same terms as the DXC arm above.
# Its target exists on Apple alone, and it needs no toolchain of its own to build — shaped-shader-compiler-msl
# resolves `xcrun metal` at run time and falls back to MSL source where there is none.
# This is also what carries an SGL package to metal: create_sgl_compiler(create_metal_compiler()).
if(TARGET shaped-shader-compiler-msl)
    target_sources(shaped-shader-library PRIVATE src/shaped-shader-library/compiler/metal_compiler.cc)
    target_sources(shaped-shader-library
        PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
        FILES src/shaped-shader-library/compiler/metal_compiler.hh
    )
    target_link_libraries(shaped-shader-library PRIVATE shaped-shader-compiler-msl)
    target_compile_definitions(shaped-shader-library PUBLIC SLIB_HAS_METAL=1)
else()
    target_compile_definitions(shaped-shader-library PUBLIC SLIB_HAS_METAL=0)
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
        tests/reflection_mismatch-test.cc
        tests/dxc_compiler-test.cc
        tests/wgsl_compiler-test.cc
        tests/sgl_compiler-test.cc
        tests/metal_compiler-test.cc
        tests/pipeline-test.cc
        tests/binding/binding_groups-test.cc
        tests/binding/wgsl_declarations-test.cc
    )
    sc_nexus_binary(shaped-shader-library-test KINDS tests)

    # The binding corpus is one data file both parsers are checked against -- this one, and the build-time
    # generator's once it exists. Baked in at configure time so the test needs no path into the source tree,
    # and CMAKE_CONFIGURE_DEPENDS is what makes editing it re-run this.
    # The delimiter is chosen so nothing in HLSL can close the raw string accidentally.
    set(_slib_corpus_source "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/binding-corpus.txt")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_slib_corpus_source}")
    file(READ "${_slib_corpus_source}" _slib_corpus_text)

    # MSVC caps ONE string literal at 16380 bytes and truncates past it with C2026, and the corpus passed that
    # the moment it grew past a couple of dozen cases.
    # Adjacent literals are concatenated only AFTER that per-literal limit applies, so the corpus goes out as a
    # run of chunks -- the same answer sc_add_shader_package reached for an embedded shader.
    #
    # Chunks end at a NEWLINE rather than at a byte offset, which matters here in a way it does not there: the
    # corpus carries the pass's own error messages, em dashes and all, and a split inside a UTF-8 sequence would
    # leave half a character at the end of one literal and half at the start of the next.
    set(_slib_corpus_chunk_bytes 8000)
    string(LENGTH "${_slib_corpus_text}" _slib_corpus_length)
    set(_slib_corpus_literals "")
    set(_slib_corpus_at 0)
    while(_slib_corpus_at LESS _slib_corpus_length)
        math(EXPR _slib_corpus_remaining "${_slib_corpus_length} - ${_slib_corpus_at}")
        set(_slib_corpus_take ${_slib_corpus_chunk_bytes})
        if(NOT _slib_corpus_remaining GREATER _slib_corpus_take)
            set(_slib_corpus_take ${_slib_corpus_remaining})
        else()
            # Back up to the last newline in the candidate chunk, so no boundary lands mid-character.
            string(SUBSTRING "${_slib_corpus_text}" ${_slib_corpus_at} ${_slib_corpus_take} _slib_corpus_chunk)
            string(FIND "${_slib_corpus_chunk}" "\n" _slib_corpus_newline REVERSE)
            # GREATER -1, not 0: a chunk whose only newline is its very first byte still splits there.
            if(_slib_corpus_newline GREATER -1)
                math(EXPR _slib_corpus_take "${_slib_corpus_newline} + 1")
            endif()
        endif()

        string(SUBSTRING "${_slib_corpus_text}" ${_slib_corpus_at} ${_slib_corpus_take} _slib_corpus_chunk)
        string(APPEND _slib_corpus_literals "\n    R\"slibcorpus(${_slib_corpus_chunk})slibcorpus\"")
        math(EXPR _slib_corpus_at "${_slib_corpus_at} + ${_slib_corpus_take}")
    endwhile()

    set(_slib_corpus_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/binding_corpus.generated.cc")
    file(WRITE "${_slib_corpus_generated}"
"// Generated from tests/data/binding-corpus.txt by shaped-shader-library's CMakeLists. Do not edit.\n"
"#include <binding/binding_corpus.hh>\n"
"\n"
"namespace\n"
"{\n"
"constexpr char const* k_corpus =${_slib_corpus_literals};\n"
"} // namespace\n"
"\n"
"cc::string_view slib_test::binding_corpus_text() { return k_corpus; }\n"
)
    target_sources(shaped-shader-library-test PRIVATE "${_slib_corpus_generated}")
    target_include_directories(shaped-shader-library-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    # The pipeline test compares the committed impl/pipeline_fields.hh against what the SGL compiler generates now.
    target_compile_definitions(shaped-shader-library-test PRIVATE SLIB_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

    # The pipeline test walks the SGL prelude, which slib itself links privately.
    target_link_libraries(shaped-shader-library-test PRIVATE shaped-shader-library shaped-graphics-language nexus)

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
            shade.hlsl:compute:main
            mesh.hlsl:vertex:main_vs
            mesh.hlsl:fragment:main_ps
            frame_bindings.hlsli:binding:frame_bindings
            mesh.hlsl:vertex_input:vs_input
            mesh.hlsl:vertex_input:instance_input
            mesh.hlsl:payload:trace_payload
            shade.hlsl:constants:gConstants
    )

    # And one in SGL, which names its fragment stage `pixel` and generates no mirror yet.
    # One source for every format: the SGL compiler edge picks the text per target.
    sc_add_shader_package(
        TARGET     shaped-shader-library-test
        NAME       slib_test_sgl_shaders
        NAMESPACE  slib_test::sgl_shaders
        SOURCE_DIR tests/shaders-sgl
        LANGUAGE   sgl
        SHADERS
            cube.sgl:vertex:main_vs
            cube.sgl:pixel:main_ps
            double_values.sgl:compute:main
    )

    # Browser test runner (Emscripten only); a no-op on native toolchains.
    sc_add_nexus_web_runner(shaped-shader-library-test)
endif()
