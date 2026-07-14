cmake_minimum_required(VERSION 3.28)
project(shaped-shader-compiler-dxc LANGUAGES CXX)

# shaped-shader-compiler-dxc (ssc::) wraps the DirectX Shader Compiler (DXC) to turn HLSL into an
# sg::compiled_shader (bytecode + reflected bindings + compute workgroup size). Windows-only: it links
# DXC (downloaded release binaries in extern/dxc/.install) and uses d3d12shader.h reflection.
# The enclosing add_subdirectory (root CMakeLists) gates this on WIN32 + the `dxc` target existing.
add_library(shaped-shader-compiler-dxc
    src/shaped-shader-compiler-dxc/compiler.cc
    src/shaped-shader-compiler-dxc/shader_cache.cc
    src/shaped-shader-compiler-dxc/impl/command_line_args.cc
    src/shaped-shader-compiler-dxc/impl/include_handler.cc
    src/shaped-shader-compiler-dxc/impl/reflection.cc
)

# Public headers (co-located in src/). impl/ headers are internal — reachable via the src/ include
# dir below, but intentionally not part of the public FILE_SET.
target_sources(shaped-shader-compiler-dxc
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-shader-compiler-dxc/fwd.hh
    src/shaped-shader-compiler-dxc/all.hh
    src/shaped-shader-compiler-dxc/compile_options.hh
    src/shaped-shader-compiler-dxc/shader_description.hh
    src/shaped-shader-compiler-dxc/preprocessed_source.hh
    src/shaped-shader-compiler-dxc/compiler.hh
    src/shaped-shader-compiler-dxc/shader_cache.hh
)

target_compile_features(shaped-shader-compiler-dxc PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-shader-compiler-dxc/...>' works for consumers.
target_include_directories(shaped-shader-compiler-dxc
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# shaped-graphics provides sg::compiled_shader (public API surface). DXC is an implementation detail
# kept behind the pimpl, so it links PRIVATE.
target_link_libraries(shaped-shader-compiler-dxc PUBLIC shaped-graphics PRIVATE dxc)

target_compile_options(shaped-shader-compiler-dxc
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# Test binary. Named *-test so the dev.py runner discovers it.
add_executable(shaped-shader-compiler-dxc-test
    tests/main.cc
    tests/preprocess-test.cc
    tests/compile-test.cc
    tests/reflect-raytracing-test.cc
    tests/shader_cache-test.cc
)

target_link_libraries(shaped-shader-compiler-dxc-test PRIVATE shaped-shader-compiler-dxc nexus)

# The dx12 backend is added before this lib (root CMakeLists order), so its target exists here and can be
# linked for a whole-chain integration test: compile HLSL -> reflect -> bind -> dispatch on WARP -> read back.
if(TARGET shaped-graphics-dx12)
    target_sources(shaped-shader-compiler-dxc-test PRIVATE
        tests/end-to-end-sampler-test.cc
        tests/end-to-end-multigroup-test.cc
        tests/end-to-end-inline-constants-test.cc
        tests/end-to-end-inline-raytracing-test.cc
        tests/end-to-end-raytracing-test.cc
        tests/end-to-end-raster-test.cc
        tests/end-to-end-raster-stages-test.cc
        tests/end-to-end-cube-window-test.cc
        tests/end-to-end-raytraced-cube-window-test.cc
    )
    target_link_libraries(shaped-shader-compiler-dxc-test PRIVATE shaped-graphics-dx12)
endif()

# Stage the DXC runtime DLLs (dxcompiler + its dxil.dll signer) next to the test binary so it loads
# them at run time. DXC_RUNTIME_DLLS is set by extern/dxc; dxil.dll isn't the imported target, so
# $<TARGET_RUNTIME_DLLS> alone would miss it.
add_custom_command(TARGET shaped-shader-compiler-dxc-test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${DXC_RUNTIME_DLLS} $<TARGET_FILE_DIR:shaped-shader-compiler-dxc-test>
    COMMAND_EXPAND_LISTS
)
