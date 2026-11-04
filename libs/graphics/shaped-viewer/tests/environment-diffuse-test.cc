#include "agreement_test_support.hh"

#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>

// The environment's diffuse light estimated against its irradiance, `sv::environment_diffuse::exact`, held to the sampled estimate.
// It must converge to the same image wherever the sky is partly blocked, and take the noise off a face nothing blocks.

namespace
{
using namespace cc::primitive_defines;
namespace agreement = sv_test::agreement;

/// The modes that trace the environment, each under the studio sky alone.
[[nodiscard]] cc::vector<agreement::mode_config> sky_modes(sv::environment_diffuse estimate)
{
    auto const studio = sv::background::studio();
    auto out = cc::vector<agreement::mode_config>();
    out.push_back({.name = "megakernel",
                   .mode = sv::render_mode::megakernel,
                   .background = studio,
                   .fallback_light = false,
                   .environment_diffuse = estimate});
    out.push_back({.name = "wavefront",
                   .mode = sv::render_mode::wavefront,
                   .background = studio,
                   .fallback_light = false,
                   .environment_diffuse = estimate});
    out.push_back({.name = "forward",
                   .mode = sv::render_mode::hybrid,
                   .lighting = sv::hybrid_lighting::forward,
                   .background = studio,
                   .fallback_light = false,
                   .environment_diffuse = estimate});
    out.push_back({.name = "deferred",
                   .mode = sv::render_mode::hybrid,
                   .lighting = sv::hybrid_lighting::deferred,
                   .background = studio,
                   .fallback_light = false,
                   .environment_diffuse = estimate});
    return out;
}

[[nodiscard]] u32 samples_for(agreement::mode_config const& m)
{
    return m.mode == sv::render_mode::hybrid ? 512u : 1024u;
}

/// A matte cube floating at the origin, nothing near it, so no face of it blocks the sky from another.
[[nodiscard]] cc::vector<sv::scene_item> floating_cube(sv::gpu_resource_manager& resources)
{
    auto cube = sv_test::cornell_box{};
    sv_test::cb_push_box(
        cube, tg::pos3f(-0.5f, -0.5f, -0.5f), tg::pos3f(0.5f, 0.5f, 0.5f),
        {.base_color = tg::vec3f(0.7f, 0.7f, 0.7f), .metallic = 0.0f, .roughness = 1.0f, .emissive = tg::vec3f(0, 0, 0)});
    auto items = cc::vector<sv::scene_item>();
    items.push_back(resources.acquire_scene_item(sv_test::as_mesh("floating cube", cube.positions, cube.materials)));
    resources.wait_for_pending_uploads();
    return items;
}

/// How far two estimates of one image differ, summed over its pixels, relative to its brightness.
[[nodiscard]] f64 relative_noise(agreement::converged const& c)
{
    auto difference = 0.0;
    auto total = 0.0;
    for (auto i = isize(0); i < c.first.size(); ++i)
    {
        difference += tg::abs(agreement::luminance_of(c.first[i]) - agreement::luminance_of(c.second[i]));
        total += 0.5 * (agreement::luminance_of(c.first[i]) + agreement::luminance_of(c.second[i]));
    }
    return difference / cc::max(total, 1e-9);
}
} // namespace

ASYNC_INVOCABLE_TEST("sv environment diffuse - exact converges to the sampled image where the sky is partly blocked",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!agreement::can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither the tracers nor the hybrid build");
    auto resources = sv::gpu_resource_manager::create(ctx);
    // The box without its lamp, open toward the camera: every wall sees the sky through the opening and is blocked from the rest of it.
    auto const lit = sv_test::as_lit_meshes(sv_test::make_cornell_box(), 1.0f);
    auto items = cc::vector<sv::scene_item>();
    items.push_back(resources.acquire_scene_item(lit.enclosure));
    resources.wait_for_pending_uploads();
    auto camera = sv_test::cornell_camera();
    camera.projection.vertical_fov = tg::angle_d::make_from_degree(30.0);

    auto const sampled = sky_modes(sv::environment_diffuse::sampled);
    auto const exact = sky_modes(sv::environment_diffuse::exact);
    for (auto i = isize(0); i < exact.size(); ++i)
    {
        auto const reference = co_await agreement::converge(ctx, resources, items, camera, sampled[i],
                                                            samples_for(sampled[i]), agreement::k_reference_restarts);
        auto const contender
            = co_await agreement::converge(ctx, resources, items, camera, exact[i], samples_for(exact[i]));
        agreement::check_agreement(cc::format("environment-diffuse-{}", exact[i].name), contender, reference, 0.01);
        CC_LOG_INFO("environment diffuse noise in the box, {}: sampled {:.4f}, exact {:.4f}", exact[i].name,
                    relative_noise(reference), relative_noise(contender));
    }
    co_await cc::async_settled(sv::background_work(ctx));
}

ASYNC_INVOCABLE_TEST("sv environment diffuse - a face nothing blocks takes no noise from the sky",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!agreement::can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither the tracers nor the hybrid build");
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const items = floating_cube(resources);
    auto const camera = sv::camera::looking_at(tg::pos3d(1.6, 1.2, -1.9), tg::pos3d(0, 0, 0));

    auto sampled = sky_modes(sv::environment_diffuse::sampled);
    auto exact = sky_modes(sv::environment_diffuse::exact);
    for (auto i = isize(0); i < exact.size(); ++i)
    {
        // One frame's worth: a moving image shows exactly this.
        sampled[i].samples_per_pixel = 1;
        exact[i].samples_per_pixel = 1;
        auto const before
            = co_await agreement::converge(ctx, resources, items, camera, sampled[i], 1, 0, agreement::k_size, true);
        auto const after
            = co_await agreement::converge(ctx, resources, items, camera, exact[i], 1, 0, agreement::k_size, true);
        auto const noise_before = relative_noise(before);
        auto const noise_after = relative_noise(after);
        CC_LOG_INFO("environment diffuse noise, {}: sampled {:.4f}, exact {:.4f}", exact[i].name, noise_before,
                    noise_after);
        // What stays is the rough specular layer's sky, which is still sampled.
        CHECK(noise_after < 0.2 * noise_before)
            .dump("mode", exact[i].name)
            .dump("sampled", noise_before)
            .dump("exact", noise_after);
        auto const exposure = agreement::exposure_of(before.first);
        cc::vector<tg::vec3f> const panels[]
            = {agreement::display_of(before.first, exposure), agreement::display_of(after.first, exposure)};
        agreement::write_panels(cc::format("environment-diffuse-noise-{}", exact[i].name), panels);
    }
    co_await cc::async_settled(sv::background_work(ctx));
}
