#include "agreement_test_support.hh"

#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/reconstruct.hh>
#include <shaped-viewer/all.hh>

// A denoised stream over time: what one frame leaves in the next.
// A blue sphere floats across the lit box, a few pixels a frame, and every moving frame is held to a converged image of
// the sphere where that frame drew it.
// A filter that reuses history where the scene moved leaves the sphere's color behind it, which is a ghost.
// Then the sphere holds still, and once the members have handed over, the denoised image may change from frame to frame
// no more than the mean it denoises does.

namespace
{
using namespace cc::primitive_defines;
namespace agreement = sv_test::agreement;

/// Where the sphere floats, back and forth across the back half of the box.
constexpr int k_positions = 8;

[[nodiscard]] tg::vec3f position_at(int index)
{
    return tg::vec3f(-0.5f + f32(index) / f32(k_positions - 1), 0.0f, 0.4f);
}

/// The position frame `f` of the move draws: forward through all of them, then back, and again.
[[nodiscard]] int index_at(int f)
{
    auto const period = 2 * (k_positions - 1);
    auto const phase = f % period;
    return phase < k_positions ? phase : period - phase;
}

/// How much bluer than gray a pixel is, relative to its brightness.
[[nodiscard]] f64 blueness(tg::vec4f const& c)
{
    auto const lum = agreement::luminance_of(c);
    return (f64(c[2]) - f64(cc::max(c[0], c[1]))) / cc::max(lum, 0.02);
}

/// Pixels whose whole 3x3 neighborhood is blue, and distinctly bluer than `reference`: the sphere's color where the sphere is not.
/// Noise moves one pixel at a time, and a ghost is a patch; a black pixel is no bluer than gray, which a dark red wall is less.
[[nodiscard]] cc::vector<bool> ghosts_of(cc::span<tg::vec4f const> image, cc::span<tg::vec4f const> reference)
{
    auto const size = agreement::k_size;
    auto out = cc::vector<bool>::create_filled(image.size(), false);
    for (auto y = 1; y < size[1] - 1; ++y)
        for (auto x = 1; x < size[0] - 1; ++x)
        {
            auto ghost = true;
            for (auto dy = -1; dy <= 1; ++dy)
                for (auto dx = -1; dx <= 1; ++dx)
                {
                    auto const i = isize(y + dy) * size[0] + (x + dx);
                    ghost = ghost && agreement::is_finite(image[i]) && blueness(image[i]) > 0.5
                         && blueness(image[i]) - blueness(reference[i]) > 0.3;
                }
            out[isize(y) * size[0] + x] = ghost;
        }
    return out;
}

[[nodiscard]] int count_of(cc::span<bool const> mask)
{
    auto n = 0;
    for (auto const m : mask)
        n += m ? 1 : 0;
    return n;
}

/// How much `image` changed since `previous`, over its mean brightness.
[[nodiscard]] f64 change_of(cc::span<tg::vec4f const> image, cc::span<tg::vec4f const> previous)
{
    auto change = 0.0;
    auto total = 0.0;
    for (auto i = isize(0); i < image.size(); ++i)
        if (agreement::is_finite(image[i]) && agreement::is_finite(previous[i]))
        {
            change += tg::abs(agreement::luminance_of(image[i]) - agreement::luminance_of(previous[i]));
            total += agreement::luminance_of(image[i]);
        }
    return change / cc::max(total, 1e-9);
}

/// How far `image` sits from `reference`, over the reference's brightness.
[[nodiscard]] f64 error_of(cc::span<tg::vec4f const> image, cc::span<tg::vec4f const> reference)
{
    auto error = 0.0;
    auto total = 0.0;
    for (auto i = isize(0); i < image.size(); ++i)
        if (agreement::is_finite(image[i]))
        {
            error += tg::abs(agreement::luminance_of(image[i]) - agreement::luminance_of(reference[i]));
            total += agreement::luminance_of(reference[i]);
        }
    return error / cc::max(total, 1e-9);
}

[[nodiscard]] cc::vector<tg::vec3f> ghost_map(cc::span<tg::vec4f const> image, cc::span<bool const> ghosts, f32 exposure)
{
    auto out = agreement::display_of(image, exposure);
    for (auto i = isize(0); i < out.size(); ++i)
        if (ghosts[i])
            out[i] = tg::vec3f(1.0f, 0.0f, 1.0f);
    return out;
}
} // namespace

ASYNC_INVOCABLE_TEST("sv temporal - a denoised stream leaves no ghost behind a moving object, and settles once still",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!agreement::can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither the tracers nor the hybrid build");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const box = agreement::make_lit_box(resources);
    auto& lib = sv_test::shared_material_library();
    auto const blue = lib.acquire(
        sv::material::create("temporal blue", lib.acquire_type(sv::builtin_material::pbr).value(),
                             {sv::material_attribute_binding::of("base_color", tg::vec3f(0.05f, 0.1f, 0.8f)),
                              sv::material_attribute_binding::of("roughness", 0.6f)}));
    auto sphere = resources.acquire_scene_item(
        sv::mesh{.name = "temporal sphere", .geometry = sv_test::sphere_geometry(0.2f, 24, 32), .material = blue});
    resources.wait_for_pending_uploads();

    auto items_at = [&](int index)
    {
        auto items = box.items;
        sphere.transform = tg::affine_transform3f::make_translation(position_at(index));
        items.push_back(sphere);
        return items;
    };

    // One converged image per position, at the path length the hybrid's one bounce corresponds to.
    auto references = cc::vector<cc::vector<tg::vec4f>>();
    for (auto p = 0; p < k_positions; ++p)
    {
        auto const items = items_at(p);
        auto const c = co_await agreement::converge(
            ctx, resources, items, box.camera,
            {.name = "temporal reference", .mode = sv::render_mode::megakernel, .max_bounces = 2}, 256);
        auto mean = cc::vector<tg::vec4f>();
        for (auto i = isize(0); i < c.first.size(); ++i)
            mean.push_back(c.mean_at(i));
        references.push_back(cc::move(mean));
    }
    auto const exposure = agreement::exposure_of(references[0]);

    // The sphere left whole at its first position, read against its last, is the ghost the measure exists to see.
    auto const planted = count_of(ghosts_of(references[0], references[k_positions - 1]));
    REQUIRE(planted > 50).dump("ghost pixels of a sphere left behind", planted);

    auto const support = sr::query_reconstruct_support(ctx);
    sr::denoise_method const members[]
        = {sr::denoise_method::atrous, sr::denoise_method::svgf, sr::denoise_method::oidn, sr::denoise_method::dlss_rr};
    constexpr int moving_frames = 3 * 2 * (k_positions - 1);
    constexpr int still_frames = 64;
    // Past the temporal member's frames and the fade after them, so what is measured is the member that owns a still image.
    constexpr int settle_after = 24;

    for (auto const member : members)
    {
        auto const name = cc::format("temporal-hybrid-{}", sr::to_string(member));
        if (!support.supports(member))
        {
            CC_LOG_INFO("{}: not run, this build or device has no {}", name, sr::to_string(member));
            continue;
        }
        auto const start = items_at(0);
        auto view = agreement::denoise_view(ctx, resources, start, box.camera,
                                            {.name = name, .mode = sv::render_mode::hybrid, .denoiser = member});
        auto const warmed = co_await view.warm_up();
        CHECK(warmed).dump("test", cc::string_view(name));
        if (!warmed)
            continue;

        auto& scene_items = sv::ensure_scene_3d(view.def.views[0]).items;
        auto worst_ghosts = 0;
        auto worst_at = -1;
        auto worst_image = cc::vector<tg::vec4f>();
        auto worst_mask = cc::vector<bool>();
        auto worst_index = 0;
        auto denoised_change = 0.0;
        auto mean_change = 0.0;
        auto settle_frames = 0;
        auto previous_denoised = cc::vector<tg::vec4f>();
        auto previous_mean = cc::vector<tg::vec4f>();
        // The still image's error against the converged one: the temporal member's last, and the worst after the hand-off.
        auto temporal_error = -1.0;
        auto temporal_image = cc::vector<tg::vec4f>();
        auto handed_error = 0.0;
        auto handed_at = -1;
        auto handed_image = cc::vector<tg::vec4f>();
        auto errors = cc::string();
        for (auto f = 0; f < moving_frames + still_frames; ++f)
        {
            auto const index = f < moving_frames ? index_at(f) : index_at(moving_frames - 1);
            scene_items.back().transform = tg::affine_transform3f::make_translation(position_at(index));
            auto const r = co_await view.frame();
            if (!r.ran || r.denoised.empty())
            {
                previous_denoised.clear();
                continue;
            }

            auto const ghosts = ghosts_of(r.denoised, references[index]);
            auto const count = count_of(ghosts);
            if (count > worst_ghosts || worst_image.empty())
            {
                worst_ghosts = cc::max(worst_ghosts, count);
                worst_at = f;
                worst_image = r.denoised;
                worst_mask = ghosts;
                worst_index = index;
            }

            auto const still = f - moving_frames;
            if (still >= 0)
            {
                auto const error = error_of(r.denoised, references[index]);
                if (still % 8 == 7)
                    errors += cc::format(" {:.1f}%", 100.0 * error);
                if (still < 16)
                {
                    temporal_error = error;
                    temporal_image = r.denoised;
                }
                if (still >= settle_after && error > handed_error)
                {
                    handed_error = error;
                    handed_at = still;
                    handed_image = r.denoised;
                }
            }

            if (still >= settle_after && !previous_denoised.empty() && !previous_mean.empty())
            {
                denoised_change += change_of(r.denoised, previous_denoised);
                mean_change += change_of(r.raw, previous_mean);
                ++settle_frames;
            }
            previous_denoised = r.denoised;
            previous_mean = r.raw;
        }
        denoised_change /= f64(cc::max(settle_frames, 1));
        mean_change /= f64(cc::max(settle_frames, 1));
        CC_LOG_INFO("{}: worst frame {} with {} ghost pixels; still, the denoised image changes {:.2f}% a frame and "
                    "its mean {:.2f}%, over {} frames",
                    name, worst_at, worst_ghosts, 100.0 * denoised_change, 100.0 * mean_change, settle_frames);
        CC_LOG_INFO("{}: still, off the converged image by{} every 8 frames; the temporal member's last {:.1f}%, the "
                    "worst "
                    "after the hand-off {:.1f}% at still frame {}",
                    name, errors, 100.0 * temporal_error, 100.0 * handed_error, handed_at);

        cc::vector<tg::vec3f> const panels[]
            = {agreement::display_of(references[worst_index], exposure), agreement::display_of(worst_image, exposure),
               ghost_map(worst_image, worst_mask, exposure), agreement::display_of(temporal_image, exposure),
               agreement::display_of(handed_image, exposure)};
        agreement::write_panels(name, panels);

        // A trail behind the sphere covers about a hundred pixels; a highlight bluer than the reference's covers a few.
        CHECK(worst_ghosts <= 20).dump("test", cc::string_view(name)).dump("frame", worst_at);
        // A filter on the mean may let its noise through, and may not add flicker of its own.
        CHECK(denoised_change <= 1.5 * mean_change + 0.002)
            .dump("test", cc::string_view(name))
            .dump("denoised change", denoised_change)
            .dump("mean change", mean_change);
        // Holding still only ever brings the image closer, the hand-off from the temporal member included.
        CHECK(handed_error <= 1.1 * temporal_error)
            .dump("test", cc::string_view(name))
            .dump("temporal error", temporal_error)
            .dump("worst error after the hand-off", handed_error)
            .dump("at still frame", handed_at);
        co_await cc::async_settled(sv::background_work(ctx));
    }
}

namespace
{
/// Holds `items`, seen through `camera` and lit by `lights` where given, still under every denoiser, and measures how
/// much each denoised image changes from frame to frame once the mean has nearly converged, against the mean's own change.
/// Each still frame samples its pixels at another offset, so the mean antialiases; a guide drawn at that frame's offset
/// changes surface behind every edge from frame to frame, and a filter steered by it flickers there however converged its input.
/// Awaited on the spot, so what it takes by reference outlives every suspend.
cc::shared_async<cc::unit> hold_still(sg::context& ctx,
                                      sv::gpu_resource_manager& resources,
                                      cc::span<sv::scene_item const> items,
                                      sv::camera const& camera,
                                      cc::span<sv::scene_light const> lights,
                                      cc::string_view prefix)
{
    auto const size = agreement::k_size;
    auto const support = sr::query_reconstruct_support(ctx);
    sr::denoise_method const members[]
        = {sr::denoise_method::atrous, sr::denoise_method::svgf, sr::denoise_method::oidn, sr::denoise_method::dlss_rr};
    // Late enough that the mean's own change is a fraction of a percent, and long enough to average.
    constexpr int measure_from = 256;
    constexpr int measure_to = 320;

    for (auto const member : members)
    {
        auto const name = cc::format("{}-{}", prefix, sr::to_string(member));
        if (!support.supports(member))
        {
            CC_LOG_INFO("{}: not run, this build or device has no {}", name, sr::to_string(member));
            continue;
        }
        auto view = agreement::denoise_view(ctx, resources, items, camera,
                                            {.name = name, .mode = sv::render_mode::hybrid, .denoiser = member});
        if (!lights.empty())
        {
            auto& scene = sv::ensure_scene_3d(view.def.views[0]);
            for (auto const& l : lights)
                scene.lights.push_back(l);
            scene.fallback_light = cc::nullopt;
        }
        auto const warmed = co_await view.warm_up();
        CHECK(warmed).dump("test", cc::string_view(name));
        if (!warmed)
            continue;

        auto denoised_change = 0.0;
        auto mean_change = 0.0;
        auto frames = 0;
        auto where = cc::vector<f64>::create_filled(isize(size[0]) * size[1], 0.0);
        auto where_mean = cc::vector<f64>::create_filled(isize(size[0]) * size[1], 0.0);
        auto previous_denoised = cc::vector<tg::vec4f>();
        auto previous_mean = cc::vector<tg::vec4f>();
        auto last = cc::vector<tg::vec4f>();
        for (auto f = 0; f < 4 * measure_to; ++f)
        {
            auto const r = co_await view.frame();
            if (!r.ran || r.denoised.empty() || r.samples < u32(measure_from))
            {
                previous_denoised.clear();
                continue;
            }
            if (r.samples >= u32(measure_to))
                break;
            if (!previous_denoised.empty())
            {
                denoised_change += change_of(r.denoised, previous_denoised);
                mean_change += change_of(r.raw, previous_mean);
                for (auto i = isize(0); i < where.size(); ++i)
                {
                    where[i] += tg::abs(agreement::luminance_of(r.denoised[i])
                                        - agreement::luminance_of(previous_denoised[i]));
                    where_mean[i]
                        += tg::abs(agreement::luminance_of(r.raw[i]) - agreement::luminance_of(previous_mean[i]));
                }
                ++frames;
            }
            previous_denoised = r.denoised;
            previous_mean = r.raw;
            last = r.denoised;
        }
        REQUIRE(frames > 8).dump("test", cc::string_view(name));
        denoised_change /= f64(frames);
        mean_change /= f64(frames);
        // An edge is a few pixels of the image, so its flicker shows in the pixels that change most rather than in the average.
        auto const tail = [&](cc::vector<f64> values)
        {
            cc::sort(values);
            return values[values.size() * 99 / 100] / f64(frames);
        };
        auto const denoised_tail = tail(where);
        auto const mean_tail = tail(where_mean);
        CC_LOG_INFO(
            "{}: from {} to {} samples, the denoised image changes {:.3f}% a frame and its mean {:.3f}%; the 1% of "
            "pixels changing most, {:.4f} and {:.4f}",
            name, measure_from, measure_to, 100.0 * denoised_change, 100.0 * mean_change, denoised_tail, mean_tail);

        // Where the denoised image changes, brightest where it changes most.
        auto peak = 1e-9;
        for (auto const w : where)
            peak = cc::max(peak, w);
        auto map = cc::vector<tg::vec3f>();
        for (auto const w : where)
            map.push_back(tg::vec3f(f32(w / peak), f32(w / peak), f32(w / peak)));
        auto const exposure = agreement::exposure_of(last);
        cc::vector<tg::vec3f> const panels[]
            = {agreement::display_of(last, exposure), agreement::display_of(previous_mean, exposure), cc::move(map)};
        agreement::write_panels(name, panels);

        CHECK(denoised_change <= 2.0 * mean_change + 0.001)
            .dump("test", cc::string_view(name))
            .dump("denoised change", denoised_change)
            .dump("mean change", mean_change);
        CHECK(denoised_tail <= 2.0 * mean_tail)
            .dump("test", cc::string_view(name))
            .dump("denoised, 99th percentile", denoised_tail)
            .dump("mean, 99th percentile", mean_tail);
        co_await cc::async_settled(sv::background_work(ctx));
    }
}
} // namespace

ASYNC_INVOCABLE_TEST("sv temporal - a still denoised image stops changing as its mean converges",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!agreement::can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither the tracers nor the hybrid build");
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const box = agreement::make_lit_box(resources);
    co_await hold_still(ctx, resources, box.items, box.camera, {}, "settled-hybrid");
}

// The scene the edge flicker was first seen in: a room of furniture edges, lit by its own lights, from its author's camera.
// It is fetched on request, so a run without it skips.
ASYNC_INVOCABLE_TEST("sv temporal - the White Room's still denoised image stops changing as its mean converges",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!agreement::can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither the tracers nor the hybrid build");
    auto const asset = sv::asset_loader().load(SV_TEST_RESEARCH_SCENES_DIR "/living-room-2/living_room_2_extended.glb");
    if (asset.has_error())
        SKIP(cc::format("the White Room did not load ({}); uv run extern/gltf-research-scenes/fetch-research-scenes.py "
                        "installs it",
                        asset.error().to_string()));
    REQUIRE(!asset.value().cameras.empty());

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto items = cc::vector<sv::scene_item>();
    for (auto const& mesh : asset.value().meshes)
        items.push_back(resources.acquire_scene_item(mesh));
    resources.wait_for_pending_uploads();
    auto lights = cc::vector<sv::scene_light>();
    for (auto const& l : asset.value().lights)
        lights.push_back({.id = sv::light_id::from_string(l.id), .light = l.light});
    co_await hold_still(ctx, resources, items, asset.value().cameras[0].camera, lights, "settled-white-room");
}

// The first still frames belong to the temporal member, which denoises each frame's own samples against its history.
// Its image must change less from frame to frame than the mean beside it does, or its history is not doing its job.
// A temporal member handed guides that describe something other than its history rejects or misweighs that history every frame.
ASYNC_INVOCABLE_TEST("sv temporal - a temporal member's young still image changes less than its mean",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!agreement::can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither the tracers nor the hybrid build");
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const box = agreement::make_lit_box(resources);
    auto const support = sr::query_reconstruct_support(ctx);

    struct young_case
    {
        sr::denoise_method member;
        sr::upscale_method upscaler;
    };
    // SVGF followed by FSR at the native scale too, which a hybrid frame's shared jitter lets run as anti-aliasing.
    young_case const cases[] = {{.member = sr::denoise_method::svgf, .upscaler = sr::upscale_method::none},
                                {.member = sr::denoise_method::svgf, .upscaler = sr::upscale_method::fsr},
                                {.member = sr::denoise_method::dlss_rr, .upscaler = sr::upscale_method::none}};
    for (auto const& c : cases)
    {
        auto const member = c.member;
        auto const name = c.upscaler == sr::upscale_method::none
                            ? cc::format("young-hybrid-{}", sr::to_string(member))
                            : cc::format("young-hybrid-{}-{}", sr::to_string(member), sr::to_string(c.upscaler));
        if (!support.supports(member) || (c.upscaler != sr::upscale_method::none && !support.supports(c.upscaler)))
        {
            CC_LOG_INFO("{}: not run, this build or device cannot", name);
            continue;
        }
        auto view = agreement::denoise_view(ctx, resources, box.items, box.camera,
                                            {.name = name, .mode = sv::render_mode::hybrid, .denoiser = member});
        sv::ensure_scene_3d(view.def.views[0]).settings.reconstruct.upscaler = c.upscaler;
        auto const warmed = co_await view.warm_up();
        CHECK(warmed).dump("test", cc::string_view(name));
        if (!warmed)
            continue;
        // A restart, so the still period starts at frame 0.
        view.def.views[0].camera.position += tg::vec3d(1e-3, 0, 0);
        (void)co_await view.frame();
        view.def.views[0].camera = box.camera;

        auto previous = cc::vector<tg::vec4f>();
        auto previous_mean = cc::vector<tg::vec4f>();
        auto denoised_change = 0.0;
        auto mean_change = 0.0;
        auto frames = 0;
        // Inside the temporal member's window, which `temporal_denoise_frames` makes 16.
        for (auto f = 0; f < 16; ++f)
        {
            auto const r = co_await view.frame();
            if (!r.ran || r.denoised.empty())
            {
                previous.clear();
                continue;
            }
            if (!previous.empty())
            {
                denoised_change += change_of(r.denoised, previous);
                mean_change += change_of(r.raw, previous_mean);
                ++frames;
            }
            previous = r.denoised;
            previous_mean = r.raw;
        }
        REQUIRE(frames > 8).dump("test", cc::string_view(name));
        denoised_change /= f64(frames);
        mean_change /= f64(frames);
        CC_LOG_INFO("{}: over the first 16 still frames, the denoised image changes {:.2f}% a frame and its mean "
                    "{:.2f}%",
                    name, 100.0 * denoised_change, 100.0 * mean_change);
        CHECK(denoised_change < 0.5 * mean_change)
            .dump("test", cc::string_view(name))
            .dump("denoised change", denoised_change)
            .dump("mean change", mean_change);
        co_await cc::async_settled(sv::background_work(ctx));
    }
}
