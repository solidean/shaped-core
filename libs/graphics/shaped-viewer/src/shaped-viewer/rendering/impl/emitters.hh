#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/function/function_ref.hh>
#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/transform/transform.hh>

// The CPU half of an emissive instance as a light of the trace: whether it is one, and what selection weighs it by.
// libs/graphics/shaped-viewer/docs/lights.md#emissive-instances is the design; the GPU half is shaders/sgl/tracer_shade.sgl's `emitter_radiance`.

namespace sv::impl
{
/// The largest finite f32, at which an overflowing estimate or selection weight is held.
/// WORKAROUND: belongs in typed-geometry as a scalar limit `tg::max_finite<T>`, a variable template beside `tg::pi<T>`, whose `half_float` value is `half_float::max`.
inline constexpr f32 largest_finite_f32 = 3.40282347e38f;

/// What selection knows of the data a non-constant emission factor reads, or empty for one it knows nothing of.
struct emission_sources
{
    cc::function_ref<cc::optional<value_summary>(texture_id)> texture;
    cc::function_ref<cc::optional<value_summary>(attribute_id)> attribute;
};

/// What selection weighs an emissive instance by.
struct emitter_estimate
{
    /// An estimate of the mean radiance, finite and above zero in some channel.
    tg::vec3f radiance = tg::vec3f(0, 0, 0);

    /// Whether `radiance` rests on a stand-in for a factor no read saw above zero, as a strided scan that met only black.
    /// Such an emitter may emit, so it keeps a weight above zero, yet it is no evidence of light: `sv::light_flag_unseen_emission` carries it to selection.
    bool unseen = false;
};

/// The summary of a texture in residency `state`, as selection may trust it: `summary` once the texture is sampled, else empty.
/// A pending texture samples a placeholder and a failed one magenta, so its own texels prove nothing of what the GPU reads.
[[nodiscard]] cc::optional<value_summary> sampled_texture_summary(residency state,
                                                                  cc::optional<value_summary> const& summary);

/// What selection weighs an instance by: an estimate of the mean radiance `emission` sends, read out of its parameter block `block`.
///
/// **Empty where the instance is no light**, and then only the camera sees it, whatever bounces later meet it.
/// So emptiness must be proven: its type declares no emission, or a factor is at or below zero wherever it is read.
/// A constant factor is that when it is at or below zero, and a texture or a mesh attribute when an exact summary bounds every value it holds there.
/// A texture's lane is known exactly without a summary where it reads a constant selector, or where its transform's scale of 0 leaves the bias alone.
/// Under a bias that is not zero, a texture's lane must stay a margin below zero to be dark, since the GPU's filtering, sRGB decode and fused multiply-add round otherwise than this does.
/// It is empty too where a constant or a transform is not finite, which no estimate can weigh.
/// Every other emission qualifies, whatever its factors' kinds, since the GPU reads each at the sampled point.
///
/// The estimate is `max(0, scale) * max(0, color)` over each factor's mean, a texture's through its swizzle and its scale and bias.
/// A factor `sources` knows nothing of reads as 1 before that swizzle and transform.
/// The estimate is finite, and above zero in some channel.
/// Where the means multiply to zero, each factor's bound stands in, exact or not, else its mean, else a tiny value above zero.
/// That last is a scan that read only black, and marks the estimate `unseen`.
/// The renderer adds the rest of the rule — a resident triangle mesh, shaded by its own permutation rather than the fallback.
[[nodiscard]] cc::optional<emitter_estimate> emitter_estimate_of(material_emission const& emission,
                                                                 instance_record const& block,
                                                                 emission_sources const& sources);

/// What `placement` makes of an object-space area: exact under a similarity, and an estimate under a non-uniform scale.
/// The estimate undercounts a flat lamp flattened along its normal, to nothing at a zero scale, so the renderer asks
/// `mesh_manager::acquire_world_area` for the exact area there and holds this only until that lands.
/// Only an emitter's selection weight reads it, which a rough area leaves unbiased.
[[nodiscard]] f32 world_area_of(f32 object_area, tg::affine_transform3f const& placement);

/// Whether `linear` is a rotation, possibly mirrored, times one uniform scale, under which `world_area_of` is exact.
/// TEMPORARY until typed-geometry can classify a linear map, which is where this belongs.
[[nodiscard]] bool is_similarity(tg::mat3f const& linear);
} // namespace sv::impl
