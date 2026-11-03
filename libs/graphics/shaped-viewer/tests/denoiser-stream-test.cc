#include "agreement_test_support.hh"

#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/reconstruct.hh>
#include <shaped-viewer/all.hh>

// A denoiser shown in a viewer runs on a stream, not a still.
// While the camera moves each frame restarts the mean, so a spatial member filters one frame's samples and a temporal one
// reprojects its history; once it stops, the mean grows and the members hand over.
// Every frame of that stream must be whole: no pixel that is not a number, and no block punched black.
// A single extreme sample a filter spreads, or a NaN, does that on one frame and somewhere else on the next.
// And a still image must settle: little change from one frame to the next.
//
// The box under its lowered lamp throws the strongest fireflies these tests know.
// A black sphere and a glass one are surfaces that reflect next to nothing, which the native members filter apart.
// `denoise_view` checks every frame it reads for non-finite pixels on top of what this measures.

namespace
{
using namespace cc::primitive_defines;
namespace agreement = sv_test::agreement;

/// What one frame of a denoised stream looked like.
struct stream_frame_stats
{
    /// Pixels black along with their whole 5x5 neighborhood where the converged image is lit: a hole a filter punched.
    /// A NaN spread through a filter's passes covers at least that much, where a cluster of unlit samples in a one-sample
    /// frame, which a filter leaves black, covers a few pixels.
    int holes = 0;
    f64 mean = 0;
};

[[nodiscard]] f64 luminance(tg::vec4f const& c)
{
    return agreement::luminance_of(c);
}

[[nodiscard]] stream_frame_stats stats_of(cc::span<tg::vec4f const> image, agreement::converged const& reference)
{
    auto const size = agreement::k_size;
    auto out = stream_frame_stats{};
    auto finite = 0;
    for (auto const& c : image)
        if (agreement::is_finite(c))
        {
            out.mean += luminance(c);
            ++finite;
        }
    out.mean /= f64(cc::max(finite, 1));
    for (auto y = 2; y < size[1] - 2; ++y)
        for (auto x = 2; x < size[0] - 2; ++x)
        {
            auto black = true;
            auto lit = true;
            for (auto dy = -2; dy <= 2; ++dy)
                for (auto dx = -2; dx <= 2; ++dx)
                {
                    auto const i = isize(y + dy) * size[0] + (x + dx);
                    // A pixel that is not a number shows black, so it counts toward a hole as well.
                    black = black && !(luminance(image[i]) > 1e-4);
                    lit = lit && luminance(reference.mean_at(i)) > 0.02;
                }
            out.holes += black && lit ? 1 : 0;
        }
    return out;
}
} // namespace

ASYNC_INVOCABLE_TEST("sv agreement - every denoiser keeps a moving and a settling stream whole and steady",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!agreement::can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither the tracers nor the hybrid build");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto box = agreement::make_lit_box(resources, 0.05f);
    auto& lib = sv_test::shared_material_library();
    auto const pbr = lib.acquire_type(sv::builtin_material::pbr).value();
    auto const openpbr = lib.acquire_type(sv::builtin_material::openpbr).value();
    auto const black = lib.acquire(
        sv::material::create("stream black", pbr,
                             {sv::material_attribute_binding::of("base_color", tg::vec3f(0.02f, 0.02f, 0.02f)),
                              sv::material_attribute_binding::of("roughness", 0.3f)}));
    auto const glass
        = lib.acquire(sv::material::create("stream glass", openpbr,
                                           {sv::material_attribute_binding::of("transmission_weight", 1.0f),
                                            sv::material_attribute_binding::of("specular_roughness", 0.0f)}));
    box.items.push_back(resources.acquire_scene_item(
        sv::mesh{.name = "stream black sphere",
                 .geometry = sv_test::sphere_geometry(0.28f, 24, 32, tg::vec3f(-0.4f, -0.72f, -0.6f)),
                 .material = black}));
    box.items.push_back(resources.acquire_scene_item(
        sv::mesh{.name = "stream glass sphere",
                 .geometry = sv_test::sphere_geometry(0.28f, 24, 32, tg::vec3f(0.4f, -0.72f, -0.6f)),
                 .material = glass}));
    resources.wait_for_pending_uploads();

    auto const reference
        = co_await agreement::converge(ctx, resources, box.items, box.camera,
                                       {.name = "megakernel", .mode = sv::render_mode::megakernel, .max_bounces = 2},
                                       512, agreement::k_reference_restarts);
    auto reference_mean = cc::vector<tg::vec4f>();
    for (auto i = isize(0); i < reference.first.size(); ++i)
        reference_mean.push_back(reference.mean_at(i));
    auto const exposure = agreement::exposure_of(reference_mean);

    auto const support = sr::query_reconstruct_support(ctx);
    sr::denoise_method const members[]
        = {sr::denoise_method::atrous, sr::denoise_method::svgf, sr::denoise_method::oidn, sr::denoise_method::dlss_rr};
    constexpr int moving_frames = 32;
    constexpr int still_frames = 48;

    for (auto const member : members)
    {
        auto const name = cc::format("stream-hybrid-{}", sr::to_string(member));
        if (!support.supports(member))
        {
            CC_LOG_INFO("{}: not run, this build or device has no {}", name, sr::to_string(member));
            continue;
        }
        auto view = agreement::denoise_view(ctx, resources, box.items, box.camera,
                                            {.name = name, .mode = sv::render_mode::hybrid, .denoiser = member});
        auto const warmed = co_await view.warm_up();
        CHECK(warmed).dump("test", cc::string_view(name));
        if (!warmed)
            continue;

        auto worst_holes = 0;
        auto worst_at = -1;
        auto flicker = 0.0;
        auto flicker_frames = 0;
        auto previous = cc::vector<tg::vec4f>();
        auto worst_shown = cc::vector<tg::vec4f>();
        auto still_shown = cc::vector<tg::vec4f>();
        for (auto f = 0; f < moving_frames + still_frames; ++f)
        {
            // Small sideways steps back and forth, a few pixels over the whole move, then still where the reference is.
            view.def.views[0].camera = box.camera;
            if (f < moving_frames)
                view.def.views[0].camera.position += tg::vec3d(0.004 * f64(f % 8), 0.0, 0.0);
            auto const r = co_await view.frame();
            // A frame the member did not produce shows the raw mean, which is not what is measured here.
            if (!r.ran || r.denoised.empty())
                continue;
            auto const s = stats_of(r.denoised, reference);
            if (s.holes > worst_holes || worst_shown.empty())
            {
                worst_holes = cc::max(worst_holes, s.holes);
                worst_at = f;
                worst_shown = r.denoised;
            }
            // Past the first frames after the stop, which still carry the move in their history.
            if (f >= moving_frames + 8 && !previous.empty())
            {
                auto change = 0.0;
                for (auto i = isize(0); i < previous.size(); ++i)
                    if (agreement::is_finite(r.denoised[i]) && agreement::is_finite(previous[i]))
                        change += tg::abs(luminance(r.denoised[i]) - luminance(previous[i]));
                flicker += change / (f64(previous.size()) * cc::max(s.mean, 1e-9));
                ++flicker_frames;
            }
            previous = r.denoised;
            still_shown = r.denoised;
        }
        flicker /= f64(cc::max(flicker_frames, 1));
        CC_LOG_INFO("{}: worst frame {} with {} pixels in holes; still, {:.2f}% change a frame", name, worst_at,
                    worst_holes, 100.0 * flicker);

        cc::vector<tg::vec3f> const panels[]
            = {agreement::display_of(reference_mean, exposure), agreement::display_of(worst_shown, exposure),
               agreement::display_of(still_shown, exposure)};
        agreement::write_panels(name, panels);

        CHECK(worst_holes == 0).dump("test", cc::string_view(name)).dump("frame", worst_at);
        co_await cc::async_settled(sv::background_work(ctx));
    }
}
