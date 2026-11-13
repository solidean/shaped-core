#include "raster_test_support.hh"
#include "viewer_test_env.hh"

#include <babel-serializer/image/image.hh>
#include <clean-core/common/time.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/record/log.hh> // CC_LOG_INFO
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

using namespace cc::primitive_defines;

// A megakernel layer under a noise target: each pixel keeps its own count and the variance of its frames, and stops once its
// mean's error, as the display shows it, is down to the target.
// That is what a time-budgeted final render spends its time through: the samples go where the image is still noisy.

namespace
{
/// What a run left: whether the noise target ended it, the frames it took, its mean, and each pixel's (frames, M2).
struct noise_run
{
    bool noise_settled = false;
    int frames = 0;
    cc::vector<tg::vec4f> mean;
    cc::vector<tg::vec2f> stats;
};

/// The lit Cornell box traced by the megakernel, under `noise_target`, until its layer stops or `max_frames` frames.
cc::shared_async<noise_run> trace_box(sg::context& ctx,
                                      sv::gpu_resource_manager& resources,
                                      cc::span<sv::scene_item const> items,
                                      f32 noise_target,
                                      int max_frames)
{
    auto const output_size = tg::vec2i(64, 48);
    auto const id = sv::view_id::from_string("noisy");
    auto def = sv::viewer_definition{};
    {
        auto v = sv::view_data{};
        v.id = id;
        v.camera = sv_test::cornell_camera();
        auto& scene = sv::ensure_scene_3d(v);
        for (auto const& item : items)
            scene.items.push_back(item);
        scene.settings.mode = sv::render_mode::megakernel;
        scene.settings.noise_target = noise_target;
        def.views.push_back(cc::move(v));

        auto const root_node = def.nodes.add_container(sv::invalid_node);
        auto leaf = sv::layout_leaf{};
        leaf.views.push_back(sv::view_index(0));
        def.nodes.add_leaf(root_node, cc::move(leaf));
        auto root = sv::view_data{};
        root.id = sv::view_id::from_string("root");
        root.layers.push_back({.kind = sv::layer_kind::layout, .blend = sv::layer_blend::replace, .root_node = root_node});
        def.root_view = sv::view_index(def.views.size());
        def.views.push_back(cc::move(root));
    }

    auto const output = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                                          .width = output_size[0],
                                                          .height = output_size[1],
                                                          .usage = sg::texture_usage::render_target});
    auto const history = sv::view_history{};
    auto index = u64(0);
    auto const execute = [&](sv::view_store& store, sg::command_list& cmd)
    {
        resources.advance_to(ctx.current_epoch());
        store.begin_frame(u64(ctx.current_epoch()));
        auto const plan = sv::build_render_plan(def, output_size, index++, history);
        return sv::viewer_renderer::execute(cmd, def, plan, resources, store,
                                            output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
    };
    auto const slot_in = [&](sv::view_store& store) -> sv::impl::temporal_slot const*
    {
        auto const* const rec = store.peek_ptr(id);
        return rec != nullptr ? rec->temporal.get_ptr(sv::temporal_id::accumulation(0)) : nullptr;
    };

    // Until every permutation compiled, a stand-in restarts the mean every frame, which would count against the run.
    {
        auto warm = sv::view_store{};
        REQUIRE(sv_test::frames_until_executed(ctx,
                                               [&](sg::command_list& cmd)
                                               {
                                                   (void)execute(warm, cmd);
                                                   auto const* const s = slot_in(warm);
                                                   return s != nullptr && s->accum_frame >= 2
                                                            ? sg::routine_outcome::executed
                                                            : sg::routine_outcome::declined;
                                               }));
    }

    auto store = sv::view_store{};
    auto run = noise_run{};
    while ((slot_in(store) == nullptr || !slot_in(store)->is_at_target()) && run.frames < max_frames)
    {
        auto cmd = ctx.create_command_list();
        CHECK(execute(store, *cmd) == sg::routine_outcome::executed);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
        ++run.frames;
    }
    auto const& slot = *slot_in(store);
    run.noise_settled = slot.noise_settled;

    auto cmd = ctx.create_command_list();
    auto mean = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(slot.texture.raw()));
    auto stats = cc::optional<sg::data_future<tg::vec2f>>();
    if (slot.pixel_stats.raw() != nullptr)
        stats = sg::data_future<tg::vec2f>(cmd->download.bytes_from_texture(slot.pixel_stats.raw()));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();
    co_await ctx.idle_completion();
    co_await ctx.idle_completion(); // an epoch advance drains the GPU but not the readback actor
    REQUIRE(mean.try_get_data().has_value());
    for (auto const p : mean.try_get_data().value())
        run.mean.push_back(p);
    if (stats.has_value())
    {
        REQUIRE(stats.value().try_get_data().has_value());
        for (auto const p : stats.value().try_get_data().value())
            run.stats.push_back(p);
    }
    co_await cc::async_settled(sv::background_work(ctx));
    co_return run;
}

/// `c`'s luminance after a Reinhard curve, which is the scale a noise target is measured on.
[[nodiscard]] f32 displayed(tg::vec4f c)
{
    auto const l = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    return l / (1.0f + l);
}
} // namespace

ASYNC_INVOCABLE_TEST("sv - a megakernel layer under a noise target stops each pixel where its noise is down to it",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx) || !ctx.supports(sg::feature::raytracing_pipeline))
        SKIP("no ray-tracing pipeline or no SGL compiler, so the megakernel does not build");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const box = sv_test::as_lit_meshes(sv_test::make_cornell_box(), 15.0f);
    auto items = cc::vector<sv::scene_item>();
    items.push_back(resources.acquire_scene_item(box.enclosure));
    items.push_back(resources.acquire_scene_item(box.lamp));
    resources.wait_for_pending_uploads();

    constexpr auto target = 0.01f;
    auto const adaptive = co_await trace_box(ctx, resources, items, target, 2000);
    // The reference traces every pixel every frame, far past where the adaptive run stopped.
    auto const reference = co_await trace_box(ctx, resources, items, 0.0f, 600);

    CHECK(adaptive.noise_settled);
    REQUIRE(adaptive.stats.size() == adaptive.mean.size());
    auto fewest = 1e30f;
    auto most = 0.0f;
    for (auto const s : adaptive.stats)
    {
        fewest = cc::min(fewest, s[0]);
        most = cc::max(most, s[0]);
    }
    CC_LOG_INFO("noise target {}: {} frames, a pixel's own count from {} to {}", target, adaptive.frames, fewest, most);
    // every pixel took the minimum before it was judged, and the noisy ones took far more than the quiet ones
    CHECK(fewest >= 16.0f);
    CHECK(most >= 2.0f * fewest);
    // the count of pixels still tracing reads back a frame or two late, so the run takes at most that many past its last noisy pixel
    CHECK(int(most) <= adaptive.frames);
    CHECK(adaptive.frames <= int(most) + 3);

    // The image it stopped at is the reference's, to within about the error it was allowed.
    REQUIRE(reference.mean.size() == adaptive.mean.size());
    auto total = 0.0;
    auto lit = 0;
    for (auto i = isize(0); i < adaptive.mean.size(); ++i)
    {
        total += f64(tg::abs(displayed(adaptive.mean[i]) - displayed(reference.mean[i])));
        if (displayed(reference.mean[i]) > 0.02f)
            ++lit;
    }
    auto const mean_error = total / f64(adaptive.mean.size());
    CC_LOG_INFO("noise target {}: mean displayed difference to a {}-frame reference {}", target, reference.frames,
                mean_error);
    REQUIRE(lit > int(adaptive.mean.size()) / 4); // a black image would agree with anything
    CHECK(mean_error <= 2.0 * f64(target)).dump("mean displayed difference", mean_error);
}

// A final render: a sequence under a time budget, its layer left `automatic`, so the render picks the tracer, the noise target
// and the denoiser itself, and spends the budget frame by frame.
// The run must end near its budget however much a perfect image would take, and write every frame.
INVOCABLE_TEST("sv - a sequence under a time budget renders every frame within it", (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx) || !ctx.supports(sg::feature::raytracing_pipeline))
        SKIP("no ray-tracing pipeline or no SGL compiler, so the final render's tracer does not build");

    constexpr auto frames = 4;
    constexpr auto budget = 8.0;
    auto const pattern = cc::temp_file_path("sv-final-render", "_%03d.png");
    for (auto i = 0; i < frames; ++i)
        cc::remove_file(sr::capture_frame_path(pattern, i)); // leftovers from an earlier run would make the checks vacuous

    auto const request = sr::capture_request{.active = true,
                                             .output_path = pattern,
                                             .size = tg::vec2i(96, 64),
                                             .timeout_seconds = 60.0,
                                             .budget_seconds = budget,
                                             .frames = frames,
                                             .fps = 10.0};

    auto const box = sv_test::as_lit_meshes(sv_test::make_cornell_box(), 15.0f);
    auto const start = cc::current_time_steady_secs();
    auto drawn = 0;
    for (auto f : sv::interactive(ctx, "sv-test/final-render", {}, request))
    {
        // the camera turns a little each frame, so the frames differ and the history has motion to follow
        auto view = f.window().view();
        auto camera = sv_test::cornell_camera();
        camera.position = camera.position + tg::vec3d(0.2 * f.seconds(), 0, 0);
        view.camera(camera);
        auto scene = view.add_scene();
        scene.add_mesh(box.enclosure);
        scene.add_mesh(box.lamp);
        ++drawn;
        REQUIRE(cc::current_time_steady_secs() - start < 120.0); // only keeps a hang from becoming a timeout
    }
    auto const took = cc::current_time_steady_secs() - start;
    CC_LOG_INFO("a {}-frame final render under a {} s budget took {} s over {} viewer frames", frames, budget, took,
                drawn);

    // a share spent still finishes its frame with one denoise, so the run may pass its budget by about a frame's worth
    CHECK(took < budget + 4.0);
    for (auto i = 0; i < frames; ++i)
    {
        auto const path = sr::capture_frame_path(pattern, i);
        auto opened = cc::file_read_stream_adapter::open(path);
        REQUIRE(opened.has_value()).context(path);
        cc::read_stream in = opened.value();
        auto const decoded = babel::image::read(in);
        REQUIRE(decoded.has_value()).context(path);
        auto lit = 0;
        auto const& img = decoded.value();
        for (auto p = isize(0); p < isize(img.width) * img.height; ++p)
            if (int(img.pixels[p * img.channels]) + int(img.pixels[p * img.channels + 1]) > 40)
                ++lit;
        CHECK(lit > img.width * img.height / 4).context(path); // the box, lit, rather than a blank frame
        cc::remove_file(path);
    }
}
