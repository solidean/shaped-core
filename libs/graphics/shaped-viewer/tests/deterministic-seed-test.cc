#include "viewer_test_env.hh"

#include <clean-core/common/time.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>

// A traced mean draws its random sequence from what the image is, never from how it got there.
// So one scene renders to the same bits in two runs whose ids, acceleration structures and restarts all differ.
// The wavefront is here too, since a pixel's samples do not depend on how frames split them (libs/graphics/shaped-viewer/docs/wavefront.md).

namespace
{
using namespace cc::primitive_defines;

constexpr auto k_size = tg::vec2i(32, 32);

/// What one run read back: the mean's bits, and the key its sequence was drawn from.
struct run_result
{
    cc::vector<tg::vec4f> pixels;
    u64 seed_key = 0;
};

/// Renders the lit Cornell box in `mode` through a manager and store of its own until the mean holds `samples`.
///
/// `disturbed` makes the run unlike any other.
/// A mesh drawn first in another view gives the box other ids and acceleration structures at other addresses.
/// And once the mean holds samples, the camera leaves for a frame and comes back, which restarts it twice.
cc::shared_async<run_result> render(sg::context& ctx, sv::render_mode mode, bool disturbed, u32 samples)
{
    auto resources = sv::gpu_resource_manager::create(ctx);

    auto v = sv::view_data{};
    v.id = sv::view_id::from_string("deterministic seed");
    v.resolution = k_size;
    v.camera = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
    v.camera.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
    auto& l = sv::ensure_scene_3d(v);
    l.settings = {.samples_per_pixel = 4, .max_bounces = 3, .mode = mode};

    auto store = sv::view_store{};

    // Drawn in a view of its own, so it holds resources the box's are minted after.
    if (disturbed)
    {
        auto other = sv::view_data{};
        other.id = sv::view_id::from_string("unrelated");
        other.resolution = k_size;
        other.camera = v.camera;
        auto const cloud = sv_test::make_triangle_cloud(8);
        sv::ensure_scene_3d(other).items.push_back(
            resources.acquire_scene_item(sv_test::as_mesh("unrelated cloud", cloud.positions, cloud.materials)));
        resources.wait_for_pending_uploads();
        auto cmd = ctx.create_command_list();
        resources.advance_to(ctx.current_epoch());
        store.begin_frame(u64(ctx.current_epoch()));
        (void)sv::view_renderer::execute(*cmd, other, resources, store);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
    }

    auto const box = sv_test::as_lit_meshes(sv_test::make_cornell_box(), 4.0f);
    l.items.push_back(resources.acquire_scene_item(box.enclosure));
    l.items.push_back(resources.acquire_scene_item(box.lamp));
    resources.wait_for_pending_uploads();

    auto const home = v.camera.position;
    auto away_frames = disturbed ? 1 : 0;
    auto const start = cc::current_time_steady_secs();
    while (true)
    {
        // The detour starts once the mean holds samples of the home camera, so it ends a run that drew some.
        auto const accumulated = store.min_accumulated_samples(v.id);
        auto const away = away_frames > 0 && accumulated > 0;
        v.camera.position = away ? home + tg::vec3d(0.3, 0, 0) : home;

        (void)ctx.routines.tick();
        auto cmd = ctx.create_command_list();
        resources.advance_to(ctx.current_epoch());
        store.begin_frame(u64(ctx.current_epoch()));
        auto const image = sv::view_renderer::execute(*cmd, v, resources, store);
        auto readback = sg::data_future<tg::vec4f>();
        auto const done = !away && away_frames == 0 && store.min_accumulated_samples(v.id) >= samples;
        if (done)
            readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(image.raw()));
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
        if (away)
            --away_frames;

        if (done)
        {
            auto const pixels = readback.try_get_data();
            REQUIRE(pixels.has_value());
            auto out = run_result{.seed_key = store.history_of(v.id).seed_key};
            for (auto const& p : pixels.value())
                out.pixels.push_back(p);
            (void)co_await cc::async_settled(sv::background_work(ctx));
            co_return out;
        }
        // A cold compile of every kernel and every permutation takes an unoptimized build a minute or more.
        REQUIRE(cc::current_time_steady_secs() - start < 240.0);
        sv_test::drive_ambient_work();
    }
}

[[nodiscard]] cc::string_view name_of(sv::render_mode mode)
{
    switch (mode)
    {
    case sv::render_mode::megakernel:
        return "megakernel";
    case sv::render_mode::hybrid:
        return "hybrid";
    default:
        return "wavefront";
    }
}
} // namespace

// issue: bug-sv-deterministic-seed-hybrid-differs, disabled until it is fixed; run it by its name
ASYNC_INVOCABLE_TEST("sv - a traced mean is the same bits whatever ids and restarts its run went through",
                     (sg::context_handle const& ctx_h),
                     nx::config::disabled)
{
    auto& ctx = *ctx_h;
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler for this backend to build the path-tracing shaders");
    if (!ctx.supports(sg::feature::raytracing_pipeline))
        SKIP("device reports no ray tracing support");

    for (auto const mode : {sv::render_mode::megakernel, sv::render_mode::hybrid, sv::render_mode::wavefront})
    {
        if (mode == sv::render_mode::wavefront && !ctx.supports(sg::feature::ray_query))
            continue;
        auto const plain = co_await render(ctx, mode, false, 16);
        auto const disturbed = co_await render(ctx, mode, true, 16);
        CHECK(plain.seed_key == disturbed.seed_key).context(name_of(mode));
        REQUIRE(plain.pixels.size() == disturbed.pixels.size());

        auto differing = 0;
        auto lit = 0;
        for (auto i = isize(0); i < plain.pixels.size(); ++i)
        {
            for (auto c = 0; c < 4; ++c)
                if (cc::bit_cast<u32>(plain.pixels[i][c]) != cc::bit_cast<u32>(disturbed.pixels[i][c]))
                    ++differing;
            if (plain.pixels[i][0] > 0.0f)
                ++lit;
        }
        CHECK(lit > 0).context(name_of(mode)); // an image of nothing would be equal for nothing
        CHECK(differing == 0).context(cc::format("{}: {} channels differ", name_of(mode), differing));
    }
}
