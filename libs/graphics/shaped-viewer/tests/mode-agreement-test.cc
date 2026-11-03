#include "agreement_test_support.hh"

#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>

// Every render mode, drawn through the viewer as a window draws it, held tile by tile to the megakernel's image.
//
// The megakernel is the reference because its own tests hold it to closed forms: furnaces, light units, emitter series.
// A mode is held to what it promises, not to the full transport: a hybrid of n bounces to the megakernel's path of n + 1,
// since the tracer's path length counts the primary hit.

namespace
{
using namespace cc::primitive_defines;
namespace agreement = sv_test::agreement;

/// How many restarts a reference runs before its first half, so its seeds are none the contender draws.
constexpr int k_reference_restarts = 3;

/// The Cornell box lit by its lamp alone, as the view-level tests place it, and the camera it fills.
struct lit_box
{
    cc::vector<sv::scene_item> items;
    sv::camera camera;
};

/// `lamp_drop` lowers the lamp from its 2 mm under the ceiling.
[[nodiscard]] lit_box make_lit_box(sv::gpu_resource_manager& resources, f32 lamp_drop = 0.0f)
{
    auto box = sv_test::make_cornell_box();
    auto const lamp_vertices = 3 * sv_test::cornell_lamp_triangles;
    for (auto i = box.positions.size() - lamp_vertices; i < box.positions.size(); ++i)
        box.positions[i] = box.positions[i] - tg::vec3f(0, lamp_drop, 0);
    auto const lit = sv_test::as_lit_meshes(box, 15.0f);
    auto out = lit_box{.camera = sv_test::cornell_camera()};
    out.items.push_back(resources.acquire_scene_item(lit.enclosure));
    out.items.push_back(resources.acquire_scene_item(lit.lamp));
    resources.wait_for_pending_uploads();
    // Narrow enough that the box fills the view, so no tile is half background.
    out.camera.projection.vertical_fov = tg::angle_d::make_from_degree(30.0);
    return out;
}

[[nodiscard]] bool can_trace(sg::context& ctx)
{
    return sv_test::can_run(ctx) && ctx.supports(sg::feature::ray_query);
}
} // namespace

// The comparison's own control: a megakernel cut short at its direct light differs from the full one exactly by the
// indirect light, a few tens of percent of every wall, so a comparison that passes it would pass anything.
ASYNC_INVOCABLE_TEST("sv agreement - the comparison tells the Cornell box's direct light from its full transport",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so the tracer does not build");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const box = make_lit_box(resources);

    auto const full = co_await agreement::converge(ctx, resources, box.items, box.camera,
                                                   {.name = "full", .mode = sv::render_mode::megakernel}, 1024,
                                                   k_reference_restarts);
    auto const direct
        = co_await agreement::converge(ctx, resources, box.items, box.camera,
                                       {.name = "direct", .mode = sv::render_mode::megakernel, .max_bounces = 1}, 1024);
    auto const report = agreement::compare_tiles(direct, full, 0.01, 5.0);
    CC_LOG_INFO("direct light against the full transport: {} of {} tiles past 5 standard errors, worst {:.1f}",
                report.failed, report.compared, report.worst_z);
    CHECK(report.failed * 2 > report.compared).dump("failed", report.failed).dump("compared", report.compared);
}

ASYNC_INVOCABLE_TEST("sv agreement - the wavefront converges to the megakernel's Cornell box, tile by tile",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither tracer builds");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const box = make_lit_box(resources);

    auto const reference = co_await agreement::converge(ctx, resources, box.items, box.camera,
                                                        {.name = "megakernel", .mode = sv::render_mode::megakernel},
                                                        1024, k_reference_restarts);
    auto const wavefront = co_await agreement::converge(
        ctx, resources, box.items, box.camera, {.name = "wavefront", .mode = sv::render_mode::wavefront}, 1024);
    // One estimator split into stages: nothing but float rounding may separate the two.
    agreement::check_agreement("wavefront-vs-megakernel", wavefront, reference, 0.01);
}

// A firefly is one sample worth far more than the rest, so it lands in one of two independent halves and not the other.
// Each is a weight that is not bounded: light sampling alone, without MIS, near a large emitter is the one this finds.
// The lamp hangs 5 cm below the ceiling, so bounces reach the ceiling above it, which its two-sided face lights from up close;
// where MIS hands that light to the BSDF sample the path is quiet, and where a last vertex samples the light alone it is not.
// Besides how it looks, it is what blunts the comparisons: one firefly in a tile widens its tolerance past a 15% bias.
ASYNC_INVOCABLE_TEST("sv agreement - no mode draws fireflies under a lamp hanging below the ceiling",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither tracer builds");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const box = make_lit_box(resources, 0.05f);

    agreement::mode_config const modes[] = {
        {.name = "megakernel-fireflies", .mode = sv::render_mode::megakernel},
        {.name = "megakernel-direct-fireflies", .mode = sv::render_mode::megakernel, .max_bounces = 1},
        {.name = "megakernel-one-bounce-fireflies", .mode = sv::render_mode::megakernel, .max_bounces = 2},
        {.name = "wavefront-fireflies", .mode = sv::render_mode::wavefront},
        {.name = "hybrid-deferred-fireflies", .mode = sv::render_mode::hybrid},
        {.name = "hybrid-forward-fireflies", .mode = sv::render_mode::hybrid, .lighting = sv::hybrid_lighting::forward},
    };
    for (auto const& m : modes)
    {
        auto const image = co_await agreement::converge(ctx, resources, box.items, box.camera, m, 1024);
        auto mean = cc::vector<tg::vec4f>();
        for (auto i = isize(0); i < image.first.size(); ++i)
            mean.push_back(image.mean_at(i));
        // At a thousand samples a half, halves five neighborhoods apart hold one sample worth five thousand of theirs.
        // The ceiling right above the lamp is lit some hundred times brighter than the walls, so a bounce landing there is
        // worth up to some three thousand honestly; a light sample left unbalanced near the lamp was worth 14000 to 28000.
        // The lamp is 15 nits and no wall reaches 2, so a neighborhood above that is the lamp's edge.
        auto const found = agreement::fireflies_of(image, 5.0, 2.0);

        auto worst = agreement::firefly{};
        for (auto const& f : found)
            if (f.ratio > worst.ratio)
                worst = f;
        CC_LOG_INFO(
            "{}: {} fireflies at {} samples per pixel a half; the worst halves {:.1f}x its neighbors apart at ({}, "
            "{}), a sample worth {:.0f} of theirs",
            m.name, found.size(), image.samples, worst.ratio, worst.pixel[0], worst.pixel[1], worst.sample_weight);

        // The converged image, and the same with every firefly marked red.
        auto const exposure = agreement::exposure_of(mean);
        auto const shown = agreement::display_of(mean, exposure);
        auto marked = shown;
        for (auto const& f : found)
            marked[isize(f.pixel[1]) * agreement::k_size[0] + f.pixel[0]] = tg::vec3f(1, 0, 0);
        cc::vector<tg::vec3f> const panels[] = {shown, marked};
        agreement::write_panels(m.name, panels);

        CHECK(found.empty())
            .dump("mode", m.name)
            .dump("fireflies", found.size())
            .dump("worst ratio", worst.ratio)
            .dump("worst sample weight", worst.sample_weight);
    }
}

ASYNC_INVOCABLE_TEST("sv agreement - the hybrid lights the Cornell box as the megakernel's path one bounce longer, "
                     "forward and deferred",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither the hybrid nor its reference builds");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const box = make_lit_box(resources);

    struct contender
    {
        cc::string_view name;
        sv::hybrid_lighting lighting = sv::hybrid_lighting::deferred;
        i32 bounces = 1;
    };
    contender const contenders[] = {
        {.name = "hybrid-forward-1-vs-megakernel", .lighting = sv::hybrid_lighting::forward, .bounces = 1},
        {.name = "hybrid-deferred-1-vs-megakernel", .lighting = sv::hybrid_lighting::deferred, .bounces = 1},
        {.name = "hybrid-deferred-2-vs-megakernel", .lighting = sv::hybrid_lighting::deferred, .bounces = 2},
        {.name = "hybrid-forward-2-vs-megakernel", .lighting = sv::hybrid_lighting::forward, .bounces = 2},
    };
    auto references = cc::vector<agreement::converged>();
    for (auto const bounces : {1, 2})
        references.push_back(co_await agreement::converge(
            ctx, resources, box.items, box.camera,
            {.name = "megakernel", .mode = sv::render_mode::megakernel, .max_bounces = bounces + 1}, 1024,
            k_reference_restarts));

    for (auto const& c : contenders)
    {
        auto const hybrid = co_await agreement::converge(
            ctx, resources, box.items, box.camera,
            {.name = c.name, .mode = sv::render_mode::hybrid, .lighting = c.lighting, .hybrid_bounces = c.bounces}, 512);
        // The box is matte throughout, so the bounce's reduced material is the material itself, and only noise is left.
        agreement::check_agreement(c.name, hybrid, references[c.bounces - 1], 0.02);
    }
}
