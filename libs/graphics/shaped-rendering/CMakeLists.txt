cmake_minimum_required(VERSION 3.28)
project(shaped-rendering LANGUAGES CXX)

# shaped-rendering (sr::) builds concrete render routines and helpers on top of shaped-graphics (mipmap generation, tonemapping, texture compression, ...).
# The render-routine *framework* (the routine base and the per-context ctx.routines registry) lives in shaped-graphics;
# sr hosts the concrete routines, which land in their own .cc files as they are implemented.
# It is also home to the window abstraction.
add_library(shaped-rendering
    src/shaped-rendering/shaped-rendering.cc
    src/shaped-rendering/window.cc
    src/shaped-rendering/imgui_context.cc
    src/shaped-rendering/imgui_routine.cc
    src/shaped-rendering/shaders.cc
    src/shaped-rendering/impl/imgui_draw_math.cc
    src/shaped-rendering/impl/imgui_input_translation.cc
    src/shaped-rendering/impl/imgui_texture_registry.cc
)

target_sources(shaped-rendering
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-rendering/fwd.hh
    src/shaped-rendering/all.hh
    src/shaped-rendering/input.hh
    src/shaped-rendering/window.hh
    src/shaped-rendering/imgui_context.hh
    src/shaped-rendering/imgui_routine.hh
    src/shaped-rendering/shaders.hh
)

target_compile_features(shaped-rendering PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-rendering/...>' works for consumers.
target_include_directories(shaped-rendering
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# sr depends on slib because concrete routines acquire their shaders through it (a routine-author dependency).
# The routine framework in sg needs no slib — reload tracking is sg's own generation counter.
# slib is added after us in the root CMakeLists, which is fine — target_link_libraries resolves item names at generate time (shaped-graphics-test does the same).
#
# imgui is PUBLIC on three counts, all pointing the same way:
# an application writes ImGui::Begin(...) in its own TUs,
# sr's public headers name ImDrawData,
# and imgui's target carries PUBLIC compile definitions that change what imgui.h declares — a consumer that missed them would see a different ImGuiIO layout than imgui.cpp was built with.
target_link_libraries(shaped-rendering PUBLIC shaped-graphics shaped-shader-library imgui)

target_compile_options(shaped-rendering
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# The window API is always present.
# Only its backend is optional, because SDL3 is fetched on demand.
# Without one, window_system::try_create fails instead of the type disappearing — a caller writes the same code either way and finds out at run time, rather than gating on a macro.
if(SC_HAS_SDL3)
    target_sources(shaped-rendering
        PRIVATE
        src/shaped-rendering/impl/input_translation.cc
        src/shaped-rendering/impl/window_sdl.cc
    )

    # SDL links PRIVATE: an implementation detail of impl/, reaching no public header.
    target_link_libraries(shaped-rendering PRIVATE SDL3::SDL3)
    target_compile_definitions(shaped-rendering PUBLIC SR_HAS_WINDOW=1)
else()
    target_sources(shaped-rendering PRIVATE src/shaped-rendering/impl/window_null.cc)
    target_compile_definitions(shaped-rendering PUBLIC SR_HAS_WINDOW=0)
endif()

# sr's own shader package.
# Must be declared here, in the CMakeLists that defines the target: the generated sources are added as PRIVATE sources and add_custom_command(OUTPUT) is directory-scoped.
# Routines that need shaders add their entry points to this one package.
#
# Deliberately not gated on a compiler existing.
# Package generation needs none — it emits C++ descriptors and embeds the sources;
# compilation happens at runtime through slib.
# Without a compiler the routine simply acquires no shader and draws nothing, which is the same graceful path a broken shader edit takes.
sc_add_shader_package(
    TARGET     shaped-rendering
    NAME       sr_shaders
    NAMESPACE  sr::shaders
    SOURCE_DIR shaders
    LANGUAGE   hlsl
    SHADERS
        imgui.hlsl:vertex:main_vs
        imgui.hlsl:fragment:main_ps
)

# Test executable.
# The window/input tests and the imgui arithmetic (scissor clamping, ortho projection, texture-rect packing) need no device and run everywhere;
# the end-to-end imgui test needs a real one.
if(SC_BUILD_TESTS)
    # These compile in either build: they use only the public API, and guard on SR_HAS_WINDOW the few cases that need a live backend.
    add_executable(shaped-rendering-test
        tests/main.cc
        tests/smoke-test.cc
        tests/input-test.cc
        tests/window-test.cc
        tests/window-manual-test.cc
        tests/imgui-registry-test.cc
        tests/imgui-input-test.cc
    )

    target_link_libraries(shaped-rendering-test
        PRIVATE
        shaped-rendering
        nexus
    )

    if(SC_HAS_SDL3)
        # These two reach into impl/ and see SDL headers directly, so they exist only with a backend.
        target_sources(shaped-rendering-test
            PRIVATE
            tests/input-dispatch-test.cc
            tests/input-translation-test.cc
        )

        # The sanctioned place for SDL besides the backend itself — see docs/coding-guidelines.md.
        # sr links SDL PRIVATE, so the test needs its own link to get the include directories.
        target_link_libraries(shaped-rendering-test PRIVATE SDL3::SDL3)

        # Stage SDL3.dll next to the test binary so it starts.
        # SDL3::SDL3 is a real (not imported) shared target, so $<TARGET_RUNTIME_DLLS> reaches it through sr's PRIVATE link — no hand-listed DLL variable, unlike the DXC wiring.
        # The expression is empty off Windows.
        if(WIN32)
            add_custom_command(TARGET shaped-rendering-test POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:shaped-rendering-test> $<TARGET_FILE_DIR:shaped-rendering-test>
                COMMAND_EXPAND_LISTS
            )
        endif()
    endif()

    # The end-to-end imgui test drives a dx12 WARP context directly (create_dx12_context) and compiles the imgui shader through DXC, so it needs both the dx12 backend and a compiler.
    # The tests above need neither and run everywhere.
    if(WIN32 AND SC_HAS_DXC_COMPILER AND TARGET shaped-graphics-dx12)
        target_sources(shaped-rendering-test PRIVATE tests/imgui-routine-test.cc tests/imgui-manual-test.cc)
        target_link_libraries(shaped-rendering-test PRIVATE shaped-graphics-dx12)
    endif()

    # Browser test runner (Emscripten only); a no-op on native toolchains.
    sc_add_nexus_web_runner(shaped-rendering-test)
endif()
