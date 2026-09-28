#include <nexus/test.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/binding/pipeline_layout.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/exceptions.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/resource/texture.hh>

using namespace cc::primitive_defines;

// Each form below is refused where the device lacks its feature and built where it has it, on every backend alike.

namespace
{
sg::texture_description texture_of(sg::pixel_format format, sg::texture_usages usage)
{
    return {.format = format, .dimension = sg::texture_dimension::d2, .width = 4, .height = 4, .usage = usage};
}

/// Whether `create` returns rather than throws the sg::exception a refused creation is.
bool creates(auto&& create)
{
    try
    {
        (void)create();
        return true;
    }
    catch (sg::exception const&)
    {
        return false;
    }
}
} // namespace

INVOCABLE_TEST("sg - an image format outside the portable set needs extended_image_formats",
               (sg::context_handle const& ctx))
{
    auto const extended = ctx->supports(sg::feature::extended_image_formats);
    auto const storage = sg::texture_usage::image;

    auto const persistent = [&](sg::pixel_format f, sg::texture_usages usage)
    { return creates([&] { return ctx->persistent.create_raw_texture(texture_of(f, usage)); }); };
    CHECK(persistent(sg::pixel_format::r8_unorm, storage) == extended);
    CHECK(creates([&] { return ctx->transient.create_raw_texture(texture_of(sg::pixel_format::rgb10a2_unorm, storage)); })
          == extended);
    CHECK(persistent(sg::pixel_format::rgba8_unorm, storage));
    // Only storage asks: the same format sampled is portable everywhere.
    CHECK(persistent(sg::pixel_format::r8_unorm, sg::texture_usage::texture));

    auto bindings = cc::vector<sg::binding>();
    bindings.push_back({.name = "target", .type = sg::binding_type::image, .access = sg::access_mode::read_write});
    bindings[0].texture_dimension = sg::texture_view_dimension::tex_2d;
    bindings[0].image_format = sg::pixel_format::r8_unorm;
    bindings[0].access = sg::access_mode::write;
    sg::apply_stage_visibility(bindings, sg::shader_stage::compute);
    CHECK(ctx->uncached.try_create_binding_group_layout(bindings).has_value() == extended);
}

INVOCABLE_TEST("sg - a 32-bit float view on a filterable binding needs float32_filtering",
               (sg::context_handle const& ctx))
{
    auto bindings = cc::vector<sg::binding>();
    bindings.push_back({.name = "source", .type = sg::binding_type::texture});
    bindings[0].texture_dimension = sg::texture_view_dimension::tex_2d;
    bindings[0].sample_type = sg::texture_sample_type::filterable_float;
    sg::apply_stage_visibility(bindings, sg::shader_stage::compute);
    auto const filterable = ctx->uncached.create_binding_group_layout(bindings);

    auto const sampled = sg::texture_usage::texture;
    auto const r32
        = sg::texture_2d::from_raw(ctx->persistent.create_raw_texture(texture_of(sg::pixel_format::r32_float, sampled)));
    auto const rgba8 = sg::texture_2d::from_raw(
        ctx->persistent.create_raw_texture(texture_of(sg::pixel_format::rgba8_unorm, sampled)));

    auto const group_of = [&](sg::binding_group_layout_handle const& layout, sg::texture_2d const& t)
    {
        auto const views = cc::vector<sg::named_view>{{.name = "source", .view = t.as_texture_view()}};
        return creates([&] { return ctx->persistent.create_binding_group(layout, views, {}); });
    };
    CHECK(group_of(filterable, r32) == ctx->supports(sg::feature::float32_filtering));
    CHECK(group_of(filterable, rgba8));

    // Declared unfilterable, the same view binds on every device.
    bindings[0].sample_type = sg::texture_sample_type::unfilterable_float;
    CHECK(group_of(ctx->uncached.create_binding_group_layout(bindings), r32));
}

INVOCABLE_TEST("sg - a pipeline-level static sampler builds a layout on every backend, and two at one register do not",
               (sg::context_handle const& ctx))
{
    auto const at = [](cc::string_view name, u32 index)
    {
        return sg::bound_sampler{
            .binding = {.name = name, .space = 0u, .index = index, .type = sg::binding_type::sampler},
            .sampler = {.min_filter = sg::sampler_filter::nearest}};
    };
    CHECK(ctx->uncached.try_create_pipeline_layout({.static_samplers = {at("point", 0), at("other", 1)}}).has_value());
    CHECK(!ctx->uncached.try_create_pipeline_layout({.static_samplers = {at("point", 0), at("clash", 0)}}).has_value());
}
