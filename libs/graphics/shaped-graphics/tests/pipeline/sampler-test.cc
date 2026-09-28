#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/texture.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// Every field of sg::sampler a probe can observe, swept through a sampler the group binds (sampling.sgl).
// anisotropy and the LOD bias are left out: neither changes an explicit-level sample on every backend alike.

namespace
{
// Level 0 is 4 × 4, red by column and green by row: 51, 102, 153, 204, which unorm reads as 0.2, 0.4, 0.6, 0.8.
// Level 1 is 2 × 2, red by column and green by row: 20 and 240.
constexpr u8 level0_steps[] = {51, 102, 153, 204};
constexpr u8 level1_steps[] = {20, 240};

constexpr float unorm(u8 v)
{
    return float(v) / 255.0f;
}

struct probe
{
    sg::sampler sampler;
    tg::vec4f at; // u, v, level
    float red;
    float green; // < 0 where the probe does not read green
    cc::string_view what;
};

sg::sampler point_sampler(sg::sampler_address_mode u, sg::sampler_address_mode v)
{
    return {.min_filter = sg::sampler_filter::nearest,
            .mag_filter = sg::sampler_filter::nearest,
            .mip_filter = sg::sampler_filter::nearest,
            .address_u = u,
            .address_v = v};
}

bool near(float a, float b)
{
    return a - b < 0.01f && b - a < 0.01f;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - each sampler address mode, filter and level clamp reads its own texel",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    using address = sg::sampler_address_mode;
    using filter = sg::sampler_filter;
    auto const clamp = address::clamp_edge;
    auto probes = cc::vector<probe>();

    // Outside 0..1, where repeat, mirror and clamp each land on another texel's centre: u = 1.375 is 0.375, 0.625 and
    // 1 folded back, and u = -0.375 is 0.625, 0.375 and 0.
    for (auto const [u, expected] :
         {cc::pair{1.375f, tg::vec3f(0.4f, 0.6f, 0.8f)}, cc::pair{-0.375f, tg::vec3f(0.6f, 0.4f, 0.2f)}})
    {
        probes.push_back(
            {point_sampler(address::repeat, clamp), tg::vec4f(u, 0.625f, 0, 0), expected[0], 0.6f, "address_u repeat"});
        probes.push_back({point_sampler(address::mirror_repeat, clamp), tg::vec4f(u, 0.625f, 0, 0), expected[1], 0.6f,
                          "address_u mirror"});
        probes.push_back({point_sampler(clamp, clamp), tg::vec4f(u, 0.625f, 0, 0), expected[2], 0.6f, "address_u clamp"});
        probes.push_back(
            {point_sampler(clamp, address::repeat), tg::vec4f(0.625f, u, 0, 0), 0.6f, expected[0], "address_v repeat"});
        probes.push_back({point_sampler(clamp, address::mirror_repeat), tg::vec4f(0.625f, u, 0, 0), 0.6f, expected[1],
                          "address_v mirror"});
        probes.push_back({point_sampler(clamp, clamp), tg::vec4f(0.625f, u, 0, 0), 0.6f, expected[2], "address_v clamp"});
    }

    // Level 0 is magnified: u = 0.3125 is a quarter texel left of texel 1's centre, 0.4 nearest and 0.35 filtered.
    // The minification filter is set the other way each time, so a backend that swapped the two fails.
    auto magnified = point_sampler(clamp, clamp);
    magnified.mag_filter = filter::linear;
    probes.push_back({magnified, tg::vec4f(0.3125f, 0.625f, 0, 0), 0.35f, 0.6f, "mag_filter linear"});
    auto magnified_point = point_sampler(clamp, clamp);
    magnified_point.min_filter = filter::linear;
    probes.push_back({magnified_point, tg::vec4f(0.3125f, 0.625f, 0, 0), 0.4f, 0.6f, "mag_filter nearest"});

    // Level 1 is minified: u = 0.375 is a quarter texel right of its texel 0's centre, 20 nearest and 75 filtered.
    auto minified = point_sampler(clamp, clamp);
    minified.min_filter = filter::linear;
    probes.push_back({minified, tg::vec4f(0.375f, 0.25f, 1, 0), unorm(75), unorm(20), "min_filter linear"});
    auto minified_point = point_sampler(clamp, clamp);
    minified_point.mag_filter = filter::linear;
    probes.push_back({minified_point, tg::vec4f(0.375f, 0.25f, 1, 0), unorm(20), unorm(20), "min_filter nearest"});

    // Level 0.25 is level 0 to the nearest mip, and a quarter of the way to level 1 filtered.
    probes.push_back({point_sampler(clamp, clamp), tg::vec4f(0.125f, 0.125f, 0.25f, 0), 0.2f, 0.2f, "mip_filter nearest"});
    auto mip_linear = point_sampler(clamp, clamp);
    mip_linear.mip_filter = filter::linear;
    auto const quarter = 0.75f * 0.2f + 0.25f * unorm(20);
    probes.push_back({mip_linear, tg::vec4f(0.125f, 0.125f, 0.25f, 0), quarter, quarter, "mip_filter linear"});

    // The level clamps move an explicit level: 0 up to 1, and 1 down to 0.
    auto clamped_up = point_sampler(clamp, clamp);
    clamped_up.min_lod = 1.0f;
    probes.push_back({clamped_up, tg::vec4f(0.125f, 0.125f, 0, 0), unorm(20), unorm(20), "min_lod"});
    auto clamped_down = point_sampler(clamp, clamp);
    clamped_down.max_lod = 0.0f;
    probes.push_back({clamped_down, tg::vec4f(0.125f, 0.125f, 1, 0), 0.2f, 0.2f, "max_lod"});

    auto const pipeline = co_await shaders::sampling.sample_at.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::probe>();

    auto const source
        = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                             .width = 4,
                                             .height = 4,
                                             .mip_levels = 2,
                                             .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
    auto level0 = cc::vector<u8>();
    for (auto y = 0; y < 4; ++y)
        for (auto x = 0; x < 4; ++x)
        {
            u8 const texel[] = {level0_steps[x], level0_steps[y], 0, 255};
            level0.push_back_range(texel);
        }
    auto level1 = cc::vector<u8>();
    for (auto y = 0; y < 2; ++y)
        for (auto x = 0; x < 2; ++x)
        {
            u8 const texel[] = {level1_steps[x], level1_steps[y], 0, 255};
            level1.push_back_range(texel);
        }

    auto at = cc::vector<tg::vec4f>();
    for (auto const& p : probes)
        at.push_back(p.at);
    auto const coordinates = ctx->persistent.create_buffer_from_data(cc::move(at), sg::buffer_usage::readonly_buffer);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(source.raw(), cc::as_bytes(cc::span<u8 const>(level0)), {.mip_level = 0});
    cmd->upload.bytes_to_texture(source.raw(), cc::as_bytes(cc::span<u8 const>(level1)), {.mip_level = 1});
    // One dispatch per probe, each through its own group: the sampler is what differs between them.
    auto outputs = cc::vector<sg::buffer<tg::vec4f>>();
    auto futures = cc::vector<sg::data_future<tg::vec4f>>();
    cmd->compute.bind_pipeline(*pipeline);
    for (auto i = 0; i < probes.size(); ++i)
    {
        outputs.push_back(
            ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4f>::create_defaulted(probes.size()),
                                                    sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src));
        auto const group
            = ctx->transient.create_binding_group(*cmd, layout,
                                                  shaders::probe{.source = source.as_texture_view(),
                                                                 .smp = probes[i].sampler,
                                                                 .at = coordinates.as_readonly_buffer(),
                                                                 .values = outputs.back().as_readwrite_buffer()});
        cmd->compute.bind_group(0, *group);
        cmd->compute.dispatch_threads(int(probes.size()));
        futures.push_back(cmd->download.data_from_buffer(outputs.back()));
    }
    ctx->submit_command_list(cc::move(cmd));

    for (auto i = 0; i < probes.size(); ++i)
    {
        auto const got = co_await futures[i].data();
        auto const& p = probes[i];
        CHECK(near(got[i][0], p.red)).context(cc::format("{}: red {} against {}", p.what, got[i][0], p.red));
        if (p.green >= 0)
            CHECK(near(got[i][1], p.green)).context(cc::format("{}: green {} against {}", p.what, got[i][1], p.green));
    }
}

ASYNC_INVOCABLE_TEST("sg - a comparison sampler passes each compare_op's own references", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // A 1 × 1 depth texture cleared to 0.5, compared against 0.25, 0.5 and 0.75 by every op.
    // The comparison is `reference op texel` on every backend, so each op passes a different set of the three.
    constexpr sg::compare_op ops[] = {sg::compare_op::never,         sg::compare_op::less,    sg::compare_op::equal,
                                      sg::compare_op::less_equal,    sg::compare_op::greater, sg::compare_op::not_equal,
                                      sg::compare_op::greater_equal, sg::compare_op::always};
    constexpr float references[] = {0.25f, 0.5f, 0.75f};
    auto const passes = [](sg::compare_op op, float r)
    {
        switch (op)
        {
        case sg::compare_op::never:
            return false;
        case sg::compare_op::less:
            return r < 0.5f;
        case sg::compare_op::equal:
            return r == 0.5f;
        case sg::compare_op::less_equal:
            return r <= 0.5f;
        case sg::compare_op::greater:
            return r > 0.5f;
        case sg::compare_op::not_equal:
            return r != 0.5f;
        case sg::compare_op::greater_equal:
            return r >= 0.5f;
        case sg::compare_op::always:
            return true;
        }
        return false;
    };

    auto const pipeline = co_await shaders::sampling.compare_at.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::compared>();
    auto const depth
        = ctx->persistent.create_texture_2d({.format = sg::pixel_format::depth32_float,
                                             .width = 1,
                                             .height = 1,
                                             .usage = sg::texture_usage::depth_stencil | sg::texture_usage::texture});
    auto const refs = ctx->persistent.create_buffer_from_data(
        cc::vector<float>{references[0], references[1], references[2]}, sg::buffer_usage::readonly_buffer);

    auto cmd = ctx->create_command_list();
    {
        // An empty scope whose only effect is the clear.
        auto scope = cmd->raster.render_to({.depth_stencil_target = depth.as_depth_stencil_view().cleared(0.5f)});
    }
    auto futures = cc::vector<sg::data_future<float>>();
    auto outputs = cc::vector<sg::buffer<float>>();
    cmd->compute.bind_pipeline(*pipeline);
    for (auto const op : ops)
    {
        outputs.push_back(ctx->persistent.create_buffer_from_data(
            cc::vector<float>::create_filled(3, -1.0f), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src));
        auto const group = ctx->transient.create_binding_group(
            *cmd, layout,
            shaders::compared{.depth = depth.as_texture_view(),
                              .smp = sg::sampler{.min_filter = sg::sampler_filter::nearest,
                                                 .mag_filter = sg::sampler_filter::nearest,
                                                 .mip_filter = sg::sampler_filter::nearest,
                                                 .compare = op},
                              .references = refs.as_readonly_buffer(),
                              .values = outputs.back().as_readwrite_buffer()});
        cmd->compute.bind_group(0, *group);
        cmd->compute.dispatch_threads(3);
        futures.push_back(cmd->download.data_from_buffer(outputs.back()));
    }
    ctx->submit_command_list(cc::move(cmd));

    for (auto o = 0; o < 8; ++o)
    {
        auto const got = co_await futures[o].data();
        for (auto r = 0; r < 3; ++r)
            CHECK(got[r] == (passes(ops[o], references[r]) ? 1.0f : 0.0f))
                .context(cc::format("compare_op #{} against reference {}", o, references[r]));
    }
}
