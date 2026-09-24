#include <nexus/test.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/binding/pipeline_layout.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/exceptions.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/resource/texture.hh>

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

INVOCABLE_TEST("sg - a storage format outside the portable set needs extended_storage_formats",
               (sg::context_handle const& ctx))
{
    auto const extended = ctx->supports(sg::feature::extended_storage_formats);
    auto const storage = sg::texture_usage::readwrite_texture;

    auto const persistent = [&](sg::pixel_format f, sg::texture_usages usage)
    { return creates([&] { return ctx->persistent.create_raw_texture(texture_of(f, usage)); }); };
    CHECK(persistent(sg::pixel_format::r8_unorm, storage) == extended);
    CHECK(creates([&] { return ctx->transient.create_raw_texture(texture_of(sg::pixel_format::rgb10a2_unorm, storage)); })
          == extended);
    CHECK(persistent(sg::pixel_format::rgba8_unorm, storage));
    // Only storage asks: the same format sampled is portable everywhere.
    CHECK(persistent(sg::pixel_format::r8_unorm, sg::texture_usage::readonly_texture));

    auto bindings = cc::vector<sg::binding>();
    bindings.push_back({.name = "target", .type = sg::binding_type::readwrite_texture});
    bindings[0].texture_dimension = sg::texture_view_dimension::tex_2d;
    bindings[0].storage_format = sg::pixel_format::r8_unorm;
    bindings[0].storage_access = sg::storage_access::write;
    sg::apply_stage_visibility(bindings, sg::shader_stage::compute);
    CHECK(ctx->uncached.try_create_binding_group_layout(bindings).has_value() == extended);
}

INVOCABLE_TEST("sg - a 32-bit float view on a filterable binding needs float32_filtering",
               (sg::context_handle const& ctx))
{
    auto bindings = cc::vector<sg::binding>();
    bindings.push_back({.name = "source", .type = sg::binding_type::readonly_texture});
    bindings[0].texture_dimension = sg::texture_view_dimension::tex_2d;
    bindings[0].sample_type = sg::texture_sample_type::filterable_float;
    sg::apply_stage_visibility(bindings, sg::shader_stage::compute);
    auto const filterable = ctx->uncached.create_binding_group_layout(bindings);

    auto const sampled = sg::texture_usage::readonly_texture;
    auto const r32
        = sg::texture_2d::from_raw(ctx->persistent.create_raw_texture(texture_of(sg::pixel_format::r32_float, sampled)));
    auto const rgba8 = sg::texture_2d::from_raw(
        ctx->persistent.create_raw_texture(texture_of(sg::pixel_format::rgba8_unorm, sampled)));

    auto const group_of = [&](sg::binding_group_layout_handle const& layout, sg::texture_2d const& t)
    {
        auto const views = cc::vector<sg::named_view>{{.name = "source", .view = t.as_readonly_view()}};
        return creates([&] { return ctx->persistent.create_binding_group(layout, views, {}); });
    };
    CHECK(group_of(filterable, r32) == ctx->supports(sg::feature::float32_filtering));
    CHECK(group_of(filterable, rgba8));

    // Declared unfilterable, the same view binds on every device.
    bindings[0].sample_type = sg::texture_sample_type::unfilterable_float;
    CHECK(group_of(ctx->uncached.create_binding_group_layout(bindings), r32));
}

INVOCABLE_TEST("sg - a pipeline-level static sampler builds where the backend binds it and is refused elsewhere",
               (sg::context_handle const& ctx))
{
    // A known gap, which libs/graphics/shaped-graphics/docs/TODO.md records: vulkan and metal bind no bound_sampler yet.
    // They refuse one rather than build a pipeline that samples nothing, so closing the gap fails this test on purpose.
    auto const binds = ctx->backend() == sg::backend_kind::dx12 || ctx->backend() == sg::backend_kind::webgpu;
    auto const layout = ctx->uncached.try_create_pipeline_layout(sg::pipeline_layout_description{
        .static_samplers
        = {sg::bound_sampler{.binding = {.name = "point", .space = 0u, .index = 0, .type = sg::binding_type::sampler},
                             .sampler = {.min_filter = sg::sampler_filter::nearest}}},
    });
    CHECK(layout.has_value() == binds);
}
