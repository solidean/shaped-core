#include "viewer_test_env.hh"

#include <babel-serializer/image/image.hh>
#include <clean-core/common/time.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context, dx12_config::withhold_raytracing
#include <shaped-viewer/all.hh>

// A raster layer on a device without ray tracing.
//
// Ray tracing is sv's first choice and never its requirement, so a raster layer draws wherever raster pipelines do.
// These tests bring up a dx12 context that withholds ray tracing, which is how a host whose GPU has it reaches the device that lacks it.
// On that context `build_blas` and `build_tlas` break their precondition, so a raster frame that built either would assert rather than pass.
// The context's stats say the same thing without relying on an assertion: no acceleration structure was built and no ray dispatched.

namespace
{
using namespace cc::primitive_defines;

/// A dx12 context that reports neither `ray_query` nor `raytracing_pipeline`, or an error where there is no dx12 device.
[[nodiscard]] cc::result<sg::context_handle> create_context_without_raytracing()
{
    // The debug layer has to be active before the process's first dx12 device, and this test may race dx12-entry.cc's
    // drivers to it, so it asks for the layer exactly as they do.
    return sg::create_dx12_context({.activate_global_debug_layer = true,
                                    .adapter = sg::backend::dx12::dx12_adapter::hardware_or_warp,
                                    .withhold_raytracing = true});
}

/// The Cornell box seen straight through its open side.
[[nodiscard]] sv::camera cornell_camera()
{
    auto cam = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
    cam.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
    return cam;
}

/// What the context recorded that only a trace records.
struct traced_work
{
    i64 blas_builds = 0;
    i64 tlas_builds = 0;
    i64 ray_dispatches = 0;
};

[[nodiscard]] traced_work traced_work_since(sg::context const& ctx, sg::stats const& before)
{
    auto const d = ctx.metrics.stats() - before;
    return {.blas_builds = d[sg::stat::blas_builds],
            .tlas_builds = d[sg::stat::tlas_builds],
            .ray_dispatches = d[sg::stat::ray_dispatches]};
}
} // namespace

// A raster layer's capture, end to end, on the context: the viewer comes up, the image is the Cornell box's, and it is final.
//
// The capture asks for `accumulation_frame_cap` frames, which only a converged layer reaches, so a file at all means the layer converged.
// The capture protocol is process environment, which is why this carries capture-environment as dx12-entry.cc's drivers do.
ASYNC_TEST("sv dx12 - a raster layer draws on a device without ray tracing", nx::config::exclusive("capture-environment"))
{
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler, so the viewer's shaders do not build");
    auto created = create_context_without_raytracing();
    if (created.has_error())
        SKIP("no dx12 device");
    auto& ctx = *created.value();
    REQUIRE(!ctx.supports(sg::feature::raytracing_pipeline));
    REQUIRE(!ctx.supports(sg::feature::ray_query));

    constexpr auto size = tg::vec2i(96, 64);
    auto const path = cc::format("{}/sv-raster-without-raytracing.png", cc::temp_directory_path());
    auto const on = cc::scoped_environment_variable(sr::capture_request_env_var, "1");
    auto const out = cc::scoped_environment_variable(sr::capture_output_env_var, path);
    auto const which = cc::scoped_environment_variable(sr::capture_name_env_var, "front");
    auto const dim = cc::scoped_environment_variable(sr::capture_size_env_var, cc::format("{}x{}", size[0], size[1]));
    auto const acc
        = cc::scoped_environment_variable(sr::capture_accumulate_env_var, cc::format("{}", sv::accumulation_frame_cap));
    auto const lim = cc::scoped_environment_variable(sr::capture_timeout_env_var, "40");

    auto const box = sv_test::make_cornell_box();
    auto const mesh = sv_test::as_mesh("cornell box", box.positions, box.materials);

    auto const before = ctx.metrics.stats();
    auto const start = cc::current_time_steady_secs();
    for (auto f : sv::interactive(ctx, "sv-test/raster-without-raytracing"))
    {
        auto view = f.window().view();
        view.camera(cornell_camera());
        f.register_capture("front", [](sv::capture_context const&) {});
        auto scene = view.add_scene();
        scene.add_mesh(mesh);
        scene.add_rect_light("key", tg::pos3f(0, 0.99f, 0), tg::vec3f(0.35f, 0, 0), tg::vec3f(0, 0, 0.35f)).nits(40);
        scene.settings({.mode = sv::render_mode::raster});
        REQUIRE(cc::current_time_steady_secs() - start < 35.0);
    }
    co_await cc::async_settled(sv::background_work(ctx));

    auto const work = traced_work_since(ctx, before);
    CHECK(work.blas_builds == 0);
    CHECK(work.tlas_builds == 0);
    CHECK(work.ray_dispatches == 0);

    {
        auto reread = cc::file_read_stream_adapter::open(path);
        REQUIRE(reread.has_value());
        cc::read_stream in = reread.value();
        auto const decoded = babel::image::read(in);
        REQUIRE(decoded.has_value());
        auto const& img = decoded.value();
        REQUIRE(img.width == size[0]);
        REQUIRE(img.height == size[1]);

        // The left wall red and the right one green, as the tracer draws them.
        auto const at = [&](int x, int y, int c) { return int(img.pixels[(y * img.width + x) * img.channels + c]); };
        CHECK(at(22, 32, 0) > at(22, 32, 1) + 20);
        CHECK(at(74, 32, 1) > at(74, 32, 0) + 20);
    }
    cc::remove_file(path);
    CHECK(!ctx.is_device_lost());
}

// Beside it, a traced layer on the same device: it cannot be drawn, so it is declined, and it says so exactly once.
//
// The raster view converging is what shows the renderer was up and reached both layers; a few frames more show the error is not repeated.
ASYNC_TEST("sv dx12 - a traced layer on a device without ray tracing is declined, and says so once")
{
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler, so the viewer's shaders do not build");
    auto created = create_context_without_raytracing();
    if (created.has_error())
        SKIP("no dx12 device");
    auto& ctx = *created.value();

    nx::expect_error("on a device without sg::feature::raytracing_pipeline", nx::exactly(1));

    auto v_r = sv::viewer::try_create(ctx, "sv-test/traced-without-raytracing",
                                      {.width = 96, .height = 48, .headless = true});
    REQUIRE(v_r.has_value());
    auto viewer = cc::move(v_r.value());

    auto const box = sv_test::make_cornell_box();
    auto const mesh = sv_test::as_mesh("cornell box", box.positions, box.materials);

    auto const before = ctx.metrics.stats();
    auto frames_after_converged = 0;
    auto traced_frames = u32(0);
    auto traced_converged = false;
    auto const start = cc::current_time_steady_secs();
    for (auto f : viewer.frames())
    {
        auto columns = f.window().view().layout_columns();
        auto const place = [&](sv::view_ref view, sv::render_mode mode)
        {
            view.camera(cornell_camera());
            auto scene = view.add_scene();
            scene.add_mesh(mesh);
            scene.add_rect_light("key", tg::pos3f(0, 0.99f, 0), tg::vec3f(0.35f, 0, 0), tg::vec3f(0, 0, 0.35f)).nits(40);
            scene.settings({.mode = mode});
        };
        auto rastered = columns.add_view("rastered");
        auto traced = columns.add_view("traced");
        place(rastered, sv::render_mode::raster);
        place(traced, sv::render_mode::megakernel);

        traced_frames = cc::max(traced_frames, traced.accumulated_frames());
        traced_converged = traced_converged || traced.is_accumulation_converged();
        if (rastered.is_accumulation_converged() && ++frames_after_converged > 3)
            viewer.request_close();
        REQUIRE(cc::current_time_steady_secs() - start < 35.0);
    }
    co_await cc::async_settled(sv::background_work(ctx));

    CHECK(traced_frames == 0);
    CHECK(!traced_converged);
    auto const work = traced_work_since(ctx, before);
    CHECK(work.blas_builds == 0);
    CHECK(work.tlas_builds == 0);
    CHECK(work.ray_dispatches == 0);
    CHECK(!ctx.is_device_lost());
}
