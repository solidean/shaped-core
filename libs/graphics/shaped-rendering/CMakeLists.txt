cmake_minimum_required(VERSION 3.28)
project(shaped-rendering LANGUAGES CXX)

# shaped-rendering (sr::) builds concrete render routines and helpers on top of shaped-graphics (mipmap
# generation, tonemapping, texture compression, ...). The render-routine *framework* (the routine base
# and the per-context ctx.routines registry) lives in shaped-graphics; sr hosts the concrete routines,
# which land in their own .cc files as they are implemented.
add_library(shaped-rendering
    src/shaped-rendering/shaped-rendering.cc
)

target_sources(shaped-rendering
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-rendering/fwd.hh
    src/shaped-rendering/all.hh
)

target_compile_features(shaped-rendering PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-rendering/...>' works for consumers.
target_include_directories(shaped-rendering
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# sr depends on slib because concrete routines acquire their shaders through it (a routine-author
# dependency). The routine framework in sg needs no slib — reload tracking is sg's own generation
# counter. slib is added after us in the root CMakeLists, which is fine — target_link_libraries
# resolves item names at generate time (shaped-graphics-test does the same).
target_link_libraries(shaped-rendering PUBLIC shaped-graphics shaped-shader-library)

target_compile_options(shaped-rendering
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# The window abstraction is optional: SDL3 is fetched on demand, so sr's routines build and are tested
# everywhere without it. SR_HAS_WINDOW is PUBLIC because window.hh (and fwd.hh/all.hh) gate on it, so a
# consumer must see the same answer. It changes no type's layout, only whether the window API exists.
# SDL links PRIVATE: it is an implementation detail of impl/window_sdl.cc and reaches no public header.
if(SC_HAS_SDL3)
    target_sources(shaped-rendering
        PRIVATE
        src/shaped-rendering/impl/input_translation.cc
        src/shaped-rendering/impl/window_sdl.cc
    )

    target_sources(shaped-rendering
        PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
        FILES
        src/shaped-rendering/input.hh
        src/shaped-rendering/window.hh
    )

    target_link_libraries(shaped-rendering PRIVATE SDL3::SDL3)
    target_compile_definitions(shaped-rendering PUBLIC SR_HAS_WINDOW=1)
else()
    target_compile_definitions(shaped-rendering PUBLIC SR_HAS_WINDOW=0)
endif()

# Test executable. sr hosts no concrete routines yet (the render-routine *framework* is tested in
# shaped-graphics, tests/routine/, where it lives), so this is just a smoke test that the library
# builds and links. Real tests land alongside the first concrete routines.
if(SC_BUILD_TESTS)
    add_executable(shaped-rendering-test
        tests/main.cc
        tests/smoke-test.cc
    )

    target_link_libraries(shaped-rendering-test
        PRIVATE
        shaped-rendering
        nexus
    )

    if(SC_HAS_SDL3)
        target_sources(shaped-rendering-test
            PRIVATE
            tests/input-dispatch-test.cc
            tests/input-test.cc
            tests/input-translation-test.cc
            tests/window-test.cc
            tests/window-manual-test.cc
        )

        # input-translation-test.cc reaches into impl/ and so sees SDL headers directly. The one sanctioned place
        # besides the backend itself — see docs/coding-guidelines.md. sr links SDL PRIVATE, so the test needs its
        # own link to get the include directories.
        target_link_libraries(shaped-rendering-test PRIVATE SDL3::SDL3)

        # Stage SDL3.dll next to the test binary so it starts. SDL3::SDL3 is a real (not imported) shared
        # target, so $<TARGET_RUNTIME_DLLS> reaches it through sr's PRIVATE link and no hand-listed DLL
        # variable is needed, unlike the DXC wiring. The expression is empty off Windows.
        if(WIN32)
            add_custom_command(TARGET shaped-rendering-test POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:shaped-rendering-test> $<TARGET_FILE_DIR:shaped-rendering-test>
                COMMAND_EXPAND_LISTS
            )
        endif()
    endif()

    # Browser test runner (Emscripten only); a no-op on native toolchains.
    sc_add_nexus_web_runner(shaped-rendering-test)
endif()
