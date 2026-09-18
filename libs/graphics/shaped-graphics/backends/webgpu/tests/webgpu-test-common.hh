#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh>
#include <shaped-graphics/binding/compiled_shader.hh>

// Shared setup for the WebGPU tier-2 suite (shaped-graphics-webgpu-test).
//
// Every test is an ASYNC_INVOCABLE_TEST taking the one context the entry driver (webgpu-entry.cc) requested for the whole run.
// Shaders are WGSL written inline with hand-authored reflection, since what this suite checks is the backend and not slib's parser.

namespace sg::backend::webgpu::test
{
/// A compiled shader over WGSL `source`, the way slib's WGSL compiler hands one over: the source text is the bytecode.
[[nodiscard]] inline sg::compiled_shader make_shader(sg::shader_stage stage,
                                                     cc::string_view source,
                                                     cc::string_view entry_point,
                                                     cc::vector<sg::binding> bindings = {},
                                                     cc::optional<sg::compute_dimensions> workgroup_size = {})
{
    auto shader = sg::compiled_shader();
    shader.stage = stage;
    shader.format = sg::shader_format::wgsl;
    shader.entry_point = cc::string(entry_point);
    shader.bytecode
        = cc::make_pinned_data(cc::span<byte const>(reinterpret_cast<byte const*>(source.data()), source.size()));
    shader.bindings = cc::move(bindings);
    shader.workgroup_size = workgroup_size;
    sg::apply_stage_visibility(shader.bindings, stage);
    return shader;
}
} // namespace sg::backend::webgpu::test
