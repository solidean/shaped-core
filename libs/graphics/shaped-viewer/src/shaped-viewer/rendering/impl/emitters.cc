#include <clean-core/common/utility.hh> // cc::memcpy
#include <shaped-viewer/material/shader_generator.hh>
#include <shaped-viewer/rendering/impl/emitters.hh>
#include <shaped-viewer/resources/instance_data.hh>
#include <shaped-viewer/resources/resource_managers.hh> // sv::residency
#include <shaped-viewer/resources/value_summary.hh>
#include <typed-geometry/scalar/scalar.hh>

namespace sv
{
namespace
{
/// One factor of an emission as selection sees it, per lane; a one-lane factor, the scale, carries its value in every lane.
///
/// `mean` is the factor's mean where its data is summarized, and otherwise what an unknown value of 1 makes through the swizzle and the transform.
/// `bound` is an estimate of a value no lane exceeds where `has_bound` holds.
/// `dark` is the proof that every lane is at or below zero wherever it is read, which only lanes known exactly give.
/// `finite` is false where the block holds a non-finite constant or transform, which no estimate can weigh.
struct factor_estimate
{
    tg::vec3f mean = tg::vec3f(1, 1, 1);
    tg::vec3f bound = tg::vec3f(1, 1, 1);
    bool has_bound = false;
    bool dark = false;
    bool finite = true;
};

[[nodiscard]] bool is_dark(tg::vec3f v)
{
    return v[0] <= 0.0f && v[1] <= 0.0f && v[2] <= 0.0f;
}

/// The slot of `block` of `kind` at byte `offset`, or null.
[[nodiscard]] instance_slot const* slot_at(instance_record const& block, material_slot_kind kind, u32 offset)
{
    for (auto const& slot : block.slots)
        if (slot.kind == kind && u32(slot.offset) == offset)
            return &slot;
    return nullptr;
}

/// The `index`-th float of a constant payload, or 0 past its end.
[[nodiscard]] f32 float_at(cc::vector<byte> const& bytes, isize index)
{
    auto value = 0.0f;
    if ((index + 1) * 4 <= bytes.size())
        cc::memcpy(&value, bytes.data() + index * 4, 4);
    return value;
}

[[nodiscard]] bool all_finite(tg::vec3f v)
{
    return tg::is_finite(v[0]) && tg::is_finite(v[1]) && tg::is_finite(v[2]);
}

/// What a summary says of one swizzle selector: a channel, or the 0 and 1 that codes 4 and 5 read.
/// Without a summary a channel reads as an unknown value of 1, while the two constants stay known.
struct lane_summary
{
    f32 mean = 1;
    f32 min = 1;
    f32 max = 1;
};

[[nodiscard]] lane_summary lane_of(value_summary const* s, u32 channel)
{
    if (channel >= 4)
    {
        auto const constant = channel == 4 ? 0.0f : 1.0f;
        return {.mean = constant, .min = constant, .max = constant};
    }
    if (s == nullptr)
        return {};
    return {.mean = s->mean[i32(channel)], .min = s->min[i32(channel)], .max = s->max[i32(channel)]};
}

/// How far below zero, relative to the terms it sums, a texture lane under a bias must stay to be dark.
/// It covers the GPU's filtering weights, its sRGB decode tolerance and a fused multiply-add, none of which this rounds as.
constexpr auto dark_margin = 1e-3f;

/// Factor `f` of `block`, `lanes` wide, as `tracer.emission_factor` reads it on the GPU.
[[nodiscard]] factor_estimate estimate_of(material_emission_factor const& f,
                                          i32 lanes,
                                          instance_record const& block,
                                          impl::emission_sources const& sources)
{
    auto const widen = [&](tg::vec3f v) { return lanes == 1 ? tg::vec3f(v[0], v[0], v[0]) : v; };

    switch (f.kind)
    {
    case material_value_kind::none:
        return {.mean = tg::vec3f(1, 1, 1), .bound = tg::vec3f(1, 1, 1), .has_bound = true};

    case material_value_kind::constant:
    {
        auto value = tg::vec3f(0, 0, 0);
        if (auto const* const slot = slot_at(block, material_slot_kind::constant, f.offset); slot != nullptr)
            for (auto c = 0; c < lanes; ++c)
                value[c] = float_at(slot->constant, c);
        return {.mean = widen(value),
                .bound = widen(value),
                .has_bound = true,
                .dark = is_dark(widen(value)),
                .finite = all_finite(value)};
    }

    case material_value_kind::attribute:
    {
        auto const* const slot = slot_at(block, material_slot_kind::attribute_descriptor, f.offset);
        auto const summary = slot != nullptr ? sources.attribute(slot->attribute) : cc::optional<value_summary>();
        if (!summary.has_value())
            return {};
        // Interpolated corners stay within the range of the elements, so an exact range bounds every point.
        auto const& s = summary.value();
        auto const bound = widen(tg::vec3f(s.max[0], s.max[1], s.max[2]));
        return {.mean = widen(tg::vec3f(s.mean[0], s.mean[1], s.mean[2])),
                .bound = bound,
                .has_bound = true,
                .dark = s.exact && is_dark(bound)};
    }

    case material_value_kind::texture:
    {
        auto const* const slot = slot_at(block, material_slot_kind::texture_index, f.offset);
        auto const summary = slot != nullptr ? sources.texture(slot->texture) : cc::optional<value_summary>();
        auto const* const s = summary.has_value() ? &summary.value() : nullptr;

        auto const* const transform = f.transform == material_opacity::no_offset
                                        ? nullptr
                                        : slot_at(block, material_slot_kind::sample_transform, f.transform);
        auto out = factor_estimate{.has_bound = s != nullptr};
        auto scales = tg::vec3f(1, 1, 1);
        auto biases = tg::vec3f(0, 0, 0);
        auto dark = true;
        for (auto c = 0; c < lanes; ++c)
        {
            auto const lane = lane_of(s, f.channels[c]);
            if (transform != nullptr)
            {
                scales[c] = float_at(transform->constant, c);
                biases[c] = float_at(transform->constant, 4 + c);
            }
            // A filtered sample stays within the texels' range, which a negative scale turns over.
            auto const term = (scales[c] >= 0.0f ? lane.max : lane.min) * scales[c];
            out.mean[c] = lane.mean * scales[c] + biases[c];
            out.bound[c] = term + biases[c];
            // A lane is known exactly where an exact summary bounds it, where it reads a constant selector, or where a scale of 0 leaves its bias alone.
            auto const known = (s != nullptr && s->exact) || f.channels[c] >= 4 || scales[c] == 0.0f;
            // A bound that a bias cancels to about zero is no proof: the GPU's rounding may leave the sample just above it.
            auto const margin = biases[c] == 0.0f ? 0.0f : dark_margin * (tg::abs(term) + tg::abs(biases[c]));
            auto const lane_dark = biases[c] == 0.0f ? out.bound[c] <= 0.0f : out.bound[c] < -margin;
            dark = dark && known && lane_dark;
        }
        out.mean = widen(out.mean);
        out.bound = widen(out.bound);
        out.dark = dark;
        out.finite = all_finite(scales) && all_finite(biases);
        return out;
    }
    }
    return {};
}

[[nodiscard]] tg::vec3f clamped(tg::vec3f v)
{
    return tg::vec3f(cc::max(v[0], 0.0f), cc::max(v[1], 0.0f), cc::max(v[2], 0.0f));
}

/// The radiance `max(0, scale) * max(0, color)`, the scale's in every lane.
/// Finite factors whose product overflows are held at the largest f32, so the estimate stays finite.
[[nodiscard]] tg::vec3f radiance_of(tg::vec3f scale, tg::vec3f color)
{
    auto const largest = impl::largest_finite_f32;
    auto const s = cc::max(scale[0], 0.0f);
    auto const c = clamped(color);
    return tg::vec3f(cc::min(s * c[0], largest), cc::min(s * c[1], largest), cc::min(s * c[2], largest));
}

/// What stands in for a factor that nothing proves dark, yet no read saw above zero, as a scan that read only black.
/// It may emit, so it stays above zero; the estimate it makes is marked `unseen`, which selection gives only a sliver of the choice.
constexpr auto unseen_emission = 1e-6f;

/// What stands in for a factor where the means multiply to zero: its bound where that is above zero, exact or not, else its mean where that is.
/// Empty where neither is, which `unseen_emission` then stands in for.
[[nodiscard]] cc::optional<tg::vec3f> stand_in_of(factor_estimate const& f, i32 lanes)
{
    auto const positive = [&](tg::vec3f v) { return lanes == 1 ? v[0] > 0.0f : !is_dark(v); };
    if (f.has_bound && positive(f.bound))
        return f.bound;
    if (positive(f.mean))
        return f.mean;
    return {};
}
} // namespace

cc::optional<value_summary> impl::sampled_texture_summary(residency state, cc::optional<value_summary> const& summary)
{
    if (state == residency::base_resident || state == residency::complete)
        return summary;
    return {};
}

cc::optional<impl::emitter_estimate> impl::emitter_estimate_of(material_emission const& emission,
                                                               instance_record const& block,
                                                               emission_sources const& sources)
{
    if (!emission.declares())
        return {};

    auto const color = estimate_of(emission.color, 3, block, sources);
    auto const scale = estimate_of(emission.scale, 1, block, sources);

    // A non-finite constant or transform has no estimate to weigh, and one would poison the selection CDF.
    if (!color.finite || !scale.finite)
        return {};

    // Dark wherever it is read: every lane of the color, or the scale, known exactly and at or below zero.
    if (color.dark || scale.dark)
        return {};

    auto const mean = radiance_of(scale.mean, color.mean);
    if (!is_dark(mean))
        return emitter_estimate{.radiance = mean};

    // Means that multiply to zero belong to an emission that is not dark throughout, so stand-ins take their place.
    // Selection weighs its noise and never its mean, and has to keep picking it.
    // Stand-ins multiply to zero only where their product underflows.
    auto const unseen = tg::vec3f(unseen_emission, unseen_emission, unseen_emission);
    auto const scale_in = stand_in_of(scale, 1);
    auto const color_in = stand_in_of(color, 3);
    auto const fallback = radiance_of(scale_in.has_value() ? scale_in.value() : unseen,
                                      color_in.has_value() ? color_in.value() : unseen);
    if (!is_dark(fallback))
        return emitter_estimate{.radiance = fallback, .unseen = !scale_in.has_value() || !color_in.has_value()};
    return emitter_estimate{.radiance = unseen, .unseen = true};
}

f32 impl::world_area_of(f32 object_area, tg::affine_transform3f const& placement)
{
    return object_area * tg::pow(tg::abs(placement.linear_mat().determinant()), 2.0f / 3.0f);
}

bool impl::is_similarity(tg::mat3f const& linear)
{
    auto const x = linear * tg::vec3f(1, 0, 0);
    auto const y = linear * tg::vec3f(0, 1, 0);
    auto const z = linear * tg::vec3f(0, 0, 1);
    auto const xx = tg::dot(x, x);
    auto const yy = tg::dot(y, y);
    auto const zz = tg::dot(z, z);
    auto const scale = cc::max(xx, cc::max(yy, zz));
    // A map to a point takes every area to nothing, which the estimate gets exactly.
    if (scale <= 0.0f)
        return true;
    // Relative to the squared scale, so a tiny lamp and a huge one are judged alike.
    auto const tolerance = 1e-5f * scale;
    return tg::abs(xx - yy) <= tolerance && tg::abs(yy - zz) <= tolerance && tg::abs(tg::dot(x, y)) <= tolerance
        && tg::abs(tg::dot(y, z)) <= tolerance && tg::abs(tg::dot(x, z)) <= tolerance;
}
} // namespace sv
