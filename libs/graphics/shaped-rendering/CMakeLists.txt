cmake_minimum_required(VERSION 3.28)
project(shaped-rendering LANGUAGES CXX)

# shaped-rendering (sr::) builds render routines and helpers on top of shaped-graphics (mipmap
# generation, tonemapping, texture compression, ...). The first feature is the render-routine
# framework: a routine base with 3-phase init, plus a library/package split for loading + hot reload.
add_library(shaped-rendering
    src/shaped-rendering/shaped-rendering.cc
    src/shaped-rendering/render_routine.cc
    src/shaped-rendering/render_routine_package.cc
    src/shaped-rendering/render_routine_library.cc
)

target_sources(shaped-rendering
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-rendering/fwd.hh
    src/shaped-rendering/all.hh
    src/shaped-rendering/render_routine.hh
    src/shaped-rendering/render_routine_package.hh
    src/shaped-rendering/render_routine_library.hh
)

target_compile_features(shaped-rendering PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-rendering/...>' works for consumers.
target_include_directories(shaped-rendering
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# sr depends on slib: routines acquire shaders through it, and the routine library watches its
# hot-reload generation. slib is added after us in the root CMakeLists, which is fine —
# target_link_libraries resolves item names at generate time (shaped-graphics-test does the same).
target_link_libraries(shaped-rendering PUBLIC shaped-graphics shaped-shader-library)

target_compile_options(shaped-rendering
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# Test executable
if(SC_BUILD_TESTS)
    add_executable(shaped-rendering-test
        tests/main.cc
        tests/framework-test.cc
    )

    target_link_libraries(shaped-rendering-test
        PRIVATE
        shaped-rendering
        nexus
    )

    # Context-driven tests run on a dx12 WARP context and compile the shader through DXC — both
    # Windows-only, so they are gated on the dx12 backend target and a shader compiler existing. The
    # backend target is defined by shaped-graphics, added before us, so if(TARGET ...) sees it.
    if(SC_HAS_DXC_COMPILER AND TARGET shaped-graphics-dx12)
        target_sources(shaped-rendering-test PRIVATE tests/dx12-routine-test.cc)
        target_link_libraries(shaped-rendering-test PRIVATE shaped-graphics-dx12)
        sc_add_shader_package(
            TARGET     shaped-rendering-test
            NAME       sr_test_shaders
            NAMESPACE  sr::test::shaders
            SOURCE_DIR tests/shaders
            LANGUAGE   hlsl
            SHADERS
                pattern_fill.hlsl:compute:main
        )
    endif()

    # Browser test runner (Emscripten only); a no-op on native toolchains.
    sc_add_nexus_web_runner(shaped-rendering-test)
endif()
