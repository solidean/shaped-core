cmake_minimum_required(VERSION 3.28)
project(shaped-graphics LANGUAGES CXX)

# shaped-graphics (sg::) is the backend-agnostic graphics-API core: context / command_list /
# GPU resource types plus the pure-virtual backend bridge. Concrete backends live under
# backends/ as their own static libraries and are added conditionally below.
add_library(shaped-graphics
    src/shaped-graphics/buffer.cc
    src/shaped-graphics/bytes_future.cc
    src/shaped-graphics/command_list.cc
    src/shaped-graphics/command_list.upload.cc
    src/shaped-graphics/command_list.download.cc
    src/shaped-graphics/command_list.copy.cc
    src/shaped-graphics/command_list.compute.cc
    src/shaped-graphics/command_list_slot.cc
    src/shaped-graphics/binding_layout.cc
    src/shaped-graphics/compute_pipeline.cc
    src/shaped-graphics/binding_group.cc
    src/shaped-graphics/context.cc
    src/shaped-graphics/context.persistent.cc
    src/shaped-graphics/context.transient.cc
    src/shaped-graphics/context.upload.cc
    src/shaped-graphics/memory_heap.cc
)

# Public headers live co-located in src/. FILE_SET models the public API explicitly.
target_sources(shaped-graphics
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-graphics/fwd.hh
    src/shaped-graphics/all.hh
    src/shaped-graphics/types.hh
    src/shaped-graphics/context.hh
    src/shaped-graphics/context.persistent.hh
    src/shaped-graphics/context.transient.hh
    src/shaped-graphics/context.upload.hh
    src/shaped-graphics/command_list.hh
    src/shaped-graphics/command_list.upload.hh
    src/shaped-graphics/command_list.download.hh
    src/shaped-graphics/command_list.copy.hh
    src/shaped-graphics/command_list.compute.hh
    src/shaped-graphics/command_list_slot.hh
    src/shaped-graphics/buffer.hh
    src/shaped-graphics/resource_access.hh
    src/shaped-graphics/backend/resource_access_state.hh
    src/shaped-graphics/backend/access_inference.hh
    src/shaped-graphics/backend/subresource.hh
    src/shaped-graphics/views.hh
    src/shaped-graphics/binding.hh
    src/shaped-graphics/binding_layout.hh
    src/shaped-graphics/compiled_shader.hh
    src/shaped-graphics/compute_pipeline.hh
    src/shaped-graphics/binding_group.hh
    src/shaped-graphics/bytes_future.hh
    src/shaped-graphics/allocation_info.hh
    src/shaped-graphics/memory_heap.hh
)

target_compile_features(shaped-graphics PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-graphics/...>' works for consumers.
target_include_directories(shaped-graphics
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# clean-core provides the vocabulary types + assertions; typed-geometry the math types sg's
# resource/viewport/geometry APIs will speak in.
target_link_libraries(shaped-graphics PUBLIC clean-core typed-geometry)

# Enable conforming preprocessor for MSVC
target_compile_options(shaped-graphics
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# Backends: each is its own static library that links this core. A backend is built only where it is
# available for the platform/build: dx12 needs Windows (its SDK ships with it), vulkan needs the
# Vulkan SDK on a non-web target. Enabled backends are collected so the test binary link-checks them.
set(_sg_backends "")

if(WIN32)
    add_subdirectory(backends/dx12)
    list(APPEND _sg_backends shaped-graphics-dx12)
endif()

if(NOT EMSCRIPTEN)
    # Optional: the vulkan backend links the loader, so it only builds when the SDK is present.
    find_package(Vulkan)
    if(Vulkan_FOUND)
        add_subdirectory(backends/vulkan)
        list(APPEND _sg_backends shaped-graphics-vulkan)
    else()
        message(STATUS "shaped-graphics: Vulkan SDK not found, skipping the vulkan backend")
    endif()
endif()

# Test executable. Tests are organized by topic under tests/<topic>/. Pure/abstract-surface tests (value
# types, no GPU) compile unconditionally; the backend-agnostic sg *API* tests are context_handle
# INVOCABLE_TESTs run against every available backend and are gated on a backend being present (below), since
# the nexus orphan check fails an enabled invocable that no driver invokes. Per-backend smoke tests and
# internal invariants live with their backend as their own *-test binary (see backends/dx12/tests).
add_executable(shaped-graphics-test
    tests/main.cc
    tests/buffer/views-test.cc
    tests/transfer/bytes_future-test.cc
    tests/binding/binding-test.cc
    tests/barrier/access_state-test.cc
    tests/barrier/subresource-test.cc
    tests/barrier/slot-test.cc
)

# Cross-backend API tests: run the sg API against each backend compiled into this binary. Each backend
# contributes an entry-point driver under tests/backends/; the alias setup (tests/backends/backends.cc) makes
# each invocable runnable by name across all backends. Only meaningful when at least one backend is present.
#
# TODO: introduce a CPU / validation backend (an always-available, in-process context — likely no shader
# support at all) so these backend-agnostic tests run everywhere, not just where a real GPU backend builds.
# It would make this `if(_sg_backends)` guard unnecessary (a backend is always present), let large parts of
# the suite run on headless CI without a GPU, and double as a reference oracle to validate real backends
# against. Future work.
if(_sg_backends)
    target_sources(shaped-graphics-test PRIVATE
        tests/backends/backends.cc
        tests/context/context-test.cc
        tests/buffer/buffer-test.cc
        tests/command_list/command_list-test.cc
        tests/transfer/transfer-test.cc
        tests/transfer/transfer-fuzz-test.cc
        tests/transfer/upload-async-test.cc
        tests/copy/copy-test.cc
        tests/transient/transient-test.cc
        tests/barrier/barrier-test.cc
    )
    if(WIN32)
        target_sources(shaped-graphics-test PRIVATE tests/backends/dx12-entry.cc)
    endif()
    if(Vulkan_FOUND)
        target_sources(shaped-graphics-test PRIVATE tests/backends/vulkan-entry.cc)
    endif()
endif()

target_link_libraries(shaped-graphics-test
    PRIVATE
    shaped-graphics
    ${_sg_backends}
    nexus
)

# Browser test runner (Emscripten only); a no-op on native toolchains.
sc_add_nexus_web_runner(shaped-graphics-test)
