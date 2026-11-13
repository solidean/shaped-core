#include "viewer_test_env.hh"

#include <babel-serializer/image/image.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/time.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-viewer/all.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

using namespace cc::primitive_defines;

// A final render as a user asks for one: a 20 second clip at 30 fps, orbiting a real asset, under a budget of minutes.
//
// The asset is `SV_FINAL_RENDER_MODEL` (a .glb, .gltf, .obj or .fbx), and the budget `SV_FINAL_RENDER_BUDGET` seconds, 120 by default.
// It reports how long the clip took and how far each frame's image is from a reference of the clip's first frame, rendered alone
// for as long as the whole clip had.
// A benchmark rather than a test: it takes minutes by design, and asserts only that the clip kept to its budget.

namespace
{
constexpr auto k_size = tg::vec2i(1280, 720);
constexpr double k_clip_seconds = 20.0;
constexpr double k_fps = 30.0;

[[nodiscard]] cc::result<sg::context_handle> tracing_context()
{
    auto created = sg::create_dx12_context({.adapter = sg::backend::dx12::dx12_adapter::hardware});
    if (created.has_error())
        return cc::error("no dx12 hardware device");
    if (!sv_test::shared_env().has_compiler)
        return cc::error("no SGL compiler that reaches DXIL to build the shaders");
    if (!created.value()->supports(sg::feature::raytracing_pipeline))
        return cc::error("no ray tracing on this device");
    CC_LOG_INFO("adapter: {}", created.value()->metrics.adapter().name);
    return cc::move(created.value());
}

/// What one capture run left: the wall time it took, and the seconds each written frame took.
struct run_result
{
    double seconds = 0.0;
    cc::vector<double> frame_seconds;
};

/// Captures `frames` frames of `asset` orbiting at `fps`, under `budget` seconds, into `pattern`.
/// A budget of 0 accumulates `accumulate` samples instead, and `settings` where given replaces the layer's, so a run can
/// hold the final render's choices against fixed ones.
run_result capture(sg::context& ctx,
                   sv::asset_data const& asset,
                   cc::string const& pattern,
                   i32 frames,
                   double budget,
                   cc::optional<sv::render_settings> const& settings = {},
                   u32 accumulate = 256)
{
    auto const box = asset.bounds().value();
    auto const center = tg::pos3d(0.5 * (f64(box.min[0]) + f64(box.max[0])), 0.5 * (f64(box.min[1]) + f64(box.max[1])),
                                  0.5 * (f64(box.min[2]) + f64(box.max[2])));
    auto const radius = 0.5 * f64((box.max - box.min).length());

    auto const request = sr::capture_request{.active = true,
                                             .output_path = pattern,
                                             .size = k_size,
                                             .accumulate = accumulate,
                                             .timeout_seconds = budget > 0.0 ? 60.0 : 600.0,
                                             .budget_seconds = budget,
                                             .frames = frames,
                                             .fps = k_fps};

    auto result = run_result{};
    auto const start = cc::current_time_steady_secs();
    auto frame_start = start;
    auto shown = -1.0;
    for (auto f : sv::interactive(ctx, "sv-bench/final-render", {}, request))
    {
        // a new instant on the request's clock is a new frame of the clip
        if (f.seconds() != shown)
        {
            auto const now = cc::current_time_steady_secs();
            if (shown >= 0.0)
                result.frame_seconds.push_back(now - frame_start);
            frame_start = now;
            shown = f.seconds();
        }

        // once around in the clip's 20 seconds, a little above the asset
        auto view = f.window().view();
        auto const azimuth = tg::angle_d::make_from_degree(360.0 * f.seconds() / k_clip_seconds);
        view.camera(sv::camera::orbiting(center, 2.2 * radius, azimuth, tg::angle_d::make_from_degree(20.0)));
        auto scene = view.add_scene();
        for (auto const& mesh : asset.meshes)
            scene.add_mesh(mesh);
        if (settings.has_value())
            scene.settings(settings.value());
        scene.studio();
    }
    result.frame_seconds.push_back(cc::current_time_steady_secs() - frame_start);
    result.seconds = cc::current_time_steady_secs() - start;
    return result;
}

/// The mean absolute difference of two images of one size, in 8-bit steps over every channel; negative where either is missing.
[[nodiscard]] double image_difference(cc::string const& a_path, cc::string const& b_path)
{
    auto opened_a = cc::file_read_stream_adapter::open(a_path);
    auto opened_b = cc::file_read_stream_adapter::open(b_path);
    if (!opened_a.has_value() || !opened_b.has_value())
        return -1.0;
    cc::read_stream in_a = opened_a.value();
    cc::read_stream in_b = opened_b.value();
    auto const a = babel::image::read(in_a);
    auto const b = babel::image::read(in_b);
    if (!a.has_value() || !b.has_value() || a.value().pixels.size() != b.value().pixels.size())
        return -1.0;
    auto total = 0.0;
    for (auto i = isize(0); i < a.value().pixels.size(); ++i)
        total += tg::abs(f64(a.value().pixels[i]) - f64(b.value().pixels[i]));
    return total / f64(a.value().pixels.size());
}
} // namespace

BENCHMARK("sv - a 20 second clip of a real asset rendered within a budget of minutes")
{
    auto const model = cc::environment_variable("SV_FINAL_RENDER_MODEL");
    if (!model.has_value())
        SKIP("set SV_FINAL_RENDER_MODEL to an asset to render, a .glb for instance");
    auto budget = 120.0;
    if (auto const asked = cc::environment_variable("SV_FINAL_RENDER_BUDGET"); asked.has_value())
        (void)cc::from_string(asked.value(), budget);

    auto ctx = tracing_context();
    if (ctx.has_error())
        SKIP(ctx.error().to_string());
    auto const loaded = sv::asset_loader().load(model.value());
    REQUIRE(loaded.has_value()).context(model.value());
    auto const& asset = loaded.value();
    REQUIRE(asset.bounds().has_value());

    // `SV_FINAL_RENDER_PROBE=a,b,...` renders one frame per step instead of the clip, every step a viewer of its own on the one context.
    // A step is `final` (the budgeted final render, 30 s), `oidn` or `spatial` (denoised at a fixed count), `noise` (a noise target,
    // undenoised), `raw` (neither), or `raster` or `hybrid` (that mode, undenoised).
    // Every step places the same asset's meshes, so a chain also checks what one viewer leaves the next.
    if (auto const probe = cc::environment_variable("SV_FINAL_RENDER_PROBE"); probe.has_value())
    {
        auto const megakernel = sv::render_settings{.mode = sv::render_mode::megakernel, .max_bounces = 8};
        auto rest = cc::string_view(probe.value());
        auto index = 0;
        while (!rest.empty())
        {
            auto const comma = rest.find(',');
            auto const step = comma < 0 ? rest : rest.subview({.offset = 0, .size = comma});
            rest = comma < 0 ? cc::string_view() : rest.subview(comma + 1);
            auto const path = cc::temp_file_path(cc::format("sv-final-render-probe-{}", index++), "_%05d.png");
            auto settings = megakernel;
            if (step == "oidn" || step == "spatial")
            {
                settings.reconstruct = {.denoiser = sr::denoise_method::automatic};
                settings.still_denoise = step == "oidn" ? sv::still_denoise::oidn : sv::still_denoise::spatial;
            }
            if (step == "noise")
                settings.noise_target = 0.004f;
            if (step == "raster")
                settings.mode = sv::render_mode::raster;
            if (step == "hybrid")
                settings.mode = sv::render_mode::hybrid;

            if (step == "final")
                (void)capture(*ctx.value(), asset, path, 1, 30.0);
            else
                (void)capture(*ctx.value(), asset, path, 1, 0.0, settings, step == "noise" ? 16384u : 256u);
            CC_LOG_INFO("probe: {} wrote {}", step, sr::capture_frame_path(path, 0));
        }
        return;
    }

    auto const frames = i32(k_clip_seconds * k_fps);
    auto const pattern = cc::temp_file_path("sv-final-render-clip", "_%05d.png");
    auto const clip = capture(*ctx.value(), asset, pattern, frames, budget);

    auto sorted = clip.frame_seconds;
    for (auto i = isize(1); i < sorted.size(); ++i)
        for (auto j = i; j > 0 && sorted[j] < sorted[j - 1]; --j)
            cc::swap(sorted[j], sorted[j - 1]);
    CC_LOG_INFO(
        "a {} s clip, {} frames at {}x{}, under a {} s budget: took {} s; a frame took {} s at the median, {} s at "
        "the most",
        k_clip_seconds, frames, k_size[0], k_size[1], budget, clip.seconds, sorted[sorted.size() / 2], sorted.back());

    // The clip's first frame, alone, for the whole budget: what the budget would buy one frame, as its reference.
    auto const reference_path = cc::temp_file_path("sv-final-render-reference", "_%05d.png");
    auto const reference = capture(*ctx.value(), asset, reference_path, 1, budget);
    auto const first = image_difference(sr::capture_frame_path(pattern, 0), sr::capture_frame_path(reference_path, 0));
    CC_LOG_INFO("the clip's first frame against one rendered alone for {} s: {} 8-bit steps on average",
                reference.seconds, first);

    // a frame's share spent still finishes it with one denoise, so the clip may pass its budget by about that much
    CHECK(clip.seconds < budget * 1.1 + 10.0);
    CHECK(first >= 0.0);
    CC_LOG_INFO("frames left in {}", pattern);
}
