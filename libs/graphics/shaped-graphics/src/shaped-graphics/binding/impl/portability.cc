#include "portability.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/binding/binding_group_layout.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/resource/views.hh>

using namespace sg;

namespace
{
bool needs_extended_storage(pixel_format f)
{
    return supports_typed_uav(f) && !is_portable_storage_format(f);
}

cc::optional<cc::string> judge_view(context const& ctx, binding const& b, bound_view const& bound)
{
    if (b.type != binding_type::readonly_texture || b.sample_type != texture_sample_type::filterable_float)
        return {};
    if (ctx.supports(feature::float32_filtering))
        return {};
    for (auto const& view : bound.span())
        if (auto const* const texture = try_as_texture_view(view);
            texture != nullptr && is_float32_format(texture->format))
            return cc::format("binding_group: '{}' is a filterable texture and its view is a 32-bit float format, "
                              "which needs sg::feature::float32_filtering (webgpu's float32-filterable), and this "
                              "device lacks it; declare the binding unfilterable instead",
                              b.name);
    return {};
}
} // namespace

cc::optional<cc::string> impl::find_unsupported_binding(context const& ctx, cc::span<binding const> bindings)
{
    if (ctx.supports(feature::extended_storage_formats))
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

cc::optional<cc::string> impl::find_unsupported_texture(context const& ctx, texture_description const& desc)
{
    if (!desc.usage.has(texture_usage::readwrite_texture) || !needs_extended_storage(desc.format))
        return {};
    if (ctx.supports(feature::extended_storage_formats))
        return {};
    return cc::string("texture: a storage texture in a format outside the portable storage formats needs "
                      "sg::feature::extended_storage_formats (webgpu's texture-formats-tier1), and this device lacks "
                      "it");
}

cc::optional<cc::string> impl::find_unsupported_view(context const& ctx,
                                                     binding_group_layout const& layout,
                                                     cc::span<named_view const> views)
{
    for (auto const& v : views)
        for (auto const& b : layout.bindings())
            if (b.name == v.name)
                if (auto message = judge_view(ctx, b, v.view); message.has_value())
                    return message;
    return {};
}

cc::optional<cc::string> impl::find_unsupported_view(context const& ctx,
                                                     binding_group_layout const& layout,
                                                     cc::span<slotted_view const> views)
{
    auto const bindings = layout.bindings();
    for (auto const& v : views)
    {
        auto const slot = isize(v.slot);
        if (v.slot == binding_slot::invalid || slot >= bindings.size())
            continue;
        if (auto message = judge_view(ctx, bindings[slot], v.view); message.has_value())
            return message;
    }
    return {};
}
