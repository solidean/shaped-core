#pragma once

#include "mesh.metallib.h"

#include <clean-core/common/utility.hh>
#include <shaped-graphics/binding/compiled_shader.hh>

// The `mesh.metal` library, as the `sg::compiled_shader` its entry points are reached through.
//
// Shared rather than copied: three test files draw on the same blob, and a second hand-rolled copy of this wrapper is
// how two of them would drift apart on what `shader.format` or `workgroup_size` has to say.
// The blob itself is large, so this header is included by the files that use it rather than by `metal-test-common.hh`.

namespace sg::backend::metal::test
{
/// One entry point of `mesh.metal`, with the whole library as its bytecode.
///
/// A metallib holds every entry point it was compiled from, so which one runs is `entry_point`'s alone — the same blob
/// backs a vertex stage, a fragment stage and a kernel.
[[nodiscard]] inline sg::compiled_shader mesh_shader(sg::shader_stage stage, cc::string entry)
{
    auto shader = sg::compiled_shader{};
    shader.stage = stage;
    shader.format = sg::shader_format::metal_lib;
    shader.entry_point = cc::move(entry);

    auto blob = cc::pinned_data<byte>::create_uninitialized(isize(sizeof(mesh_metallib)));
    cc::memcpy(blob.data(), mesh_metallib, sizeof(mesh_metallib));
    shader.bytecode = cc::pinned_data<byte const>(cc::move(blob));
    return shader;
}

/// A compute entry point of `mesh.metal`, at the one-thread-per-group size every kernel in it is written for.
[[nodiscard]] inline sg::compiled_shader mesh_kernel(cc::string entry)
{
    auto shader = mesh_shader(sg::shader_stage::compute, cc::move(entry));
    shader.workgroup_size = sg::compute_dimensions{.x = 1, .y = 1, .z = 1};
    return shader;
}
} // namespace sg::backend::metal::test
