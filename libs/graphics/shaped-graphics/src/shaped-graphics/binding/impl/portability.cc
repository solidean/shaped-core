#include "portability.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/resource/views.hh>

using namespace sg;

namespace
{
bool needs_extended_image_format(pixel_format f)
{
    return supports_typed_uav(f) && !is_portable_image_format(f);
}

/// Whether `supported` has every feature a view is judged by, so that no view is refused.
bool judges_no_view(feature_set supported)
{
    return supported.has(feature::float32_filtering) && supported.has(feature::norm16_filtering)
        && supported.has(feature::readwrite_norm16_image_formats);
}

pixel_format read_format_of(raw_texture_view const& view)
{
    if (view.format != pixel_format::undefined || view.texture == nullptr)
        return view.format;
    return view.texture->format();
}
} // namespace

cc::optional<cc::string> impl::find_unsupported_binding(feature_set supported, cc::span<binding const> bindings)
{
    for (auto const& b : bindings)
        if (!is_valid_access(b.type, b.access))
            return cc::format("binding_group_layout: '{}' carries an access its kind cannot: only an image is ever "
                              "write-only, and only a buffer, bytes or an image is ever written",
                              b.name);
    for (auto const& b : bindings)
        if (b.type == binding_type::image && b.access == access_mode::read_write
            && is_norm16_format(b.image_format.value_or(pixel_format::undefined))
            && !supported.has(feature::readwrite_norm16_image_formats))
            return cc::format("binding_group_layout: image '{}' is read_write in a 16-bit normalized format, which "
                              "needs sg::feature::readwrite_norm16_image_formats, which no webgpu or metal device "
                              "has, and this device lacks it; declare it write or read instead",
                              b.name);
    if (supported.has(feature::extended_image_formats))
        return {};
    for (auto const& b : bindings)
        if (b.type == binding_type::image && b.image_format.has_value()
            && needs_extended_image_format(b.image_format.value()))
            return cc::format("binding_group_layout: image '{}' declares a format outside the portable "
                              "image formats, which needs sg::feature::extended_image_formats (webgpu's "
                              "texture-formats-tier1), and this device lacks it",
                              b.name);
    return {};
}

cc::optional<cc::string> impl::find_unsupported_texture(bool extended_image_formats, texture_description const& desc)
{
    if (extended_image_formats)
        return {};
    // core WebGPU has no texture of these at all, sampled, rendered to or copied, so any usage needs the feature
    if (is_norm16_format(desc.format))
        return cc::string("texture: a 16-bit normalized format needs sg::feature::extended_image_formats (webgpu's "
                          "texture-formats-tier1) whatever the texture is used as, and this device lacks it");
    if (!desc.usage.has(texture_usage::image) || !needs_extended_image_format(desc.format))
        return {};
    return cc::string("texture: image usage in a format outside the portable image formats needs "
                      "sg::feature::extended_image_formats (webgpu's texture-formats-tier1), and this device lacks "
                      "it");
}

cc::optional<cc::string> impl::find_unsupported_view(feature_set supported, binding const& b, cc::span<raw_view const> views)
{
    if (judges_no_view(supported))
        return {};
    // a read-write image of a 16-bit normalized view, judged by the view's own format whatever the layout states, so a
    // layout reflected from HLSL with no format is judged too
    if (b.type == binding_type::image && b.access == access_mode::read_write
        && !supported.has(feature::readwrite_norm16_image_formats))
        for (auto const& view : views)
            if (auto const* const image = try_as_texture_view(view);
                image != nullptr && is_norm16_format(read_format_of(*image)))
                return cc::format("binding_group: '{}' is a read_write image and its view is a 16-bit normalized "
                                  "format, which needs sg::feature::readwrite_norm16_image_formats (which no webgpu or "
                                  "metal device has), and this device lacks it; bind it as a write-only or read-only "
                                  "image instead",
                                  b.name);
    if (b.type != binding_type::texture || b.sample_type != texture_sample_type::filterable_float)
        return {};
    for (auto const& view : views)
    {
        auto const* const texture = try_as_texture_view(view);
        if (texture == nullptr)
            continue;
        auto const format = read_format_of(*texture);
        if (is_float32_format(format) && !supported.has(feature::float32_filtering))
            return cc::format("binding_group: '{}' is a filterable texture and its view is a 32-bit float format, "
                              "which needs sg::feature::float32_filtering (webgpu's float32-filterable), and this "
                              "device lacks it; declare the binding unfilterable instead",
                              b.name);
        if (is_norm16_format(format) && !supported.has(feature::norm16_filtering))
            return cc::format("binding_group: '{}' is a filterable texture and its view is a 16-bit normalized "
                              "format, which needs sg::feature::norm16_filtering (which webgpu never has), and this "
                              "device lacks it; declare the binding unfilterable instead",
                              b.name);
    }
    return {};
}

cc::optional<cc::string> impl::find_unsupported_view(feature_set supported,
                                                     cc::span<binding const> bindings,
                                                     cc::span<named_view const> views)
{
    if (judges_no_view(supported))
        return {};
    for (auto const& v : views)
        for (auto const& b : bindings)
            if (b.name == v.name)
                if (auto message = find_unsupported_view(supported, b, v.view.span()); message.has_value())
                    return message;
    return {};
}

cc::optional<cc::string> impl::find_missing_feature(feature_set supported, cc::span<compiled_shader const* const> stages)
{
    for (auto const* const stage : stages)
    {
        if (stage == nullptr || !stage->required_features.has_value())
            continue;
        for (auto const f : k_all_features)
            if (stage->required_features.value().has(f) && !supported.has(f))
                return cc::format("the shader '{}' needs sg::feature::{}, and this device lacks it", stage->entry_point,
                                  to_string(f));
    }
    return {};
}

cc::optional<cc::string> impl::find_reserved_slot(cc::span<compiled_shader const* const> stages)
{
    for (auto const* const stage : stages)
        if (stage != nullptr && !stage->reserved_slots.empty())
        {
            auto const& slot = stage->reserved_slots.front();
            return cc::format(
                "the shader '{}' declares '{}' at u{}, space{}, a reserved slot of NVAPI's that sg lays out "
                "no pipeline for: its host builds the pipeline, covering the slot as a UAV with a counter",
                stage->entry_point, slot.name, slot.index, slot.space);
        }
    return {};
}

cc::optional<cc::string> impl::find_unsupported_view(feature_set supported,
                                                     cc::span<binding const> bindings,
                                                     cc::span<slotted_view const> views)
{
    if (judges_no_view(supported))
        return {};
    for (auto const& v : views)
    {
        auto const slot = isize(v.slot);
        if (v.slot == binding_slot::invalid || slot >= bindings.size())
            continue;
        if (auto message = find_unsupported_view(supported, bindings[slot], v.view.span()); message.has_value())
            return message;
    }
    return {};
}
