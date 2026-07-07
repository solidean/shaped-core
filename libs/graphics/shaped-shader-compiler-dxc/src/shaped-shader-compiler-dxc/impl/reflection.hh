#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
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
/// each backend reinterprets it (see docs). Fails on a resource kind sg has no binding_type for yet
/// (textures/samplers/typed-UAVs/acceleration structures).
[[nodiscard]] cc::result<reflected_shader> reflect(IDxcUtils* utils, IDxcResult* result, sg::shader_stage stage);
} // namespace ssc::dxc::impl
