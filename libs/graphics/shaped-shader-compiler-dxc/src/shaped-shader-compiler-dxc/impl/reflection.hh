#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-shader-compiler-dxc/impl/dxc_common.hh>

namespace ssc::dxc::impl
{
/// The backend-agnostic reflection sg needs from a compiled shader.
struct reflected_shader
{
    cc::vector<sg::binding> bindings;
    /// NVAPI's shader extension slot, where the shader declares it at `u0, space13` (`sg::reserved_slot`).
    cc::vector<sg::reserved_slot> reserved_slots;
    cc::optional<sg::compute_dimensions> workgroup_size; ///< populated for the compute stage
    /// Populated for the fragment stage: one past the highest render target it writes, since targets may be sparse.
    cc::optional<i32> color_output_count;
};

/// Extracts bindings (+ compute workgroup size) from a DXC compile result via DXC_OUT_REFLECTION.
///
/// **Windows only.** DXC emits no DXC_OUT_REFLECTION for a `-spirv` compile, and the Linux release ships neither
/// d3d12shader.h nor an implementation of the interfaces it declares — so this is not merely unavailable there, the
/// container it reads does not exist.
/// See reflect_spirv for the other arm.
/// The DXC (register, space, kind) is recorded faithfully as sg's (index, set, type) — no remapping;
/// each backend reinterprets it (see docs). Buffers, textures (SRV/UAV), samplers, and acceleration
/// structures map to the matching sg::binding_type; it fails on a kind sg has no vocabulary for yet
/// (texel/typed buffers, append/consume/counter buffers).
/// The one counter buffer it takes is NVAPI's shader extension slot, `g_NvidiaExt` at `u0, space13`, which it reports as
/// a reserved slot rather than a binding; that name anywhere else is refused as any counter buffer is.
///
/// Ray-tracing stages (`is_raytracing_stage`) reflect a DXIL library: `entry_point` selects the function
/// whose bindings are extracted (matched by its mangled name). For non-RT stages `entry_point` is ignored.
#ifdef CC_OS_WINDOWS
[[nodiscard]] cc::result<reflected_shader> reflect(IDxcUtils* utils,
                                                   IDxcResult* result,
                                                   sg::shader_stage stage,
                                                   cc::string_view entry_point);
#endif

/// The same reflection, read out of an emitted SPIR-V module rather than out of a container beside it.
///
/// A SPIR-V binding reports `group_index` (the descriptor set) and leaves `space` absent, which is the opposite of
/// the DXIL arm above and is what a vulkan group layout needs — see shaped-graphics' binding.hh on why the two are
/// not interchangeable.
[[nodiscard]] cc::result<reflected_shader> reflect_spirv(cc::span<byte const> spirv,
                                                         sg::shader_stage stage,
                                                         cc::string_view entry_point);
} // namespace ssc::dxc::impl
