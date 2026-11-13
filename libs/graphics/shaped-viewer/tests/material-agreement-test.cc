#include "agreement_test_support.hh"

#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>

// Every builtin material type, on a sphere in the lit Cornell box, drawn by each mode and held to the megakernel's image.
//
// The wavefront shades through a kernel of its own per permutation, so it is held to the megakernel for every material.
// The forward hybrid lights a material's full closure at the primary hit, so it is held to the megakernel's path one bounce
// longer wherever the closure is one the hybrid's bounce handles.
// The deferred hybrid lights the thin G-buffer's reduction of the closure, so it is held only where that reduction is the
// closure itself: base color, metalness, roughness and specular weight.
// Where a mode approximates on purpose, its difference is measured and drawn rather than failed.

namespace
{
using namespace cc::primitive_defines;
namespace agreement = sv_test::agreement;
using agreement::enforce;

/// One material on the sphere, and which of the hybrid's lightings it is held to.
struct material_case
{
    cc::string_view name;
    cc::string_view type;
    cc::vector<sv::material_attribute_binding> bindings;
    enforce forward = enforce::tiles;
    enforce deferred = enforce::tiles;
    /// How far the forward hybrid may stray from the megakernel beyond noise, relative to it.
    /// A layer the hybrid's bounce does not see moves the light the sphere throws onto the block beside it, by up to a tenth.
    f64 forward_floor = 0.02;
};

[[nodiscard]] cc::vector<material_case> all_cases()
{
    using sv::material_attribute_binding;
    auto out = cc::vector<material_case>();
    out.push_back({.name = "pbr-matte",
                   .type = sv::builtin_material::pbr,
                   .bindings = {material_attribute_binding::of("base_color", tg::vec3f(0.7f, 0.5f, 0.3f)),
                                material_attribute_binding::of("roughness", 1.0f)}});
    out.push_back({.name = "pbr-metal",
                   .type = sv::builtin_material::pbr,
                   .bindings = {material_attribute_binding::of("base_color", tg::vec3f(0.95f, 0.65f, 0.25f)),
                                material_attribute_binding::of("metallic", 1.0f),
                                material_attribute_binding::of("roughness", 0.35f)}});
    out.push_back({.name = "openpbr-default", .type = sv::builtin_material::openpbr, .bindings = {}});
    // A coat, a fuzz and a thin film are layers the thin G-buffer has no room for.
    out.push_back({.name = "openpbr-coat",
                   .type = sv::builtin_material::openpbr,
                   .bindings = {material_attribute_binding::of("base_color", tg::vec3f(0.6f, 0.1f, 0.1f)),
                                material_attribute_binding::of("coat_weight", 1.0f),
                                material_attribute_binding::of("coat_roughness", 0.1f)},
                   .deferred = enforce::report_only,
                   .forward_floor = 0.05});
    out.push_back({.name = "openpbr-fuzz",
                   .type = sv::builtin_material::openpbr,
                   .bindings = {material_attribute_binding::of("base_color", tg::vec3f(0.2f, 0.2f, 0.5f)),
                                material_attribute_binding::of("fuzz_weight", 1.0f),
                                material_attribute_binding::of("fuzz_color", tg::vec3f(0.9f, 0.9f, 1.0f))},
                   .deferred = enforce::report_only,
                   .forward_floor = 0.05});
    out.push_back({.name = "openpbr-thin-film",
                   .type = sv::builtin_material::openpbr,
                   .bindings = {material_attribute_binding::of("base_metalness", 1.0f),
                                material_attribute_binding::of("specular_roughness", 0.2f),
                                material_attribute_binding::of("thin_film_weight", 1.0f)},
                   .deferred = enforce::report_only,
                   .forward_floor = 0.05});
    // The hybrid's bounce crosses glass at one fixed index and loses each interface's reflection, and ends at a subsurface
    // refraction, so neither lighting is held for them.
    out.push_back({.name = "openpbr-glass",
                   .type = sv::builtin_material::openpbr,
                   .bindings = {material_attribute_binding::of("transmission_weight", 1.0f),
                                material_attribute_binding::of("specular_roughness", 0.0f)},
                   .forward = enforce::report_only,
                   .deferred = enforce::report_only});
    // issue: feat-sv-hybrid-subsurface
    out.push_back({.name = "openpbr-subsurface",
                   .type = sv::builtin_material::openpbr,
                   .bindings = {material_attribute_binding::of("subsurface_weight", 1.0f),
                                material_attribute_binding::of("subsurface_color", tg::vec3f(0.9f, 0.5f, 0.4f))},
                   .forward = enforce::report_only,
                   .deferred = enforce::report_only});
    out.push_back({.name = "unlit",
                   .type = sv::builtin_material::unlit,
                   .bindings = {material_attribute_binding::of("color", tg::vec3f(0.2f, 0.6f, 0.9f))}});
    return out;
}

/// The cases a default run draws: one per type, the metal's lobe the roughest test of a closure; every case under --thorough.
/// Each case compiles a hit group, a wavefront kernel and a forward stage of its own, and a cold forward stage takes half a minute.
[[nodiscard]] cc::vector<material_case> cases_to_draw()
{
    auto out = all_cases();
    if (nx::is_thorough())
        return out;
    auto narrowed = cc::vector<material_case>();
    for (auto& c : out)
        if (c.name == "pbr-metal" || c.name == "openpbr-coat" || c.name == "unlit")
            narrowed.push_back(cc::move(c));
    return narrowed;
}
} // namespace

ASYNC_INVOCABLE_TEST("sv agreement - every material type draws the same sphere in every mode",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!agreement::can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither the tracers nor the hybrid build");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const box = agreement::make_lit_box(resources);
    auto& lib = sv_test::shared_material_library();

    for (auto const& c : cases_to_draw())
    {
        auto const type = lib.acquire_type(c.type);
        REQUIRE(type.has_value()).dump("type", c.type);
        auto const material = lib.acquire(sv::material::create(cc::string(c.name), type.value(), c.bindings));
        // In front of both blocks, resting on the floor, about half the image's height from the camera.
        auto items = box.items;
        items.push_back(resources.acquire_scene_item(
            sv::mesh{.name = cc::string(c.name),
                     .geometry = sv_test::sphere_geometry(0.35f, 32, 48, tg::vec3f(0.0f, -0.64f, -0.55f)),
                     .material = material}));
        resources.wait_for_pending_uploads();

        auto const full = co_await agreement::converge(ctx, resources, items, box.camera,
                                                       {.name = c.name, .mode = sv::render_mode::megakernel}, 1024,
                                                       agreement::k_reference_restarts);
        auto const one_bounce = co_await agreement::converge(
            ctx, resources, items, box.camera, {.name = c.name, .mode = sv::render_mode::megakernel, .max_bounces = 2},
            1024, agreement::k_reference_restarts);
        auto const wavefront = co_await agreement::converge(ctx, resources, items, box.camera,
                                                            {.name = c.name, .mode = sv::render_mode::wavefront}, 1024);
        auto const forward = co_await agreement::converge(
            ctx, resources, items, box.camera,
            {.name = c.name, .mode = sv::render_mode::hybrid, .lighting = sv::hybrid_lighting::forward}, 512);
        auto const deferred = co_await agreement::converge(
            ctx, resources, items, box.camera,
            {.name = c.name, .mode = sv::render_mode::hybrid, .lighting = sv::hybrid_lighting::deferred}, 512);

        agreement::check_agreement(cc::format("material-{}-wavefront", c.name), wavefront, full, 0.01);
        agreement::check_agreement(cc::format("material-{}-forward", c.name), forward, one_bounce, c.forward_floor,
                                   c.forward);
        agreement::check_agreement(cc::format("material-{}-deferred", c.name), deferred, one_bounce, 0.02, c.deferred);
    }
}
