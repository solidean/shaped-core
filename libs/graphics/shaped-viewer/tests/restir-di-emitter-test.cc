#include "viewer_test_env.hh"
#include "wavefront_test_support.hh"

#include <clean-core/common/log.hh>
#include <clean-core/common/macros.hh> // CC_ARCH_ARM64
#include <clean-core/container/array.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

// ReSTIR DI over emissive lamps through the wavefront, emissive triangles' P3: points on their triangles resampled as points on rects are.
// Each is held to plain path tracing's mean on the same frames, by the harness wavefront-trace-test.cc's ReSTIR tests run on.
// The stretched lamp is the exception, held to the same run with GI reuse alone, for the reason it gives.

namespace
{
using namespace cc::primitive_defines;
using namespace sv_test::wavefront;

/// The half-width of each lamp of the many-lights tests.
constexpr f32 k_lamp_half = 0.06f;

/// One lamp of a box lit by many: where its center hangs, how large and bright it is, and whether it is an emissive mesh rather than a rect light.
/// `nits` defaults to the box's own lamp's radiance.
struct box_lamp
{
    tg::pos3f center = {};
    bool emissive = false;
    f32 half = k_lamp_half;
    f32 nits = 15.0f;
};

/// The horizontal square of half-width `half` centered on `center`, as a fan of four triangles around a point off its center.
/// Their areas are 0.4, 0.7, 1.3 and 1.6 times `half²`, so a draw that took every triangle as equally likely, or skipped its area, would move the mean.
/// Wound as the box's own lamp.
[[nodiscard]] cc::vector<tg::pos3f> lamp_fan(tg::pos3f center, f32 half)
{
    auto const corners = cc::array<tg::pos3f>{tg::pos3f(center[0] - half, center[1], center[2] - half),
                                              tg::pos3f(center[0] + half, center[1], center[2] - half),
                                              tg::pos3f(center[0] + half, center[1], center[2] + half),
                                              tg::pos3f(center[0] - half, center[1], center[2] + half)};
    auto const apex = tg::pos3f(center[0] + 0.6f * half, center[1], center[2] - 0.3f * half);
    auto out = cc::vector<tg::pos3f>();
    for (auto i = isize(0); i < 4; ++i)
    {
        out.push_back(apex);
        out.push_back(corners[i]);
        out.push_back(corners[(i + 1) % 4]);
    }
    return out;
}

/// An openpbr material that reflects nothing and emits `nits` of white: a constant emission, which the view makes a light of.
[[nodiscard]] sv::material_id emissive_material(f32 nits)
{
    auto& lib = sv_test::shared_material_library();
    auto const type = lib.acquire_type(sv::builtin_material::openpbr).value();
    return lib.acquire(sv::material::create(cc::format("wavefront lamp {}", nits), type,
                                            {sv::material_attribute_binding::of("base_color", tg::vec3f(0, 0, 0)),
                                             sv::material_attribute_binding::of("specular_weight", 0.0f),
                                             sv::material_attribute_binding::of("emission_luminance", nits)}));
}

/// Adds the triangles `positions` emitting `nits` to `scene` as an emissive mesh of its own, and its light record of `area` to `records`.
/// The record is selection's estimate, as the view writes it: the world area, both faces, at the constant's radiance.
/// `place_lamps` makes an instance of it once every lamp is added.
void add_emissive_lamp(sv::gpu_resource_manager& resources,
                       cornell_scene& scene,
                       cc::vector<sv::shaders::tracer::light_record>& records,
                       cc::vector<tg::pos3f> positions,
                       f32 nits,
                       f32 area)
{
    scene.emitters.push_back(resources.acquire_scene_item(
        sv::mesh{.name = cc::format("wavefront lamp {} of {} nits", scene.emitters.size(), nits),
                 .geometry = sv::triangle_geometry::create_from_positions(cc::move(positions)),
                 .material = emissive_material(nits)}));
    records.push_back({.path = sv::light_path_mesh,
                       .area = area,
                       .flags = sv::light_flag_two_sided,
                       .emission = tg::vec3f(nits, nits, nits),
                       .cone_offset = 1,
                       .link_mask = ~0u});
}

/// Places every emissive lamp `add_emissive_lamp` added as one more TLAS instance, and makes `records` the scene's light table.
/// Each lamp shades with its own permutation's records, two per permutation, so its offset is twice the permutation's index.
void place_lamps(sv::gpu_resource_manager& resources,
                 cornell_scene& scene,
                 cc::span<sv::shaders::tracer::light_record const> records)
{
    resources.wait_for_pending_uploads();
    for (auto const& item : scene.emitters)
    {
        auto const* const permutation = resources.shaders.find(item.shader_key);
        REQUIRE(permutation != nullptr);
        auto group = isize(0);
        while (group < scene.hit_groups.size() && scene.hit_groups[group] != permutation)
            ++group;
        if (group == scene.hit_groups.size())
            scene.hit_groups.push_back(permutation);
        scene.instances.push_back(sg::tlas_instance{.blas = sv_test::blas_of(resources, item.mesh),
                                                    .instance_id = u32(scene.instances.size()),
                                                    .hit_group_offset = u32(2 * group),
                                                    .opaque_override = !permutation->opacity.can_cut_out()});
    }
    scene.lights = sv::pt_light_table::grouped(records);
}

/// Lights `scene` by `lamps` alone, each a horizontal square facing down.
/// A rect lamp is an analytic record.
/// An emissive one is a fan of four unequal triangles, a mesh of its own made a light of the trace as `sv::view_renderer` makes one.
void light_with_lamps(sv::gpu_resource_manager& resources, cornell_scene& scene, cc::span<box_lamp const> lamps)
{
    auto records = cc::vector<sv::shaders::tracer::light_record>();
    for (auto const& lamp : lamps)
    {
        auto const area = 4.0f * lamp.half * lamp.half;
        if (lamp.emissive)
        {
            add_emissive_lamp(resources, scene, records, lamp_fan(lamp.center, lamp.half), lamp.nits, area);
            continue;
        }
        records.push_back({.position = tg::vec3f(lamp.center[0], lamp.center[1], lamp.center[2]),
                           .path = u32(sv::light_path::area),
                           .u = tg::vec3f(lamp.half, 0, 0),
                           .area = area,
                           .v = tg::vec3f(0, 0, lamp.half),
                           .emission = tg::vec3f(lamp.nits, lamp.nits, lamp.nits),
                           .normal = tg::vec3f(0, -1, 0),
                           .cone_offset = 1,
                           .link_mask = ~0u});
    }
    place_lamps(resources, scene, records);
}

/// The 22 lamps of wavefront-trace-test.cc's many-lights test: a grid of 5 by 4 under the ceiling and two more, each emissive where `emissive(k)` says.
/// `varied` gives them three sizes and four brightnesses, in periods that do not divide each other, so no two neighbors in the table are alike.
/// `height` is where they hang; the ceiling is at 1.
template <class Emissive>
[[nodiscard]] cc::vector<box_lamp> twenty_two_lamps(Emissive emissive, bool varied, f32 height = 0.99f)
{
    auto out = cc::vector<box_lamp>();
    auto const add = [&](f32 x, f32 z)
    {
        auto const k = out.size();
        auto const half = varied ? k_lamp_half * (0.6f + 0.4f * f32(k % 3)) : k_lamp_half;
        auto const nits = varied ? 15.0f * (0.5f + 0.5f * f32(k % 4)) : 15.0f;
        out.push_back({.center = tg::pos3f(x, height, z), .emissive = emissive(k), .half = half, .nits = nits});
    };
    for (auto ix = 0; ix < 5; ++ix)
        for (auto iz = 0; iz < 4; ++iz)
            add(-0.8f + 0.4f * f32(ix), -0.6f + 0.4f * f32(iz));
    add(-0.9f, 0.9f);
    add(0.9f, -0.9f);
    return out;
}
} // namespace

// The many-lights test with every lamp an emissive fan: ReSTIR DI over points on emissive instances' triangles, emissive triangles' P3.
// Each reservoir holds a lamp's light record, a triangle and its barycentrics, which every reuse places through the lamp's instance anew.
// The lamps light both faces, so the ceiling above each is lit too; plain and resampled frames see the same scene.
// They hang a tenth below the ceiling rather than a hundredth: a two-sided lamp that close lights the ceiling beside it with an integrand near 1/h²,
// whose rare huge samples move a 32-pixel mean by several percent whichever estimator draws them.
// Held to the rect lamps' bounds: the mean within 3% of the plain one, and under --thorough the error 1.15 times below GI reuse alone.
// Unlike a rect light, an emissive lamp is seen by the camera, and at 32 pixels a lamp is a pixel or two that a jittered ray hits or misses.
// That is no estimator's noise, so the error leaves out every pixel whose camera ray met an emitter in any frame of the reference.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - ReSTIR DI keeps the image of many emissive lamps and cuts its noise",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif
    auto& ctx = *ctx_h;
    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler that reaches DXIL to build the path-tracing shaders");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto scene = make_scene(resources);
    light_with_lamps(resources, scene, twenty_two_lamps([](isize) { return true; }, false, 0.9f));
    REQUIRE(scene.emitters.size() == 22);
    REQUIRE(scene.lights.path_count[sv::light_path_mesh] == 22);

    constexpr int frames = 48;
    constexpr int warm = 16;
    auto const plain = co_await run_restir(ctx, resources, scene, frames, {.history = false});
    auto const di = co_await run_restir(ctx, resources, scene, frames, {.history = true, .spatial = true, .di = true});

    auto const plain_mean = mean_luminance_of(plain.mean);
    auto const di_mean = mean_luminance_of(di.mean);
    CHECK(tg::abs(di_mean - plain_mean) < 0.03f * plain_mean)
        .context(cc::format("ReSTIR DI over emissive lamps: {} against {} without", di_mean, plain_mean));
    // A DI history that survives from frame to frame lifts the confidence toward its cap of 20; one dropped every frame leaves it at 1.
    // Lamps that stand still keep the light table's key, so their history is reused.
    CHECK(di.di_confidence > 4.0f).context(cc::format("DI confidence {}", di.di_confidence));
    CC_LOG_INFO("P3, 22 emissive lamps: means {} with ReSTIR DI, {} without", di_mean, plain_mean);
    CC_LOG_INFO("P3, 22 emissive lamps: DI confidence {}, {} of it on a lamp", di.di_confidence, di.di_mesh_share);

    if (nx::is_thorough())
    {
        auto const reference = co_await run_restir(ctx, resources, scene, 4 * frames, {.history = false, .seeds = 1});
        auto const gi = co_await run_restir(ctx, resources, scene, frames, {.history = true, .spatial = true});
        auto plain_error = 0.0f;
        auto gi_error = 0.0f;
        auto di_error = 0.0f;
        auto plain_error_all = 0.0f;
        auto gi_error_all = 0.0f;
        auto di_error_all = 0.0f;
        for (auto f = warm; f < frames; ++f)
        {
            plain_error += rms_error(plain.frames[f], reference.mean, reference.emitter_seen) / f32(frames - warm);
            gi_error += rms_error(gi.frames[f], reference.mean, reference.emitter_seen) / f32(frames - warm);
            di_error += rms_error(di.frames[f], reference.mean, reference.emitter_seen) / f32(frames - warm);
            plain_error_all += rms_error(plain.frames[f], reference.mean) / f32(frames - warm);
            gi_error_all += rms_error(gi.frames[f], reference.mean) / f32(frames - warm);
            di_error_all += rms_error(di.frames[f], reference.mean) / f32(frames - warm);
        }
        auto seen = 0;
        for (auto const n : reference.emitter_seen)
            seen += n > 0 ? 1 : 0;
        CHECK(di_error * 1.15f < gi_error)
            .context(cc::format("per-frame error: {} with ReSTIR DI, {} with GI reuse alone", di_error, gi_error));
        auto const reference_mean = mean_luminance_of(reference.mean);
        CC_LOG_INFO("P3 reference mean {}, with {} pixels that see a lamp", reference_mean, seen);
        CC_LOG_INFO("P3 per-frame error without them: {} with DI, {} with GI reuse alone, {} plain", di_error, gi_error,
                    plain_error);
        CC_LOG_INFO("P3 noise ratio, GI reuse alone over DI: {}, held above 1.15", gi_error / di_error);
        CC_LOG_INFO("P3 per-frame error with them: {} with DI, {} with GI reuse alone, {} plain", di_error_all,
                    gi_error_all, plain_error_all);
    }

    co_await cc::async_settled(sv::background_work(ctx));
}

// Rect lamps and emissive lamps in one table: one reservoir domain holds points in either light's own measure.
// Every other lamp of the 22 is an emissive fan, so the two kinds alternate through the selection CDF and its indices.
// All hang a tenth below the ceiling, for the reason the many-emissive-lamps test gives: at one bounce nothing damps the near field.
// The lamps come in three sizes and four brightnesses, and each fan's triangles in four areas.
// So a candidate's density that dropped the triangle's probability or its area, or an emitter read one record off, would move the mean.
// The resampled mean is the plain one's within 3%, as every kind of analytic light's is, with the spatial pass and without.
// Under --thorough also at one bounce, where the resampled sample takes the whole of next-event estimation's weight.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - ReSTIR DI keeps the mean of rect lamps and emissive lamps together",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif
    auto& ctx = *ctx_h;
    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler that reaches DXIL to build the path-tracing shaders");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto scene = make_scene(resources);
    light_with_lamps(resources, scene, twenty_two_lamps([](isize k) { return k % 2 == 1; }, true, 0.9f));
    REQUIRE(scene.emitters.size() == 11);
    REQUIRE(scene.lights.path_count[u32(sv::light_path::area)] == 11);

    constexpr int frames = 48;
    auto const plain = co_await run_restir(ctx, resources, scene, frames, {.history = false});
    auto const di = co_await run_restir(ctx, resources, scene, frames, {.history = true, .spatial = true, .di = true});
    auto const temporal = co_await run_restir(ctx, resources, scene, frames, {.history = true, .di = true});
    auto const plain_mean = mean_luminance_of(plain.mean);
    auto const di_mean = mean_luminance_of(di.mean);
    auto const temporal_mean = mean_luminance_of(temporal.mean);
    CHECK(tg::abs(di_mean - plain_mean) < 0.03f * plain_mean)
        .context(cc::format("rects and emitters: {} with ReSTIR DI, {} without", di_mean, plain_mean));
    CHECK(tg::abs(temporal_mean - plain_mean) < 0.03f * plain_mean)
        .context(cc::format("without the spatial pass: {} with ReSTIR DI, {} without", temporal_mean, plain_mean));
    CC_LOG_INFO("P3, rect and emissive lamps: means {} with ReSTIR DI, {} without", di_mean, plain_mean);
    CC_LOG_INFO("P3, rect and emissive lamps: DI confidence {}, {} of it on a lamp", di.di_confidence, di.di_mesh_share);

    if (nx::is_thorough())
    {
        auto const plain_once = co_await run_restir(ctx, resources, scene, frames, {.history = false, .max_bounces = 1});
        auto const di_once = co_await run_restir(ctx, resources, scene, frames,
                                                 {.history = true, .spatial = true, .di = true, .max_bounces = 1});
        auto const plain_once_mean = mean_luminance_of(plain_once.mean);
        auto const di_once_mean = mean_luminance_of(di_once.mean);
        CHECK(tg::abs(di_once_mean - plain_once_mean) < 0.03f * plain_once_mean)
            .context(cc::format("one bounce: {} with ReSTIR DI, {} without", di_once_mean, plain_once_mean));
    }

    co_await cc::async_settled(sv::background_work(ctx));
}

// ReSTIR DI under an emitter that moves every frame: one emissive fan, as built on even frames and stretched on odd ones.
// The stretch doubles x and halves y about the lamp's own plane, so it keeps the volume and the lamp's height, and doubles every triangle's world area.
// A reservoir keeps its point as barycentrics, which the stretch carries along, but its weight holds only under the placement it was drawn under.
// Reusing it under another is deliberately not done: the history's resampling weight would have to be formed with the old placement, which the frame no longer has.
// So the light table's key covers every emitter's placement, and each frame here drops the DI history, as the view does on any change of its scene hash.
// Each placement's mean over its own frames is held to the same run's with GI reuse alone, within 3%, since both carry GI's history across the move.
// Over the whole run an error on one placement would nearly cancel one on the other, which is why the halves are checked rather than the run's mean.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - ReSTIR DI keeps the mean of an emissive lamp stretched every other frame",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif
    auto& ctx = *ctx_h;
    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler that reaches DXIL to build the path-tracing shaders");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto scene = make_scene(resources);
    auto lamps = cc::vector<box_lamp>();
    lamps.push_back({.center = tg::pos3f(0.0f, 0.9f, 0.0f), .emissive = true, .half = 0.25f});
    light_with_lamps(resources, scene, lamps);
    REQUIRE(scene.emitters.size() == 1);
    REQUIRE(scene.instances.size() == 2);

    // Row-major 3x4: x doubled, y halved and moved up by 0.45, which keeps the plane y = 0.9 where it is.
    auto stretched = scene.instances;
    stretched[1].transform[0] = 2.0f;
    stretched[1].transform[5] = 0.5f;
    stretched[1].transform[7] = 0.45f;
    auto placements = cc::vector<cc::vector<sg::tlas_instance>>();
    placements.push_back(scene.instances);
    placements.push_back(cc::move(stretched));

    constexpr int frames = 48;
    // Past the first frames, whose history is still young; even, so phase 0 is the as-built placement.
    constexpr int warm = 16;
    auto const plain = co_await run_restir(ctx, resources, scene, frames, {.history = false, .placements = placements});
    auto const di = co_await run_restir(ctx, resources, scene, frames,
                                        {.history = true, .spatial = true, .di = true, .placements = placements});
    // DI is held to the same run without it rather than to the plain one.
    // Both carry ReSTIR GI's history across the alternation, which a routine keeps through a moved instance where a view would forget it.
    // So the two placements' indirect light bleeds into each other alike in both, and only DI's share differs.
    auto const gi = co_await run_restir(ctx, resources, scene, frames,
                                        {.history = true, .spatial = true, .placements = placements});

    auto const phase_mean = [&](restir_run const& run, int phase) -> f32
    {
        auto sum = 0.0f;
        auto count = 0;
        for (auto f = warm + phase; f < frames; f += 2)
        {
            sum += mean_luminance_of(run.frames[f]);
            ++count;
        }
        return sum / f32(cc::max(count, 1));
    };
    // The stretch reached the trace: the lamp of twice the area lights the box nearly twice as brightly.
    CHECK(phase_mean(plain, 1) > 1.2f * phase_mean(plain, 0))
        .context(cc::format("plain means: {} stretched, {} as built", phase_mean(plain, 1), phase_mean(plain, 0)));
    // Every frame moved the lamp, so none reused a DI history: each reservoir holds its own frame's candidates alone.
    CHECK(di.di_confidence < 1.5f).context(cc::format("DI confidence {}", di.di_confidence));
    for (auto phase = 0; phase < 2; ++phase)
    {
        auto const gi_mean = phase_mean(gi, phase);
        auto const di_mean = phase_mean(di, phase);
        auto const placement = phase == 0 ? cc::string_view("as built") : cc::string_view("stretched");
        CHECK(tg::abs(di_mean - gi_mean) < 0.03f * gi_mean)
            .context(cc::format("the lamp {}: {} with ReSTIR DI, {} with GI reuse alone", placement, di_mean, gi_mean));
        CC_LOG_INFO("P3, a lamp {}: means {} with ReSTIR DI, {} with GI reuse alone", placement, di_mean, gi_mean);
    }
    CC_LOG_INFO("P3, a lamp stretched every other frame: DI confidence {}", di.di_confidence);

    co_await cc::async_settled(sv::background_work(ctx));
}

// ReSTIR DI on a large emissive lamp standing just in front of the back wall, seen in a glossy floor.
// The camera looks down at the floor, whose mirror lobe meets the lamp, so there the bounce ray carries nearly all the lamp's light.
// The walls beside the lamp see it within a fraction of its size, where the bounce ray still carries a third or more.
// The re-shade must weigh its sample by next-event estimation's half of the balance heuristic, formed from the emitter's density.
// A weight of 1 would count the bounce ray's share twice: the floor's reflection of the lamp near double, and a third more on the walls beside it.
// That is several times the 3% the mean is held to.
ASYNC_INVOCABLE_TEST("sv::wavefront_routine - ReSTIR DI keeps the mean of a large emissive lamp over a glossy floor",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif
    auto& ctx = *ctx_h;
    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler that reaches DXIL to build the path-tracing shaders");

    // The box's walls alone, without its blocks and its own lamp, so the large lamp is the one light and nothing hides its reflection.
    auto const white = sv_test::pbr_material{.base_color = tg::vec3f(0.73f, 0.73f, 0.73f),
                                             .metallic = 0.0f,
                                             .roughness = 1.0f,
                                             .emissive = tg::vec3f(0, 0, 0)};
    auto const glossy = sv_test::pbr_material{.base_color = tg::vec3f(0.9f, 0.9f, 0.9f),
                                              .metallic = 1.0f,
                                              .roughness = 0.1f,
                                              .emissive = tg::vec3f(0, 0, 0)};
    auto box = sv_test::cornell_box{.light = sv_test::make_cornell_box().light};
    sv_test::cb_push_quad(box, tg::pos3f(-1, -1, -1), tg::pos3f(1, -1, -1), tg::pos3f(1, -1, 1), tg::pos3f(-1, -1, 1),
                          glossy);
    sv_test::cb_push_quad(box, tg::pos3f(-1, 1, -1), tg::pos3f(-1, 1, 1), tg::pos3f(1, 1, 1), tg::pos3f(1, 1, -1), white);
    sv_test::cb_push_quad(box, tg::pos3f(-1, -1, 1), tg::pos3f(1, -1, 1), tg::pos3f(1, 1, 1), tg::pos3f(-1, 1, 1), white);
    sv_test::cb_push_quad(box, tg::pos3f(-1, -1, -1), tg::pos3f(-1, -1, 1), tg::pos3f(-1, 1, 1), tg::pos3f(-1, 1, -1),
                          white);
    sv_test::cb_push_quad(box, tg::pos3f(1, -1, -1), tg::pos3f(1, 1, -1), tg::pos3f(1, 1, 1), tg::pos3f(1, -1, 1), white);

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto scene = make_scene(resources, cc::move(box));
    // From above the open front, so the floor's mirror direction rises into the back wall where the lamp stands.
    scene.camera = sv::camera::looking_at(tg::pos3d(0, 0.6, -2.0), tg::pos3d(0, -0.6, 0.3));
    scene.camera.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);

    // A square of side 1, 5 cm in front of the back wall, split off its center into triangles of unequal area.
    auto records = cc::vector<sv::shaders::tracer::light_record>();
    auto const apex = tg::pos3f(0.2f, -0.1f, 0.95f);
    auto const corners = cc::array<tg::pos3f>{tg::pos3f(-0.5f, -0.5f, 0.95f), tg::pos3f(0.5f, -0.5f, 0.95f),
                                              tg::pos3f(0.5f, 0.5f, 0.95f), tg::pos3f(-0.5f, 0.5f, 0.95f)};
    auto lamp = cc::vector<tg::pos3f>();
    for (auto i = isize(0); i < 4; ++i)
    {
        lamp.push_back(apex);
        lamp.push_back(corners[i]);
        lamp.push_back(corners[(i + 1) % 4]);
    }
    add_emissive_lamp(resources, scene, records, cc::move(lamp), 8.0f, 1.0f);
    place_lamps(resources, scene, records);
    REQUIRE(scene.lights.records.size() == 1);

    constexpr int frames = 48;
    auto const plain = co_await run_restir(ctx, resources, scene, frames, {.history = false});
    auto const di = co_await run_restir(ctx, resources, scene, frames, {.history = true, .spatial = true, .di = true});
    auto const temporal = co_await run_restir(ctx, resources, scene, frames, {.history = true, .di = true});
    auto const plain_mean = mean_luminance_of(plain.mean);
    auto const di_mean = mean_luminance_of(di.mean);
    auto const temporal_mean = mean_luminance_of(temporal.mean);
    CHECK(tg::abs(di_mean - plain_mean) < 0.03f * plain_mean)
        .context(cc::format("a large emissive lamp: {} with ReSTIR DI, {} without", di_mean, plain_mean));
    CHECK(tg::abs(temporal_mean - plain_mean) < 0.03f * plain_mean)
        .context(cc::format("without the spatial pass: {} with ReSTIR DI, {} without", temporal_mean, plain_mean));
    CC_LOG_INFO("P3 with a large emissive lamp over a glossy floor: means {} with ReSTIR DI, {} without", di_mean,
                plain_mean);

    co_await cc::async_settled(sv::background_work(ctx));
}
