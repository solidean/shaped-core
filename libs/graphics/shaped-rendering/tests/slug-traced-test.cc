#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <sgl_modules/slug.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/impl/slug_reference.hh>
#include <shaped-rendering/slug_atlas.hh>
#include <shaped-rendering/slug_routine.hh>
#include <shaped-rendering/slug_shape.hh>
#include <shaped-rendering/slug_traced.hh>
#include <sr_test_sgl_shaders.hh>
#include <typed-geometry/scalar/scalar.hh>

#include <memory>

using namespace cc::primitive_defines;

namespace probes = sr_test::sgl_shaders;
using path_t = probes::slug_traced_pipeline_slug_path_t;

// Slug shapes as traced geometry on a live device, every ray held to the CPU reference.
// The quads lie at z = 0 in pixel space, and each ray of a 128 × 128 grid runs along +z through its pixel's centre:
// tests/shaders/slug_traced.sgl traces inline, and slug_traced_pipeline.sgl through a pipeline whose hit group carries
// the same `slug.decide` as its any-hit.

namespace
{
constexpr auto grid = 128;

/// One shape placed with its outline origin at pixel `origin`, `scale` pixels per outline unit, y up.
struct placement
{
    tg::pos2f origin;
    f32 scale = 1.0f;
    f32 em_scale = 1.0f;
    tg::pos2f stored_origin = tg::pos2f(0, 0);
};

struct traced_fixture
{
    sr::slug_atlas atlas;
    cc::vector<sr::slug_instance> instances;
    cc::vector<placement> placements;

    void add(sr::slug_outline const& outline, tg::pos2f origin, f32 scale)
    {
        auto const ref = atlas.add(sr::compile_slug_shape(outline)).value();
        instances.push_back(
            sr::make_slug_instance(ref, origin, tg::vec2f(scale, 0), tg::vec2f(0, -scale), tg::vec4f(1, 1, 1, 1)));
        placements.push_back(
            {.origin = origin, .scale = scale, .em_scale = ref.em_scale, .stored_origin = ref.stored_origin});
    }

    /// Where pixel (x, y)'s centre lands in instance `i`'s em space.
    [[nodiscard]] tg::pos2f em_at(isize i, int x, int y) const
    {
        auto const& p = placements[i];
        auto const ox = (f32(x) + 0.5f - p.origin[0]) / p.scale - p.stored_origin[0];
        auto const oy = (p.origin[1] - (f32(y) + 0.5f)) / p.scale - p.stored_origin[1];
        return tg::pos2f(ox * p.em_scale, oy * p.em_scale);
    }

    /// Whether em point `em` lies on instance `i`'s quad.
    [[nodiscard]] bool on_quad(isize i, tg::pos2f em) const
    {
        auto const& b = instances[i].em_bounds;
        return em[0] >= b[0] && em[0] <= b[2] && em[1] >= b[1] && em[1] <= b[3];
    }

    /// Traces the grid and reads back one float4 per ray: inline through `compute`, or through `raytraced`'s tables when
    /// it is set.
    [[nodiscard]] cc::shared_async<cc::vector<tg::vec4f>> trace(sg::context_handle ctx,
                                                                sg::compute_pipeline_handle compute,
                                                                sg::raytracing_pipeline_handle raytraced = nullptr)
    {
        if (auto* const home = ctx->device_home())
            co_await cc::async_resume_on(*home);

        // The scene's instance table, as a scene would keep it: instance id 1 reads its run's first record, 0.
        // An id that is not the record's index, so a trace that took the id for the record would read past it.
        i32 const firsts_data[] = {-1, 0};
        auto const firsts = ctx->persistent.create_buffer_from_data(firsts_data, sg::buffer_usage::readonly_buffer);
        auto const hits
            = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4f>::create_defaulted(grid * grid),
                                                      sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

        auto table = sg::raytracing_shader_table_handle();
        auto hit_group_offset = u32(0);
        if (raytraced != nullptr)
        {
            auto table_desc = path_t::table_description(raytraced);
            auto const row = path_t::add_row(table_desc, path_t::hit_groups_t::slug_quads);
            table = ctx->uncached.create_raytracing_shader_table(table_desc);
            hit_group_offset = table->offset_of(row);
        }

        auto cmd = ctx->create_command_list();
        auto const records = sr::upload_slug_records(*cmd, atlas, instances);
        auto const blas = sr::build_slug_blas(*cmd, instances, raytraced != nullptr ? path_t::ray_count : 1);
        sg::tlas_instance const placed[] = {{.blas = blas,
                                             .instance_id = 1,
                                             .hit_group_offset = hit_group_offset,
                                             .cull_mode = sg::instance_cull_mode::none}};
        auto const tlas = cmd->raytracing.build_tlas(placed);

        auto const tables = ctx->transient.create_binding_group(
            *cmd, ctx->cached.acquire_binding_group_layout<sgl_modules::slug::tables>(),
            sgl_modules::slug::tables{.curves = atlas.curve_texture().as_texture_view(),
                                      .bands = atlas.band_texture().as_texture_view()});
        auto const shapes = ctx->transient.create_binding_group(
            *cmd, ctx->cached.acquire_binding_group_layout<sgl_modules::slug::shapes>(),
            sgl_modules::slug::shapes{.instances = records.as_readonly_buffer()});

        if (raytraced != nullptr)
        {
            auto const probe = ctx->transient.create_binding_group(
                *cmd, ctx->cached.acquire_binding_group_layout<probes::pipeline_probe>(),
                probes::pipeline_probe{.world = tlas->as_view(),
                                       .width = grid,
                                       .firsts = firsts.as_readonly_buffer(),
                                       .hits = hits.as_readwrite_buffer()});
            cmd->raytracing.bind_pipeline(*raytraced);
            cmd->raytracing.bind_group(0, *tables);
            cmd->raytracing.bind_group(1, *shapes);
            cmd->raytracing.bind_group(2, *probe);
            cmd->raytracing.dispatch_rays(*table, sg::raygen_index(0), grid, grid);
        }
        else
        {
            auto const probe
                = ctx->transient.create_binding_group(*cmd, ctx->cached.acquire_binding_group_layout<probes::probe>(),
                                                      probes::probe{.world = tlas->as_view(),
                                                                    .width = grid,
                                                                    .firsts = firsts.as_readonly_buffer(),
                                                                    .hits = hits.as_readwrite_buffer()});
            cmd->compute.bind_pipeline(*compute);
            cmd->compute.bind_group(0, *tables);
            cmd->compute.bind_group(1, *shapes);
            cmd->compute.bind_group(2, *probe);
            cmd->compute.dispatch_threads(grid, grid);
        }
        auto const back = cmd->download.data_from_buffer(hits);
        ctx->submit_command_list(cc::move(cmd));
        auto const got = co_await back.data();
        auto out = cc::vector<tg::vec4f>();
        for (auto const& h : got)
            out.push_back(h);
        co_return out;
    }
};

/// A rectangle with a sub-pixel offset and an even-odd ring, side by side: slug-routine-test's scene.
[[nodiscard]] std::unique_ptr<traced_fixture> make_scene()
{
    auto ring = sr::slug_outline();
    ring.fill_rule = sr::slug_fill_rule::even_odd;
    auto const add_circle = [&](f32 r)
    {
        ring.move_to(tg::pos2f(r, 0));
        for (auto i = 0; i < 8; ++i)
        {
            auto const a1 = 0.78539816f * f32(i + 1);
            auto const am = 0.78539816f * (f32(i) + 0.5f);
            auto const cr = r / tg::cos(tg::angle_f::make_from_radians(0.39269908f));
            ring.quad_to(tg::pos2f(cr * tg::cos(tg::angle_f::make_from_radians(am)),
                                   cr * tg::sin(tg::angle_f::make_from_radians(am))),
                         tg::pos2f(r * tg::cos(tg::angle_f::make_from_radians(a1)),
                                   r * tg::sin(tg::angle_f::make_from_radians(a1))));
        }
        ring.close();
    };
    add_circle(20.0f);
    add_circle(10.0f);

    auto f = std::make_unique<traced_fixture>();
    f->add(sr::slug_outline::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(40, 30))), tg::pos2f(8.3f, 60.6f), 1.0f);
    f->add(ring, tg::pos2f(90.0f, 64.0f), 1.5f);
    return f;
}

/// Checks column `column` of every ray against the CPU point test: 1 where the ray met a shape, 0 where it did not.
/// Where `checks_quad` holds, a hit also names the shape's quad in column 1 and its t in column 2.
void check_cut(traced_fixture const& f, cc::span<tg::vec4f const> got, int column, bool checks_quad)
{
    auto compared = 0;
    for (auto y = 0; y < grid; ++y)
        for (auto x = 0; x < grid; ++x)
        {
            auto want = false;
            auto want_shape = -1;
            auto is_edge = false;
            for (auto i = isize(0); i < f.instances.size(); ++i)
            {
                auto const em = f.em_at(i, x, y);
                if (!f.on_quad(i, em))
                    continue;
                // within a hundredth of a pixel of a curve, the GPU and the CPU may round to either side
                auto const per = f.placements[i].em_scale / f.placements[i].scale * 0.01f;
                auto const c = sr::impl::slug_reference_coverage(f.atlas, f.instances[i], em, tg::vec2f(per, per), false);
                is_edge = is_edge || (c > 0.0f && c < 1.0f);
                if (sr::impl::slug_reference_contains(f.atlas, f.instances[i], em))
                {
                    want = true;
                    want_shape = int(i);
                }
            }
            if (is_edge)
                continue;
            ++compared;
            auto const& h = got[y * grid + x];
            auto const where = cc::format("pixel ({}, {})", x, y);
            CHECK((h[column] == 1.0f) == want).context(where);
            if (checks_quad && want && h[column] == 1.0f)
            {
                CHECK(int(h[1]) / 2 == want_shape).context(where);
                CHECK(tg::abs(h[2] - 1.0f) < 1e-4f).context(where);
            }
        }
    CHECK(compared > grid * grid * 9 / 10);

    // and the picture is the one meant: inside the rectangle, inside the ring's band, and through its hole
    CHECK(got[45 * grid + 28][column] == 1.0f);
    CHECK(got[64 * grid + 112][column] == 1.0f);
    CHECK(got[64 * grid + 90][column] == 0.0f);
}

[[nodiscard]] bool can_trace(sg::context_handle const& ctx)
{
    return ctx->supports(sg::feature::ray_query);
}
} // namespace

ASYNC_INVOCABLE_TEST("sr::slug traced - the any-hit decision keeps exactly the rays the point test puts inside a shape",
                     (sg::context_handle const& ctx),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);
    if (!can_trace(ctx))
        SKIP("this device has no ray queries");
    auto const f = make_scene();

    auto const building = probes::slug_traced.trace_shapes.acquire_pipeline(*ctx);
    co_await cc::async_settled(building);
    REQUIRE(building->try_value() != nullptr);
    auto const got = co_await f->trace(ctx, *building->try_value());
    REQUIRE(got.size() == grid * grid);

    check_cut(*f, got, 0, true);
}

ASYNC_INVOCABLE_TEST("sr::slug traced - a pipeline's hit group cuts the same rays with slug.decide as its any-hit",
                     (sg::context_handle const& ctx),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);
    if (!ctx->supports(sg::feature::raytracing_pipeline))
        SKIP("this device has no ray-tracing pipelines");
    auto const f = make_scene();

    auto const desc = co_await probes::slug_traced_pipeline.slug_path.description(*ctx);
    auto const pipeline = co_await ctx->cached.acquire_raytracing_pipeline(desc);
    REQUIRE(pipeline != nullptr);
    auto const got = co_await f->trace(ctx, nullptr, pipeline);
    REQUIRE(got.size() == grid * grid);

    // the surface ray through its closest hit, and the occlusion ray through its own payload's any-hit alone
    check_cut(*f, got, 0, true);
    check_cut(*f, got, 3, false);
}

ASYNC_INVOCABLE_TEST("sr::slug traced - a decal's coverage at a traced hit matches the CPU reference at the same "
                     "footprint",
                     (sg::context_handle const& ctx),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);
    if (!can_trace(ctx))
        SKIP("this device has no ray queries");
    auto const f = make_scene();

    auto const building = probes::slug_traced.cover_hits.acquire_pipeline(*ctx);
    co_await cc::async_settled(building);
    REQUIRE(building->try_value() != nullptr);
    auto const got = co_await f->trace(ctx, *building->try_value());
    REQUIRE(got.size() == grid * grid);

    auto worst = 0.0f;
    auto covered = 0;
    for (auto y = 0; y < grid; ++y)
        for (auto x = 0; x < grid; ++x)
        {
            auto const& h = got[y * grid + x];
            if (h[3] != 1.0f)
                continue; // no quad here: an undilated quad leaves its edge pixels' outer halves to nothing
            // the quads never overlap, so the hit names the one shape whose quad holds the pixel
            for (auto i = isize(0); i < f->instances.size(); ++i)
            {
                auto const em = f->em_at(i, x, y);
                if (!f->on_quad(i, em))
                    continue;
                auto const per = f->placements[i].em_scale / f->placements[i].scale;
                auto const want
                    = sr::impl::slug_reference_coverage(f->atlas, f->instances[i], em, tg::vec2f(per, per), false);
                worst = cc::max(worst, tg::abs(h[0] - want));
                ++covered;
            }
        }
    CHECK(covered > 2000);
    // the em point comes from interpolated barycentrics rather than the CPU's exact division, nothing more
    CHECK(worst < 0.01f).dump("worst", worst);

    CHECK(got[45 * grid + 28][0] > 0.99f);
    CHECK(got[64 * grid + 112][0] > 0.99f);
}
