#include "agreement_test_support.hh"

#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/linalg/quat.hh>

// What a renderer must do whatever its implementation, held without a reference image.
//
// Light adds: twice the emission is twice the image.
// Nothing lit is black, exactly, in every mode.
// The whole scene turned together with its camera is the same image, and the scene mirrored with its camera kept is that image mirrored.
// A mirrored placement turns its triangles' winding around, which is where a normal or a facing test most often goes wrong.
// The lit box's fallback light stays off throughout: it is fixed in the world, and the lamp is the box's light.

namespace
{
using namespace cc::primitive_defines;
namespace agreement = sv_test::agreement;

/// The Cornell box under a lamp of `nits`, its enclosure and lamp as two items; no lamp at all where `nits` is 0.
/// Unless `landed` is false, every upload has landed by the time it returns.
[[nodiscard]] cc::vector<sv::scene_item> lit_box_items(sv::gpu_resource_manager& resources, f32 nits, bool landed = true)
{
    auto const lit = sv_test::as_lit_meshes(sv_test::make_cornell_box(), nits > 0.0f ? nits : 1.0f);
    auto items = cc::vector<sv::scene_item>();
    items.push_back(resources.acquire_scene_item(lit.enclosure));
    if (nits > 0.0f)
        items.push_back(resources.acquire_scene_item(lit.lamp));
    if (landed)
        resources.wait_for_pending_uploads();
    return items;
}

[[nodiscard]] sv::camera box_camera()
{
    auto camera = sv_test::cornell_camera();
    camera.projection.vertical_fov = tg::angle_d::make_from_degree(30.0);
    return camera;
}

/// `c` with both halves multiplied by `factor`.
[[nodiscard]] agreement::converged scaled(agreement::converged c, f32 factor)
{
    for (auto& v : c.first)
        v = tg::vec4f(v[0] * factor, v[1] * factor, v[2] * factor, v[3]);
    for (auto& v : c.second)
        v = tg::vec4f(v[0] * factor, v[1] * factor, v[2] * factor, v[3]);
    return c;
}

/// `c` mirrored left to right, its guides with it.
[[nodiscard]] agreement::converged mirrored(agreement::converged const& c)
{
    auto const size = agreement::k_size;
    auto out = c;
    auto const flip = [&](auto const& from, auto& to)
    {
        for (auto y = 0; y < size[1]; ++y)
            for (auto x = 0; x < size[0]; ++x)
                to[isize(y) * size[0] + x] = from[isize(y) * size[0] + (size[0] - 1 - x)];
    };
    flip(c.first, out.first);
    flip(c.second, out.second);
    if (!c.normals.empty())
    {
        flip(c.normals, out.normals);
        flip(c.albedo, out.albedo);
    }
    return out;
}

/// `items` each placed under `t` before their own placement.
[[nodiscard]] cc::vector<sv::scene_item> placed(cc::span<sv::scene_item const> items, tg::affine_transform3f const& t)
{
    auto out = cc::vector<sv::scene_item>();
    for (auto item : items)
    {
        item.transform = t.composed(item.transform);
        out.push_back(item);
    }
    return out;
}

/// The two modes each invariant runs in: the reference tracer, and the hybrid's default lighting.
[[nodiscard]] cc::vector<agreement::mode_config> invariant_modes(cc::string_view name)
{
    auto out = cc::vector<agreement::mode_config>();
    out.push_back({.name = name, .mode = sv::render_mode::megakernel, .fallback_light = false});
    out.push_back({.name = name,
                   .mode = sv::render_mode::hybrid,
                   .lighting = sv::hybrid_lighting::deferred,
                   .fallback_light = false});
    return out;
}

[[nodiscard]] u32 samples_for(agreement::mode_config const& m)
{
    return m.mode == sv::render_mode::hybrid ? 512u : 1024u;
}

[[nodiscard]] cc::string_view mode_name(agreement::mode_config const& m)
{
    return m.mode == sv::render_mode::hybrid ? "deferred" : "megakernel";
}
} // namespace

ASYNC_INVOCABLE_TEST("sv invariant - twice the light is twice the image", (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!agreement::can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither the tracers nor the hybrid build");
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const camera = box_camera();
    auto const dim = lit_box_items(resources, 15.0f);
    auto const bright = lit_box_items(resources, 30.0f);

    for (auto const& m : invariant_modes("light"))
    {
        auto const once = co_await agreement::converge(ctx, resources, dim, camera, m, samples_for(m),
                                                       agreement::k_reference_restarts);
        auto const twice = co_await agreement::converge(ctx, resources, bright, camera, m, samples_for(m));
        agreement::check_agreement(cc::format("invariant-light-{}", mode_name(m)), twice, scaled(once, 2.0f), 0.01);
    }
    co_await cc::async_settled(sv::background_work(ctx));
}

ASYNC_INVOCABLE_TEST("sv invariant - a scene with nothing lit is black in every mode", (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!agreement::can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither the tracers nor the hybrid build");
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const unlit = lit_box_items(resources, 0.0f);

    agreement::mode_config const modes[] = {
        {.name = "black megakernel", .mode = sv::render_mode::megakernel, .fallback_light = false},
        {.name = "black wavefront", .mode = sv::render_mode::wavefront, .fallback_light = false},
        {.name = "black forward",
         .mode = sv::render_mode::hybrid,
         .lighting = sv::hybrid_lighting::forward,
         .fallback_light = false},
        {.name = "black deferred",
         .mode = sv::render_mode::hybrid,
         .lighting = sv::hybrid_lighting::deferred,
         .fallback_light = false},
    };
    for (auto const& m : modes)
    {
        auto const c
            = co_await agreement::converge(ctx, resources, unlit, box_camera(), m, 16, 0, agreement::k_size, true);
        auto lit = 0;
        for (auto i = isize(0); i < c.first.size(); ++i)
            lit += c.first[i][0] != 0.0f || c.first[i][1] != 0.0f || c.first[i][2] != 0.0f ? 1 : 0;
        CHECK(lit == 0).dump("mode", m.name).dump("pixels not black", lit);
    }
    co_await cc::async_settled(sv::background_work(ctx));
}

ASYNC_INVOCABLE_TEST("sv invariant - the scene turned with its camera is the same image",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!agreement::can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither the tracers nor the hybrid build");
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const items = lit_box_items(resources, 15.0f);
    auto const camera = box_camera();

    // About two axes, so the camera's up turns too.
    auto const turn_f = tg::quat_f::make_rotation_y(tg::angle_f::make_from_degree(35.0f))
                      * tg::quat_f::make_rotation_x(tg::angle_f::make_from_degree(20.0f));
    auto const turn_d = tg::quat_d::make_rotation_y(tg::angle_d::make_from_degree(35.0))
                      * tg::quat_d::make_rotation_x(tg::angle_d::make_from_degree(20.0));
    auto const turned_items
        = placed(items, tg::affine_transform3f::make_trs(tg::vec3f(0, 0, 0), turn_f, tg::vec3f(1, 1, 1)));
    auto turned_camera = camera;
    turned_camera.position = tg::pos3d::zero + turn_d * (camera.position - tg::pos3d::zero);
    turned_camera.orientation = turn_d * camera.orientation;

    for (auto const& m : invariant_modes("turned"))
    {
        auto const as_is = co_await agreement::converge(ctx, resources, items, camera, m, samples_for(m),
                                                        agreement::k_reference_restarts);
        auto const turned = co_await agreement::converge(ctx, resources, turned_items, turned_camera, m, samples_for(m));
        agreement::check_agreement(cc::format("invariant-turned-{}", mode_name(m)), turned, as_is, 0.01);
    }
    co_await cc::async_settled(sv::background_work(ctx));
}

ASYNC_INVOCABLE_TEST("sv invariant - the scene mirrored is the image mirrored",
                     (sg::context_handle const& ctx_h),
                     nx::config::thorough_only)
{
    auto& ctx = *ctx_h;
    if (!agreement::can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither the tracers nor the hybrid build");
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const items = lit_box_items(resources, 15.0f);
    // The camera sits on x = 0 looking along +z, so mirroring x leaves it where it is and flips what it sees.
    auto const camera = box_camera();
    auto const mirror_items
        = placed(items, tg::affine_transform3f::make_trs(tg::vec3f(0, 0, 0), tg::quat_f::identity, tg::vec3f(-1, 1, 1)));

    for (auto const& m : invariant_modes("mirrored"))
    {
        auto const as_is = co_await agreement::converge(ctx, resources, items, camera, m, samples_for(m),
                                                        agreement::k_reference_restarts);
        auto const mirror = co_await agreement::converge(ctx, resources, mirror_items, camera, m, samples_for(m));
        agreement::check_agreement(cc::format("invariant-mirrored-{}", mode_name(m)), mirror, mirrored(as_is), 0.01);
    }
    co_await cc::async_settled(sv::background_work(ctx));
}

// The box's only light is its lamp, so every frame drawn before the lamp can be sampled is black.
// Such a frame must restart the mean and read as holding no samples: blended in, it darkens the image for good,
// and counted, a caller waiting on a sample count takes the black image as the converged one.
// The lamp's mesh and its area CDF each land a frame or more after the first trace, which a fresh manager shows.
ASYNC_INVOCABLE_TEST("sv invariant - a scene still landing converges to the scene landed",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!agreement::can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither the tracers nor the hybrid build");

    for (auto const& m : invariant_modes("landing"))
    {
        // The reference warms the mode too, so the fresh box traces from its first frame rather than waiting out a compile.
        auto warm = sv::gpu_resource_manager::create(ctx);
        auto const landed = co_await agreement::converge(ctx, warm, lit_box_items(warm, 15.0f), box_camera(), m, 256,
                                                         agreement::k_reference_restarts);
        // A manager of its own, so the lamp is acquired afresh and nothing of it has landed.
        auto fresh = sv::gpu_resource_manager::create(ctx);
        auto const landing
            = co_await agreement::converge(ctx, fresh, lit_box_items(fresh, 15.0f, false), box_camera(), m, 256);
        agreement::check_agreement(cc::format("invariant-landing-{}", mode_name(m)), landing, landed, 0.01);
        co_await cc::async_settled(sv::background_work(ctx));
    }
}
