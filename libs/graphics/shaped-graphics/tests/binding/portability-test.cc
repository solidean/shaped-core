#include <nexus/test.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/impl/portability.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/resource/views.hh>

// The judgement behind every feature refusal, run against a device that lacks the feature.
// A desktop device grants both features, so a device-backed test rarely reaches these refusals.

namespace
{
// A texture with a shape and no GPU storage, which is all a view's judgement reads.
class shape_only_texture final : public sg::raw_texture
{
public:
    explicit shape_only_texture(sg::texture_description const& desc) : raw_texture(desc) {}
};

sg::texture_description texture_of(sg::pixel_format format, sg::texture_usages usage)
{
    return {.format = format, .dimension = sg::texture_dimension::d2, .width = 4, .height = 4, .usage = usage};
}

sg::raw_view view_of(sg::pixel_format texture_format, sg::pixel_format view_format)
{
    auto const texture
        = std::make_shared<shape_only_texture const>(texture_of(texture_format, sg::texture_usage::texture));
    return sg::raw_texture_view{.kind = sg::view_class::texture,
                                .texture = texture,
                                .view_dimension = sg::texture_view_dimension::tex_2d,
                                .format = view_format};
}

sg::binding sampled_binding(cc::optional<sg::texture_sample_type> sample_type)
{
    auto b = sg::binding{.name = "source", .type = sg::binding_type::texture};
    b.texture_dimension = sg::texture_view_dimension::tex_2d;
    b.sample_type = sample_type;
    return b;
}

/// Whether a device without float32_filtering refuses `view` bound to `b`.
bool refused(sg::binding const& b, sg::raw_view const& view)
{
    return sg::impl::find_unsupported_view(false, b, cc::span<sg::raw_view const>(&view, 1)).has_value();
}
} // namespace

TEST("sg::portability - an image format outside the portable set is refused without extended_image_formats")
{
    auto const storage = sg::texture_usage::image;
    CHECK(sg::impl::find_unsupported_texture(false, texture_of(sg::pixel_format::r8_unorm, storage)).has_value());
    CHECK(sg::impl::find_unsupported_texture(false, texture_of(sg::pixel_format::bgra8_unorm, storage)).has_value());
    CHECK(!sg::impl::find_unsupported_texture(true, texture_of(sg::pixel_format::r8_unorm, storage)).has_value());
    CHECK(!sg::impl::find_unsupported_texture(false, texture_of(sg::pixel_format::rgba8_unorm, storage)).has_value());
    // Only storage asks: the same format sampled is portable everywhere.
    CHECK(!sg::impl::find_unsupported_texture(false, texture_of(sg::pixel_format::r8_unorm, sg::texture_usage::texture))
               .has_value());

    auto b = sg::binding{.name = "target", .type = sg::binding_type::image, .access = sg::access_mode::read_write};
    b.texture_dimension = sg::texture_view_dimension::tex_2d;
    b.image_format = sg::pixel_format::r8_unorm;
    b.access = sg::access_mode::write;
    auto const bindings = cc::span<sg::binding const>(&b, 1);
    CHECK(sg::impl::find_unsupported_binding(false, bindings).has_value());
    CHECK(!sg::impl::find_unsupported_binding(true, bindings).has_value());

    b.image_format = sg::pixel_format::rgba8_unorm;
    CHECK(!sg::impl::find_unsupported_binding(false, bindings).has_value());
}

TEST("sg::portability - an access a kind cannot carry is refused whatever the device")
{
    auto const write_only_buffer
        = sg::binding{.name = "out", .type = sg::binding_type::buffer, .access = sg::access_mode::write};
    auto const read_write_texture
        = sg::binding{.name = "in", .type = sg::binding_type::texture, .access = sg::access_mode::read_write};
    CHECK(sg::impl::find_unsupported_binding(true, cc::span<sg::binding const>(&write_only_buffer, 1)).has_value());
    CHECK(sg::impl::find_unsupported_binding(true, cc::span<sg::binding const>(&read_write_texture, 1)).has_value());
}

TEST("sg::portability - a 32-bit float view is refused on a filterable binding without float32_filtering")
{
    auto const r32 = view_of(sg::pixel_format::r32_float, sg::pixel_format::r32_float);
    auto const rgba8 = view_of(sg::pixel_format::rgba8_unorm, sg::pixel_format::rgba8_unorm);

    auto const filterable = sampled_binding(sg::texture_sample_type::filterable_float);
    CHECK(refused(filterable, r32));
    CHECK(!refused(filterable, rgba8));
    CHECK(!sg::impl::find_unsupported_view(true, filterable, cc::span<sg::raw_view const>(&r32, 1)).has_value());

    // Declared unfilterable, or declaring no sample type at all, the same view binds on every device.
    CHECK(!refused(sampled_binding(sg::texture_sample_type::unfilterable_float), r32));
    CHECK(!refused(sampled_binding({}), r32));
}

TEST("sg::portability - a view of undefined format is judged by its texture's own format")
{
    auto const filterable = sampled_binding(sg::texture_sample_type::filterable_float);
    CHECK(refused(filterable, view_of(sg::pixel_format::r32_float, sg::pixel_format::undefined)));
    CHECK(!refused(filterable, view_of(sg::pixel_format::rgba8_unorm, sg::pixel_format::undefined)));
}
