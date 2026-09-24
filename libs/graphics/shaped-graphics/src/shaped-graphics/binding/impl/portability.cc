#include "portability.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/resource/views.hh>

using namespace sg;

namespace
{
bool needs_extended_storage(pixel_format f)
{
    return supports_typed_uav(f) && !is_portable_storage_format(f);
}

pixel_format read_format_of(raw_texture_view const& view)
{
    if (view.format != pixel_format::undefined || view.texture == nullptr)
        return view.format;
    return view.texture->format();
}
} // namespace

cc::optional<cc::string> impl::find_unsupported_binding(bool extended_storage_formats, cc::span<binding const> bindings)
{
    if (extended_storage_formats)
        return {};
    for (auto const& b : bindings)
        if (b.type == binding_type::readwrite_texture && b.storage_format.has_value()
            && needs_extended_storage(b.storage_format.value()))
            return cc::format("binding_group_layout: storage texture '{}' declares a format outside the portable "
                              "storage formats, which needs sg::feature::extended_storage_formats (webgpu's "
                              "texture-formats-tier1), and this device lacks it",
                              b.name);
    return {};
}

cc::optional<cc::string> impl::find_unsupported_texture(bool extended_storage_formats, texture_description const& desc)
{
    if (!desc.usage.has(texture_usage::readwrite_texture) || !needs_extended_storage(desc.format))
        return {};
    if (extended_storage_formats)
        return {};
    return cc::string("texture: a storage texture in a format outside the portable storage formats needs "
                      "sg::feature::extended_storage_formats (webgpu's texture-formats-tier1), and this device lacks "
                      "it");
}

cc::optional<cc::string> impl::find_unsupported_view(bool float32_filtering,
                                                     binding const& b,
                                                     cc::span<raw_view const> views)
{
    if (float32_filtering)
        return {};
    if (b.type != binding_type::readonly_texture || b.sample_type != texture_sample_type::filterable_float)
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
