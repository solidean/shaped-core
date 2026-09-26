#include "portability.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/resource/views.hh>

using namespace sg;

namespace
{
bool needs_extended_image_format(pixel_format f)
{
    return supports_typed_uav(f) && !is_portable_image_format(f);
}

pixel_format read_format_of(raw_texture_view const& view)
{
    if (view.format != pixel_format::undefined || view.texture == nullptr)
        return view.format;
    return view.texture->format();
}
} // namespace

cc::optional<cc::string> impl::find_unsupported_binding(bool extended_image_formats, cc::span<binding const> bindings)
{
    for (auto const& b : bindings)
        if (!is_valid_access(b.type, b.access))
            return cc::format("binding_group_layout: '{}' carries an access its kind cannot: only an image is ever "
                              "write-only, and only a buffer, bytes or an image is ever written",
                              b.name);
    if (extended_image_formats)
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
    if (!desc.usage.has(texture_usage::image) || !needs_extended_image_format(desc.format))
        return {};
    if (extended_image_formats)
        return {};
    return cc::string("texture: image usage in a format outside the portable image formats needs "
                      "sg::feature::extended_image_formats (webgpu's texture-formats-tier1), and this device lacks "
                      "it");
}

cc::optional<cc::string> impl::find_unsupported_view(bool float32_filtering,
                                                     binding const& b,
                                                     cc::span<raw_view const> views)
{
    if (float32_filtering)
        return {};
    if (b.type != binding_type::texture || b.sample_type != texture_sample_type::filterable_float)
        return {};
    for (auto const& view : views)
        if (auto const* const texture = try_as_texture_view(view);
            texture != nullptr && is_float32_format(read_format_of(*texture)))
            return cc::format("binding_group: '{}' is a filterable texture and its view is a 32-bit float format, "
                              "which needs sg::feature::float32_filtering (webgpu's float32-filterable), and this "
                              "device lacks it; declare the binding unfilterable instead",
                              b.name);
    return {};
}

cc::optional<cc::string> impl::find_unsupported_view(bool float32_filtering,
                                                     cc::span<binding const> bindings,
                                                     cc::span<named_view const> views)
{
    if (float32_filtering)
        return {};
    for (auto const& v : views)
        for (auto const& b : bindings)
            if (b.name == v.name)
                if (auto message = find_unsupported_view(false, b, v.view.span()); message.has_value())
                    return message;
    return {};
}

cc::optional<cc::string> impl::find_unsupported_view(bool float32_filtering,
                                                     cc::span<binding const> bindings,
                                                     cc::span<slotted_view const> views)
{
    if (float32_filtering)
        return {};
    for (auto const& v : views)
    {
        auto const slot = isize(v.slot);
        if (v.slot == binding_slot::invalid || slot >= bindings.size())
            continue;
        if (auto message = find_unsupported_view(false, bindings[slot], v.view.span()); message.has_value())
            return message;
    }
    return {};
}
