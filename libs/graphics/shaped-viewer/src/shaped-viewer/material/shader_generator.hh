#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/binding/sampler.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/material/material_attribute.hh>
#include <shaped-viewer/material/resolve.hh>

/// What one slot of a per-instance parameter block holds.
enum class sv::material_slot_kind : sv::u8
{
    constant,             ///< the attribute's value itself, in the layout `attribute_format` names
    attribute_descriptor, ///< an `sv::attribute_desc` — where a mesh attribute's elements live
    texture_index,        ///< a `u32` index into the bindless texture table
};

/// One field of the per-instance parameter block a permutation reads.
///
/// The generated shader loads it at `offset` and the CPU fills it at `offset`, and both come from the same layout — which is the
/// point of handing the layout back alongside the source rather than letting each side compute it.
struct sv::material_slot
{
    /// which attribute this serves; a texture's uv descriptor is named `"<attribute>.uv"`
    cc::string name;

    material_slot_kind kind = material_slot_kind::constant;

    /// byte offset into the parameter block, 4-byte aligned
    i32 offset = 0;
    i32 size_bytes = 0;

    /// what a `constant` holds, or what an `attribute_descriptor`'s elements hold; unused for a `texture_index`
    attribute_format format = attribute_format::of_scalar(scalar_type::f32);

    /// index into `resolved_material::attributes` — which resolved attribute this slot serves
    i32 attribute_index = 0;
};

/// The per-instance parameter block one permutation reads: every slot, and how big the block is.
///
/// Two materials of one permutation have the same layout and different contents, which is exactly the split `permutation_key` and
/// `parameter_key` express.
struct sv::material_parameter_layout
{
    cc::vector<material_slot> slots;
    i32 size_bytes = 0;
};

/// A material permutation, as HLSL plus the parameter layout that source reads.
struct sv::generated_material_shader
{
    cc::string source;
    material_parameter_layout layout;

    /// The sampler states this source declares, in declaration order — `samplers[i]` is what `sv_sampler_i` must be.
    /// The generated text names a register, never a state, so nothing else can recover which state belongs to which register.
    cc::vector<sg::sampler> samplers;

    /// Whether a cutout is possible on THIS permutation: its type declares an `opacity_attribute`, and something other
    /// than the signature's own default supplied it.
    ///
    /// Both halves are load-bearing.
    /// A type that never writes `geometry_opacity` leaves the default 1, and a permutation that left its opacity
    /// attribute unbound writes a compile-time constant — an any-hit in either case runs at every intersection to reject
    /// nothing, and costs the instance the hardware's opaque fast path to do it.
    /// This is what `view_renderer` turns into `sg::tlas_instance::opaque_override`, so it decides whether the any-hit
    /// can be invoked at all rather than only whether one is compiled.
    bool can_cut_out = false;

    /// What the compile is cached on: the resolution's shape and how these options spell it (see `material_shader_key`).
    cc::hash128 key;
};

/// How a generated shader is spelled, for a caller that is not the default trace.
struct sv::material_shader_options
{
    /// the function the fragment ends up inside
    cc::string_view entry_point = "sv_evaluate_material";

    /// the runtime contract to include; must be resolvable by whatever compiles the result
    cc::string_view runtime_include = "material_runtime.hlsli";

    /// Emitted AFTER the entry function, for code that calls it — the path tracer's closest-hit above all.
    ///
    /// It has to be an epilogue rather than an ordinary include: HLSL needs `sv_evaluate_material` defined before anything calls
    /// it, and this file is where that definition lands.
    /// Empty emits nothing, which is what a caller wanting only the material function asks for.
    cc::string_view epilogue_include = {};

    /// how many elements each bindless table is declared with; must match the `gpu_resource_manager`'s budgets
    bindless_config const* bindless = nullptr;
};

namespace sv
{
/// The HLSL for `r`, plus the parameter layout it reads.
///
/// The source is, in order: the runtime include, the bindless tables this permutation touches, one `SamplerState` per distinct
/// sampler it samples with, then the entry function.
/// That function declares one local per signature attribute — a constant loaded from the parameter block, a mesh attribute
/// interpolated across the hit triangle, or a texture sampled through its uv attribute — and then runs the type's fragment
/// verbatim over them.
///
/// The loads themselves happen in a nested block, so the attribute names, `surface` and `ctx` are the only names the fragment
/// shares a scope with.
/// That is what lets a material type name an attribute `params` or `uv` without the generator having to know.
///
/// Only what the permutation touches is declared: a material sampling no texture emits no texture table, so the reflection a
/// caller binds against stays as small as the material is.
///
/// The generated text depends on `r.permutation_key` AND on `opts`, so `key` covers both — two calls agreeing on the pair
/// generate the same source, byte for byte, and nothing else may share their cache entry.
///
/// Every attribute must be a scalar or vector of `f32`, `i32` or `u32`; a matrix or a 64-bit / narrow scalar asserts, since
/// neither has a settled `ByteAddressBuffer` layout here yet.
[[nodiscard]] generated_material_shader generate_material_shader(resolved_material const& r,
                                                                 material_shader_options const& opts = {});

/// The HLSL type `format` maps to — `float`, `float3`, `uint2`, ...
/// Empty for a format the generator does not support, which is what `generate_material_shader` asserts on.
[[nodiscard]] cc::string_view hlsl_type_of(attribute_format format);

/// The key `generate_material_shader` would return for `permutation_key` under `opts`, without generating anything.
///
/// `permutation_key` is the resolution's shape; `opts` is how that shape is spelled — the entry point, the two includes, and
/// the bindless budgets the tables are declared with.
/// A cache computes this first and only generates on a miss, which is also why the budgets enter by value rather than as the
/// `bindless_config const*` the caller happened to pass.
[[nodiscard]] cc::hash128 material_shader_key(cc::hash128 permutation_key, material_shader_options const& opts);
} // namespace sv
