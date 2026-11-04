#include "agreement_test_support.hh"

#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/reconstruct.hh>
#include <shaped-viewer/all.hh>

// Every denoiser member this machine runs, behind the megakernel and behind the deferred hybrid, drawn through the viewer.
//
// A denoiser is biased by design, so it is not held to the converged image pixel by pixel.
// It is held to three things that any denoiser worth running keeps.
// It ran: a member named on a layer is the member that ran, or the raw mean is presented, which the denoised slot says.
// It keeps the brightness: its image's mean, over the whole frame and over each 16x16 tile, is the raw mean's it denoised.
// It helps: its image is closer to the converged one than the raw mean it started from, pixel by pixel.

namespace
{
using namespace cc::primitive_defines;
namespace agreement = sv_test::agreement;

using agreement::denoise_view;
using agreement::denoised_frame;

/// `items` drawn the `config` way and denoised by its member, read at `samples` per pixel.
/// The view warms up, then restarts and stops at `samples`.
cc::shared_async<denoised_frame> denoise_at(sg::context& ctx,
                                            sv::gpu_resource_manager& resources,
                                            cc::span<sv::scene_item const> items,
                                            sv::camera const& camera,
                                            agreement::mode_config const& config,
                                            u32 samples)
{
    auto view = denoise_view(ctx, resources, items, camera, config);
    if (!co_await view.warm_up())
        co_return denoised_frame{};

    // A frame a hair to the side and the camera's return restart the mean, so the reading below is at `samples` exactly.
    view.def.views[0].camera.position = camera.position + tg::vec3d(1e-3, 0, 0);
    (void)co_await view.frame();
    view.def.views[0].camera = camera;
    auto r = co_await view.frame();
    auto frames = 0;
    while (r.samples < samples)
    {
        r = co_await view.frame();
        REQUIRE(++frames < 4096).dump("mode", config.name).dump("samples", r.samples);
    }
    co_await cc::async_settled(sv::background_work(ctx));
    co_return r;
}

[[nodiscard]] f64 luminance(tg::vec4f const& c)
{
    return agreement::luminance_of(c);
}

/// What one denoised frame did to its raw mean, measured against the converged image.
struct denoise_report
{
    /// the denoised image's mean luminance over the raw mean's, less one
    f64 global_shift = 0;
    /// The largest shift of a 16x16 tile's mean, over the raw frame's mean rather than the tile's own.
    /// A filter wider than a tile carries light from a lit wall into the shadowed corner beside it, which is blur and not bias,
    /// and measured against a dark tile's own mean it reads as a large fraction of next to nothing.
    /// Signed, with where it is and what the tile holds denoised and converged.
    f64 worst_tile_shift = 0;
    tg::vec2i worst_tile = tg::vec2i(-1, -1);
    f64 worst_tile_truth = 0;
    f64 worst_tile_denoised = 0;
    /// the root mean square error against the converged image, of the raw mean and of the denoised image
    f64 raw_error = 0;
    f64 denoised_error = 0;
};

/// The lamp is 15 nits and no wall reaches 2, so a pixel above that is the lamp, whose error would outweigh every wall's.
constexpr f64 k_emitter_floor = 2.0;

[[nodiscard]] denoise_report measure(denoised_frame const& f, agreement::converged const& reference)
{
    auto const size = agreement::k_size;
    auto report = denoise_report{};
    auto raw_sum = 0.0;
    auto denoised_sum = 0.0;
    auto raw_squares = 0.0;
    auto denoised_squares = 0.0;
    auto n = 0;
    for (auto i = isize(0); i < f.raw.size(); ++i)
    {
        auto const truth = luminance(reference.mean_at(i));
        if (truth >= k_emitter_floor)
            continue;
        auto const raw = luminance(f.raw[i]);
        auto const denoised = luminance(f.denoised[i]);
        raw_sum += raw;
        denoised_sum += denoised;
        raw_squares += (raw - truth) * (raw - truth);
        denoised_squares += (denoised - truth) * (denoised - truth);
        ++n;
    }
    report.global_shift = denoised_sum / cc::max(raw_sum, 1e-9) - 1.0;
    auto const frame_mean = raw_sum / cc::max(n, 1);
    report.raw_error = tg::sqrt(raw_squares / cc::max(n, 1));
    report.denoised_error = tg::sqrt(denoised_squares / cc::max(n, 1));

    constexpr int tile = 16;
    for (auto ty = 0; ty < size[1] / tile; ++ty)
        for (auto tx = 0; tx < size[0] / tile; ++tx)
        {
            auto raw = 0.0;
            auto denoised = 0.0;
            auto truth = 0.0;
            auto lit = true;
            for (auto y = ty * tile; y < (ty + 1) * tile; ++y)
                for (auto x = tx * tile; x < (tx + 1) * tile; ++x)
                {
                    auto const i = isize(y) * size[0] + x;
                    lit = lit && luminance(reference.mean_at(i)) < k_emitter_floor;
                    truth += luminance(reference.mean_at(i));
                    raw += luminance(f.raw[i]);
                    denoised += luminance(f.denoised[i]);
                }
            // A tile holding the lamp's edge is one whose blur is mostly the lamp's.
            if (!lit)
                continue;
            auto const shift = (denoised - raw) / (f64(tile * tile) * cc::max(frame_mean, 1e-9));
            if (tg::abs(shift) > tg::abs(report.worst_tile_shift))
            {
                report.worst_tile_shift = shift;
                report.worst_tile = tg::vec2i(tx, ty);
                report.worst_tile_truth = truth / (tile * tile);
                report.worst_tile_denoised = denoised / (tile * tile);
            }
        }
    return report;
}
} // namespace

ASYNC_INVOCABLE_TEST("sv agreement - every denoiser keeps the image's brightness and brings it closer to the converged "
                     "one",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!agreement::can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither the tracers nor the hybrid build");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const box = agreement::make_lit_box(resources);
    auto const support = sr::query_reconstruct_support(ctx);

    // Every member, named; one this machine cannot run is said so and passed over.
    sr::denoise_method const members[]
        = {sr::denoise_method::atrous,  sr::denoise_method::svgf,   sr::denoise_method::oidn,
           sr::denoise_method::dlss_rr, sr::denoise_method::fsr_rr, sr::denoise_method::nrd};

    struct mode_case
    {
        cc::string_view name;
        agreement::mode_config mode;
        /// the megakernel's image the mode converges to
        agreement::mode_config reference;
    };
    mode_case const modes[] = {
        {.name = "megakernel",
         .mode = {.name = "megakernel", .mode = sv::render_mode::megakernel, .samples_per_pixel = 1},
         .reference = {.name = "megakernel", .mode = sv::render_mode::megakernel}},
        {.name = "hybrid",
         .mode = {.name = "hybrid", .mode = sv::render_mode::hybrid},
         .reference = {.name = "megakernel", .mode = sv::render_mode::megakernel, .max_bounces = 2}},
    };

    // Four samples a pixel: a young mean, which is where a denoiser earns its place, and inside the temporal members' window.
    constexpr u32 samples = 4;

    for (auto const& m : modes)
    {
        auto const reference = co_await agreement::converge(ctx, resources, box.items, box.camera, m.reference, 1024,
                                                            agreement::k_reference_restarts);
        auto reference_mean = cc::vector<tg::vec4f>();
        for (auto i = isize(0); i < reference.first.size(); ++i)
            reference_mean.push_back(reference.mean_at(i));
        auto const exposure = agreement::exposure_of(reference_mean);

        for (auto const member : members)
        {
            auto const name = cc::format("denoise-{}-{}", m.name, sr::to_string(member));
            if (!support.supports(member))
            {
                CC_LOG_INFO("{}: not run, this build or device has no {}", name, sr::to_string(member));
                continue;
            }
            auto config = m.mode;
            config.denoiser = member;
            auto const f = co_await denoise_at(ctx, resources, box.items, box.camera, config, samples);
            CHECK(f.ran).dump("test", cc::string_view(name));
            if (!f.ran)
                continue;
            REQUIRE(f.raw.size() == reference.first.size());
            REQUIRE(f.denoised.size() == reference.first.size());

            auto const r = measure(f, reference);
            CC_LOG_INFO("{}: at {} samples, brightness {:+.1f}% overall and {:+.1f}% of the frame's in the worst tile, "
                        "({}, {}), {:.3f} where it converges to {:.3f}; error against the converged image {:.4f} raw, "
                        "{:.4f} denoised ({:.0f}%)",
                        name, f.samples, 100.0 * r.global_shift, 100.0 * r.worst_tile_shift, r.worst_tile[0],
                        r.worst_tile[1], r.worst_tile_denoised, r.worst_tile_truth, r.raw_error, r.denoised_error,
                        100.0 * r.denoised_error / r.raw_error);

            cc::vector<tg::vec3f> const panels[]
                = {agreement::display_of(reference_mean, exposure), agreement::display_of(f.raw, exposure),
                   agreement::display_of(f.denoised, exposure)};
            agreement::write_panels(name, panels);

            // Every member keeps the brightness within 5% and its tiles within some 15%.
            CHECK(tg::abs(r.global_shift) < 0.05).dump("test", cc::string_view(name)).dump("shift", r.global_shift);
            // SVGF's measured 17% on the hybrid: a still hybrid frame hands it the guides blended over the mean's frames, which
            // its history needs to stay steady, and which describe a jittered frame's edges a little less closely than its own.
            auto const tile_limit = member == sr::denoise_method::svgf ? 0.18 : 0.15;
            CHECK(tg::abs(r.worst_tile_shift) < tile_limit)
                .dump("test", cc::string_view(name))
                .dump("tile shift", r.worst_tile_shift);
            CHECK(r.denoised_error < 0.7 * r.raw_error)
                .dump("test", cc::string_view(name))
                .dump("raw error", r.raw_error)
                .dump("denoised error", r.denoised_error);
        }
    }
}
