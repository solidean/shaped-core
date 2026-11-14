#include "agreement_test_support.hh"

#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/reconstruct.hh>
#include <shaped-viewer/all.hh>

// How an estimate approaches its answer as its samples grow, rather than where it stands at one count.
// A mean's noise falls as one over the square root of its samples: a blend weight that forgets old samples, or counts a
// sample twice, leaves two halves that still agree with their own noise, and only the rate shows it.
// What is left of the error once that noise is accounted for is bias, which must not grow with the samples.
// A denoised image must approach the converged one as its mean grows, and end no further from it than the mean itself.

namespace
{
using namespace cc::primitive_defines;
namespace agreement = sv_test::agreement;

/// What a converged pair says about its own noise and its distance from a reference.
struct estimate_report
{
    /// the root-mean-square difference of the two halves, over the reference's mean brightness
    f64 noise = 0;
    /// the root-mean-square error of the pair's tile means beyond what both images' noise explains, over the same brightness
    f64 bias = 0;
};

[[nodiscard]] f64 mean_luminance(agreement::converged const& c)
{
    auto total = 0.0;
    for (auto i = isize(0); i < c.first.size(); ++i)
        total += agreement::luminance_of(c.mean_at(i));
    return total / f64(cc::max(c.first.size(), isize(1)));
}

[[nodiscard]] f64 half_spread(agreement::converged const& c)
{
    auto sum = 0.0;
    for (auto i = isize(0); i < c.first.size(); ++i)
    {
        auto const d = agreement::luminance_of(c.first[i]) - agreement::luminance_of(c.second[i]);
        sum += d * d;
    }
    return sum / f64(cc::max(c.first.size(), isize(1)));
}

/// `c`'s halves and mean, and the reference's mean, averaged over each 8x8 tile.
struct tiled
{
    cc::vector<f64> first;
    cc::vector<f64> second;
    cc::vector<f64> reference;
    cc::vector<f64> reference_spread;
};

[[nodiscard]] tiled tiles_of(agreement::converged const& c, agreement::converged const& reference)
{
    auto const size = agreement::k_size;
    auto const tile = agreement::k_tile;
    auto out = tiled{};
    for (auto ty = 0; ty < size[1] / tile; ++ty)
        for (auto tx = 0; tx < size[0] / tile; ++tx)
        {
            auto first = 0.0;
            auto second = 0.0;
            auto mean = 0.0;
            auto spread = 0.0;
            for (auto y = ty * tile; y < (ty + 1) * tile; ++y)
                for (auto x = tx * tile; x < (tx + 1) * tile; ++x)
                {
                    auto const i = isize(y) * size[0] + x;
                    first += agreement::luminance_of(c.first[i]);
                    second += agreement::luminance_of(c.second[i]);
                    mean += agreement::luminance_of(reference.mean_at(i));
                    spread += agreement::luminance_of(reference.first[i]) - agreement::luminance_of(reference.second[i]);
                }
            auto const n = f64(tile * tile);
            out.first.push_back(first / n);
            out.second.push_back(second / n);
            out.reference.push_back(mean / n);
            out.reference_spread.push_back(spread / n);
        }
    return out;
}

/// Bias is a tile-sized error, and a tile's mean holds a 64th of a pixel's noise, whose heavy tail a per-pixel estimate cannot subtract.
/// A pair's mean has a quarter of its halves' squared difference as variance, and so has the reference's;
/// what the squared error holds beyond both is the squared bias.
[[nodiscard]] estimate_report report_of(agreement::converged const& c, agreement::converged const& reference)
{
    auto const brightness = mean_luminance(reference);
    auto const t = tiles_of(c, reference);
    auto error = 0.0;
    auto spread = 0.0;
    auto reference_spread = 0.0;
    for (auto i = isize(0); i < t.first.size(); ++i)
    {
        auto const e = (t.first[i] + t.second[i]) * 0.5 - t.reference[i];
        error += e * e;
        spread += (t.first[i] - t.second[i]) * (t.first[i] - t.second[i]);
        reference_spread += t.reference_spread[i] * t.reference_spread[i];
    }
    auto const n = f64(cc::max(t.first.size(), isize(1)));
    auto const bias_squared = (error - spread / 4.0 - reference_spread / 4.0) / n;
    return {.noise = tg::sqrt(half_spread(c)) / brightness, .bias = tg::sqrt(cc::max(bias_squared, 0.0)) / brightness};
}

[[nodiscard]] f64 error_of(cc::span<tg::vec4f const> image, agreement::converged const& reference)
{
    auto error = 0.0;
    auto total = 0.0;
    for (auto i = isize(0); i < image.size(); ++i)
        if (agreement::is_finite(image[i]))
        {
            error += tg::abs(agreement::luminance_of(image[i]) - agreement::luminance_of(reference.mean_at(i)));
            total += agreement::luminance_of(reference.mean_at(i));
        }
    return error / cc::max(total, 1e-9);
}
} // namespace

ASYNC_INVOCABLE_TEST("sv convergence - every mode's noise falls as one over the root of its samples, and its bias does "
                     "not grow",
                     (sg::context_handle const& ctx_h),
                     nx::config::thorough_only)
{
    auto& ctx = *ctx_h;
    if (!agreement::can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither the tracers nor the hybrid build");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const box = agreement::make_lit_box(resources);

    // Every mode at the path length the hybrid's one bounce corresponds to, so one reference serves them all.
    auto const reference = co_await agreement::converge(
        ctx, resources, box.items, box.camera,
        {.name = "convergence reference", .mode = sv::render_mode::megakernel, .max_bounces = 2}, 4096,
        agreement::k_reference_restarts);

    agreement::mode_config const modes[] = {
        {.name = "megakernel", .mode = sv::render_mode::megakernel, .max_bounces = 2},
        {.name = "wavefront", .mode = sv::render_mode::wavefront, .max_bounces = 2},
        {.name = "forward", .mode = sv::render_mode::hybrid, .lighting = sv::hybrid_lighting::forward},
        {.name = "deferred", .mode = sv::render_mode::hybrid, .lighting = sv::hybrid_lighting::deferred},
    };
    constexpr u32 counts[] = {32, 128, 512};

    for (auto const& m : modes)
    {
        auto reports = cc::vector<estimate_report>();
        auto line = cc::string();
        for (auto const n : counts)
        {
            auto const c = co_await agreement::converge(ctx, resources, box.items, box.camera, m, n);
            auto const r = report_of(c, reference);
            line += cc::format(" | {} samples: noise {:.2f}%, bias {:.2f}%", n, 100.0 * r.noise, 100.0 * r.bias);
            reports.push_back(r);
        }
        // Sixteen times the samples is a quarter of the noise.
        auto const rate = reports[0].noise / cc::max(reports.back().noise, 1e-12);
        CC_LOG_INFO("convergence {}:{} | noise fell {:.2f}x for 16x the samples", m.name, line, rate);

        // A mean that forgets old samples falls slower; a sampler stratified across frames may fall faster than the root.
        CHECK(rate >= 3.2).dump("mode", m.name).dump("noise fell", rate).dump("reports", line);
        CHECK(rate <= 6.0).dump("mode", m.name).dump("noise fell", rate).dump("reports", line);
        // The estimate itself scatters by a percent or two at these counts, which is what this bound leaves room for.
        CHECK(reports.back().bias <= 0.025).dump("mode", m.name).dump("reports", line);
    }
    co_await cc::async_settled(sv::background_work(ctx));
}

ASYNC_INVOCABLE_TEST("sv convergence - a denoised image approaches the converged one as its mean grows",
                     (sg::context_handle const& ctx_h),
                     nx::config::thorough_only)
{
    auto& ctx = *ctx_h;
    if (!agreement::can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither the tracers nor the hybrid build");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const box = agreement::make_lit_box(resources);
    auto const reference = co_await agreement::converge(
        ctx, resources, box.items, box.camera,
        {.name = "convergence reference", .mode = sv::render_mode::megakernel, .max_bounces = 2}, 4096,
        agreement::k_reference_restarts);
    auto reference_mean = cc::vector<tg::vec4f>();
    for (auto i = isize(0); i < reference.first.size(); ++i)
        reference_mean.push_back(reference.mean_at(i));
    auto const exposure = agreement::exposure_of(reference_mean);

    auto const support = sr::query_reconstruct_support(ctx);
    sr::denoise_method const members[]
        = {sr::denoise_method::atrous, sr::denoise_method::svgf, sr::denoise_method::oidn, sr::denoise_method::dlss_rr};
    // A thousand hybrid frames a member is most of this test's time, so a default run stops a quadrupling short.
    auto checkpoints = cc::vector<u32>{16, 64, 256};
    if (nx::is_thorough())
        checkpoints.push_back(1024);

    for (auto const member : members)
    {
        auto const name = cc::format("convergence-hybrid-{}", sr::to_string(member));
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

        auto denoised_errors = cc::vector<f64>();
        auto raw_errors = cc::vector<f64>();
        auto panels = cc::vector<cc::vector<tg::vec3f>>();
        panels.push_back(agreement::display_of(reference_mean, exposure));
        auto line = cc::string();
        auto next = 0;
        for (auto f = 0; f < 4 * 1024 && next < checkpoints.size(); ++f)
        {
            auto const r = co_await view.frame();
            if (!r.ran || r.denoised.empty() || r.samples < checkpoints[next])
                continue;
            denoised_errors.push_back(error_of(r.denoised, reference));
            raw_errors.push_back(error_of(r.raw, reference));
            line += cc::format(" | {} samples: denoised {:.1f}%, mean {:.1f}%", r.samples,
                               100.0 * denoised_errors.back(), 100.0 * raw_errors.back());
            panels.push_back(agreement::display_of(r.denoised, exposure));
            ++next;
        }
        CC_LOG_INFO("{}:{}", name, line);
        agreement::write_panels(name, panels);

        REQUIRE(denoised_errors.size() == checkpoints.size()).dump("test", cc::string_view(name)).dump("reports", line);
        for (auto k = isize(1); k < denoised_errors.size(); ++k)
            CHECK(denoised_errors[k] <= denoised_errors[k - 1] * 1.05)
                .dump("test", cc::string_view(name))
                .dump("checkpoint", checkpoints[k])
                .dump("reports", line);
        // On a nearly converged mean a filter has next to nothing to remove, and may take nothing away that is there.
        CHECK(denoised_errors.back() <= raw_errors.back() * 1.2 + 0.005)
            .dump("test", cc::string_view(name))
            .dump("reports", line);
        co_await cc::async_settled(sv::background_work(ctx));
    }
}
