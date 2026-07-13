#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/binding.hh>
#include <shaped-graphics/compiled_shader.hh>
#include <shaped-shader-compiler-dxc/impl/dxc_common.hh>

namespace ssc::dxc::impl
{
/// The backend-agnostic reflection sg needs from a compiled shader.
struct reflected_shader
{
    cc::vector<sg::binding> bindings;
    cc::optional<sg::compute_dimensions> workgroup_size; ///< populated for the compute stage
};

/// Extracts bindings (+ compute workgroup size) from a DXC compile result via DXC_OUT_REFLECTION.
/// The DXC (register, space, kind) is recorded faithfully as sg's (index, set, type) — no remapping;
/// each backend reinterprets it (see docs). Buffers, textures (SRV/UAV), samplers, and acceleration
/// structures map to the matching sg::binding_type; it fails on a kind sg has no vocabulary for yet
/// (texel/typed buffers, append/consume/counter buffers).
///
/// Ray-tracing stages (`is_raytracing_stage`) reflect a DXIL library: `entry_point` selects the function
/// whose bindings are extracted (matched by its mangled name). For non-RT stages `entry_point` is ignored.
[[nodiscard]] cc::result<reflected_shader> reflect(IDxcUtils* utils,
                                                   IDxcResult* result,
                                                   sg::shader_stage stage,
                                                   cc::string_view entry_point);
} // namespace ssc::dxc::impl
