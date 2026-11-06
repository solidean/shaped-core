cmake_minimum_required(VERSION 3.28)
project(shaped-xr LANGUAGES CXX)

# shaped-xr (sxr::) puts sg rendering in a headset through OpenXR: the runtime, its session and frame loop, and the
# images it composes. The API exists in every build; without the loader `system::try_create` fails instead.
add_library(shaped-xr
    src/shaped-xr/system.cc
    src/shaped-xr/session.cc
    src/shaped-xr/runtime_report.cc
    src/shaped-xr/impl/view_math.cc
    src/shaped-xr/impl/capabilities.cc
    src/shaped-xr/impl/actions.cc
)
target_sources(shaped-xr
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-xr/fwd.hh
    src/shaped-xr/xr.hh
    src/shaped-xr/runtime_report.hh
    src/shaped-xr/impl/view_math.hh
    src/shaped-xr/impl/capabilities.hh
    src/shaped-xr/impl/actions.hh
)
target_sources(shaped-xr PRIVATE src/shaped-xr/impl/openxr.hh)
target_include_directories(shaped-xr PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(shaped-xr PUBLIC shaped-rendering)

# the loader and each binding, where this build has them; SXR_HAS_OPENXR says whether a system can be created at all
if(TARGET openxr_loader)
    target_link_libraries(shaped-xr PRIVATE openxr_loader)
    target_compile_definitions(shaped-xr PUBLIC SXR_HAS_OPENXR=1)
else()
    target_compile_definitions(shaped-xr PUBLIC SXR_HAS_OPENXR=0)
endif()
if(TARGET shaped-graphics-vulkan)
    target_link_libraries(shaped-xr PRIVATE shaped-graphics-vulkan)
    target_compile_definitions(shaped-xr PRIVATE SXR_HAS_VULKAN=1)
else()
    target_compile_definitions(shaped-xr PRIVATE SXR_HAS_VULKAN=0)
endif()
if(TARGET shaped-graphics-dx12)
    target_link_libraries(shaped-xr PRIVATE shaped-graphics-dx12)
    target_compile_definitions(shaped-xr PRIVATE SXR_HAS_DX12=1)
else()
    target_compile_definitions(shaped-xr PRIVATE SXR_HAS_DX12=0)
endif()

# what a runtime offers, which needs the loader but no graphics backend
if(TARGET openxr_loader)
    sc_add_example(shaped-xr-probe-example
        SOURCES
        examples/xr_probe.cc
        LINK
        shaped-xr
    )
endif()

if(SC_BUILD_TESTS)
    # the conversions at OpenXR's edge, which need no runtime, and sessions, which skip without Monado (docs/testing.md)
    add_executable(shaped-xr-test
        tests/main.cc
        tests/view-math-test.cc
        tests/actions-test.cc
        tests/capabilities-test.cc
        tests/session-test.cc
    )
    target_link_libraries(shaped-xr-test PRIVATE shaped-xr nexus)
    sc_stage_runtime_dlls(shaped-xr-test DLLS "$<TARGET_RUNTIME_DLLS:shaped-xr-test>")
    sc_nexus_binary(shaped-xr-test KINDS tests)
endif()
