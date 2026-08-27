#include "viewer_test_env.hh"

#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-shader-library/shader_library.hh>
#include <shaped-viewer/all.hh>
#include <sv_test_shaders.hh>

// What the OpenPBR closure in shaders/openpbr.hlsli actually RETURNS, measured rather than assumed.
//
// Every other GPU test in this library asserts that something ran.
// These assert on the numbers that came back, through `shaders/bsdf_probe.hlsl` — three estimators whose expected value is
// known in closed form, so a tolerance here is a statement about a named approximation and nothing else:
//
//   - the directional albedo, which must not exceed 1 and must REACH 1 for a surface that absorbs nothing,
//   - the mass `bsdf_pdf` claims against what `bsdf_sample_direction` draws, which may not exceed 1,
//   - `f(wo, wi)` against `f(wi, wo)`, which Helmholtz reciprocity requires to be equal.
//
// A lobe added to the closure belongs in `surfaces_under_test` below, and is then held to all three at once.

namespace
{
using namespace cc::primitive_defines;

/// `sv::surface` from shaders/openpbr.hlsli, lane-for-lane — keep the two in lockstep.
/// The `probe_echo` check below is what holds them there: it reads three fields back through the GPU's own decode.
struct probe_surface
{
    float base_weight = 1.0f;
    tg::vec3f base_color = tg::vec3f(0.8f, 0.8f, 0.8f);
    float base_metalness = 0.0f;
    float base_diffuse_roughness = 0.0f;

    float specular_weight = 1.0f;
    tg::vec3f specular_color = tg::vec3f(1, 1, 1);
    float specular_roughness = 0.3f;
    float specular_roughness_anisotropy = 0.0f;
    float specular_ior = 1.5f;

    float transmission_weight = 0.0f;
    tg::vec3f transmission_color = tg::vec3f(1, 1, 1);
    float transmission_depth = 0.0f;
    tg::vec3f transmission_scatter = tg::vec3f(0, 0, 0);
    float transmission_scatter_anisotropy = 0.0f;
    float transmission_dispersion_scale = 0.0f;
    float transmission_dispersion_abbe_number = 20.0f;

    float subsurface_weight = 0.0f;
    tg::vec3f subsurface_color = tg::vec3f(0.8f, 0.8f, 0.8f);
    tg::vec3f subsurface_radius = tg::vec3f(1.0f, 0.5f, 0.25f);
    float subsurface_radius_scale = 0.1f;
    float subsurface_scatter_anisotropy = 0.0f;

    float coat_weight = 0.0f;
    tg::vec3f coat_color = tg::vec3f(1, 1, 1);
    float coat_roughness = 0.0f;
    float coat_roughness_anisotropy = 0.0f;
    float coat_ior = 1.6f;
    float coat_darkening = 1.0f;

    float fuzz_weight = 0.0f;
    tg::vec3f fuzz_color = tg::vec3f(1, 1, 1);
    float fuzz_roughness = 0.5f;

    float thin_film_weight = 0.0f;
    float thin_film_thickness = 500.0f;
    float thin_film_ior = 1.4f;

    float emission_luminance = 0.0f;
    tg::vec3f emission_color = tg::vec3f(1, 1, 1);

    float geometry_thin_walled = 0.0f;
    tg::vec3f geometry_normal = tg::vec3f(0, 0, 1);
    tg::vec3f geometry_coat_normal = tg::vec3f(0, 0, 1);
    float geometry_opacity = 1.0f;
    tg::vec4f geometry_tangent_frame = tg::vec4f(0, 0, 0, 1);
    tg::vec3f geometry_tangent = tg::vec3f(1, 0, 0);
    float geometry_handedness = 1.0f;
};

static_assert(sizeof(probe_surface) == 69 * 4, "probe_surface must match sv::surface in shaders/openpbr.hlsli");

/// Which estimator a case runs — mirrors the `probe_*` constants in shaders/bsdf_probe.hlsl.
enum class probe_mode : u32
{
    albedo = 0,
    pdf_norm = 1,
    reciprocity = 2,
    echo = 3,
    medium = 4,
};

/// `sv::probe_case` from shaders/bsdf_probe.hlsl, lane-for-lane.
struct probe_case
{
    tg::vec3f wo = tg::vec3f(0, 0, 1);
    probe_mode mode = probe_mode::albedo;

    u32 samples = 0;
    u32 seed = 1;
    u32 pad0 = 0;
    u32 pad1 = 0;

    probe_surface s = {};

    float pad2 = 0.0f;
    float pad3 = 0.0f;
    float pad4 = 0.0f;
};

static_assert(sizeof(probe_case) == 320, "probe_case must match sv::probe_case in shaders/bsdf_probe.hlsl");

/// How many work items share one case, and how many samples each draws.
///
/// The product is the sample count behind every tolerance below, so lowering either loosens all of them — and the tolerances
/// are meant to be statements about named approximations, not about how many samples were affordable.
/// A quarter of this had the rough-glass pdf reading 2% high purely from noise, which is the same size as the bound it has to
/// clear, so the two were no longer distinguishable.
constexpr int blocks_per_case = 32;
constexpr u32 samples_per_block = 2048;

/// How many cases one dispatch may carry.
///
/// The whole set in a single dispatch is tens of millions of closure evaluations, which on a slow software device runs long
/// enough to trip a driver's watchdog — and that arrives as a lost device rather than as a slow test.
/// Splitting costs one command list per chunk and changes no number, since the cases are independent.
constexpr isize cases_per_dispatch = 16;

[[nodiscard]] float abs_of(float v)
{
    return v < 0.0f ? -v : v;
}

/// One case's summed result, already divided by the samples that produced it.
struct probe_result
{
    tg::vec3f mean = tg::vec3f(0, 0, 0);
    float samples = 0.0f;
};

/// Dispatches `cases` and returns one mean per case.
///
/// Everything is built inline rather than behind a routine: nothing a viewer runs dispatches this shader, so a routine
/// would put test-only machinery in the library.
cc::vector<probe_result> run_probe_chunk(sg::context& ctx, cc::span<probe_case const> cases)
{
    auto const shader = sv_test::shaders::bsdf_probe.compute.BsdfProbe->acquire(ctx);
    (void)cc::try_async_blocking_get(shader); // no async pool here, so drive the compile inline
    if (shader->has_error())
        FAIL(cc::format("the BSDF probe shader did not compile:\n{}", shader->try_error()->underlying().to_string()));

    auto const* const compiled = shader->try_value();
    REQUIRE(compiled != nullptr); // the probe shader must build; without it every check below is vacuous

    auto const group_layout = ctx.cached.acquire_binding_group_layout(compiled->bindings);
    auto const pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {group_layout}});
    auto pipeline = ctx.cached.acquire_compute_pipeline({.shader = *compiled, .layout = pipeline_layout});
    auto const built = cc::async_blocking_get(pipeline);
    REQUIRE(built != nullptr);

    auto const item_count = cases.size() * blocks_per_case;

    auto cmd = ctx.create_command_list();

    auto const case_buffer = ctx.transient.create_buffer<probe_case>(
        cases.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    cmd->upload.data_to_buffer(case_buffer, cases);

    auto const result_buffer = ctx.transient.create_buffer<tg::vec4f>(
        item_count, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto const group = ctx.transient.create_binding_group(
        group_layout, {{.name = "Cases", .view = case_buffer.as_readonly_buffer()},
                       {.name = "Results", .view = result_buffer.as_readwrite_buffer()}});

    cmd->compute.bind_pipeline(*built);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(item_count);

    auto readback = cmd->download.data_from_buffer(result_buffer);

    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch_and_wait_for_idle();
    // An epoch advance drains the GPU but not the readback actor, so this is the only completion guarantee.
    auto const delivered = ctx.wait_for(readback);
    REQUIRE(delivered.has_value());

    auto const items = delivered.value();
    REQUIRE(items.size() == item_count);

    auto out = cc::vector<probe_result>();
    out.resize_to_defaulted(cases.size());
    for (auto i = isize(0); i < item_count; ++i)
    {
        auto& r = out[i / blocks_per_case];
        r.mean += tg::vec3f(items[i][0], items[i][1], items[i][2]);
        r.samples += items[i][3];
    }
    for (auto& r : out)
        r.mean = r.mean / cc::max(r.samples, 1.0f);

    return out;
}

/// Dispatches every case, in chunks small enough that no single dispatch runs long enough to be killed.
cc::vector<probe_result> run_probe(sg::context& ctx, cc::span<probe_case const> cases)
{
    auto out = cc::vector<probe_result>();
    out.reserve(cases.size());

    for (auto begin = isize(0); begin < cases.size(); begin += cases_per_dispatch)
    {
        auto const count = cc::min(cases_per_dispatch, cases.size() - begin);
        for (auto const& r : run_probe_chunk(ctx, cases.subspan({.offset = begin, .size = count})))
            out.push_back(r);
    }
    return out;
}

/// The surfaces every estimator is run against, each a lobe or a combination the closure has to get right on its own.
struct named_surface
{
    cc::string_view name;
    probe_surface s;
};

/// A white furnace needs a surface that absorbs NOTHING, which is what makes its albedo exactly 1.
/// Everything else here is a realistic configuration, held only to energy conservation.
cc::vector<named_surface> surfaces_under_test()
{
    auto out = cc::vector<named_surface>();

    // The two furnace cases: white and lossless, so the expected albedo is 1 rather than merely bounded.
    // The diffuse one turns the specular layer off, because a dielectric over a white substrate still reflects.
    out.push_back({"white diffuse", {.base_color = tg::vec3f(1, 1, 1), .specular_weight = 0.0f}});
    out.push_back(
        {"white metal, rough", {.base_color = tg::vec3f(1, 1, 1), .base_metalness = 1.0f, .specular_roughness = 0.6f}});
    out.push_back({"white metal, smooth",
                   {.base_color = tg::vec3f(1, 1, 1), .base_metalness = 1.0f, .specular_roughness = 0.1f}});

    // Realistic configurations, one per lobe and then in combination.
    out.push_back({"grey dielectric", {}});
    out.push_back({"rough dielectric", {.specular_roughness = 0.8f}});
    out.push_back(
        {"gold", {.base_color = tg::vec3f(1.0f, 0.77f, 0.34f), .base_metalness = 1.0f, .specular_roughness = 0.25f}});
    out.push_back({"oren-nayar diffuse", {.base_diffuse_roughness = 0.9f, .specular_weight = 0.0f}});
    out.push_back({"coated", {.coat_weight = 1.0f, .coat_roughness = 0.15f}});
    out.push_back(
        {"coated, tinted + darkening off",
         {.coat_weight = 1.0f, .coat_color = tg::vec3f(0.9f, 0.5f, 0.3f), .coat_roughness = 0.2f, .coat_darkening = 0.0f}});
    out.push_back({"fuzz", {.fuzz_weight = 1.0f, .fuzz_roughness = 0.4f}});

    // Anisotropy at both ends of its range, plus a rotated tangent — the last is what catches a frame rotation that
    // silently left the basis non-orthonormal, since every estimator here assumes a proper frame.
    out.push_back({"white metal, anisotropic",
                   {.base_color = tg::vec3f(1, 1, 1),
                    .base_metalness = 1.0f,
                    .specular_roughness = 0.5f,
                    .specular_roughness_anisotropy = 0.8f}});
    out.push_back({"anisotropic dielectric", {.specular_roughness = 0.4f, .specular_roughness_anisotropy = 0.95f}});
    out.push_back({"anisotropic coat", {.coat_weight = 1.0f, .coat_roughness = 0.3f, .coat_roughness_anisotropy = 0.7f}});
    // Thin film over both a metal and a dielectric, at a thickness in the first interference order and one well past it.
    out.push_back({"thin film on metal",
                   {.base_color = tg::vec3f(0.9f, 0.9f, 0.9f),
                    .base_metalness = 1.0f,
                    .specular_roughness = 0.2f,
                    .thin_film_weight = 1.0f,
                    .thin_film_thickness = 400.0f}});
    out.push_back({"thin film on dielectric",
                   {.specular_roughness = 0.25f, .thin_film_weight = 1.0f, .thin_film_thickness = 250.0f}});
    out.push_back({"thin film, thick",
                   {.base_metalness = 1.0f,
                    .specular_roughness = 0.3f,
                    .thin_film_weight = 0.6f,
                    .thin_film_thickness = 1400.0f,
                    .thin_film_ior = 2.0f}});

    // A coat whose normal is not the base's, which is the case the coat's own frame exists for.
    out.push_back({"coat, tilted normal",
                   {.base_color = tg::vec3f(0.4f, 0.2f, 0.15f),
                    .coat_weight = 1.0f,
                    .coat_roughness = 0.2f,
                    .geometry_coat_normal = tg::vec3f(0.4f, 0.2f, 0.894f)}});
    out.push_back({"coat, steeply tilted normal",
                   {.coat_weight = 1.0f,
                    .coat_roughness = 0.35f,
                    .coat_roughness_anisotropy = 0.5f,
                    .geometry_coat_normal = tg::vec3f(0.7f, -0.3f, 0.648f)}});

    // The transparent base: refracting glass, a tinted crossing, an absorbing interior, and a thin wall that does neither.
    out.push_back({"glass, smooth", {.specular_roughness = 0.08f, .transmission_weight = 1.0f}});
    out.push_back({"glass, rough", {.specular_roughness = 0.45f, .transmission_weight = 1.0f}});
    out.push_back(
        {"glass, tinted crossing",
         {.specular_roughness = 0.2f, .transmission_weight = 1.0f, .transmission_color = tg::vec3f(0.4f, 0.85f, 0.6f)}});
    out.push_back({"glass, absorbing interior",
                   {.specular_roughness = 0.15f,
                    .transmission_weight = 1.0f,
                    .transmission_color = tg::vec3f(0.3f, 0.7f, 0.9f),
                    .transmission_depth = 0.5f}});
    out.push_back({"thin walled",
                   {.specular_roughness = 0.3f,
                    .transmission_weight = 1.0f,
                    .transmission_color = tg::vec3f(0.9f, 0.75f, 0.6f),
                    .geometry_thin_walled = 1.0f}});
    out.push_back({"half transmissive, coated",
                   {.base_color = tg::vec3f(0.3f, 0.4f, 0.5f),
                    .specular_roughness = 0.25f,
                    .transmission_weight = 0.5f,
                    .coat_weight = 1.0f,
                    .coat_roughness = 0.1f}});

    // The subsurface base, which refracts through the same interface the transparent one does and differs only in the
    // interior beyond it — so what the closure has to get right here is the split, not the walk.
    out.push_back({"subsurface", {.specular_roughness = 0.4f, .subsurface_weight = 1.0f}});
    out.push_back({"subsurface, saturated",
                   {.specular_roughness = 0.3f,
                    .subsurface_weight = 1.0f,
                    .subsurface_color = tg::vec3f(0.9f, 0.35f, 0.28f),
                    .subsurface_radius = tg::vec3f(1.0f, 0.35f, 0.18f),
                    .subsurface_radius_scale = 0.25f,
                    .subsurface_scatter_anisotropy = 0.6f}});
    out.push_back({"half subsurface over diffuse",
                   {.base_color = tg::vec3f(0.5f, 0.45f, 0.4f), .specular_roughness = 0.35f, .subsurface_weight = 0.5f}});
    out.push_back({"subsurface under a coat",
                   {.specular_roughness = 0.3f,
                    .subsurface_weight = 1.0f,
                    .subsurface_color = tg::vec3f(0.85f, 0.6f, 0.5f),
                    .coat_weight = 1.0f,
                    .coat_roughness = 0.12f}});

    // Dispersion, which changes only the angle a wavelength refracts through — the probe carries all three, so what it
    // pins is that turning it on breaks neither the energy nor the pdf.
    // An interface at index ~1, which is where the reflection's Fresnel and the refraction's have to agree exactly or the
    // surface invents light: Schlick's grazing tail climbs to white where the exact relation goes to nothing.
    out.push_back(
        {"index-matched glass", {.specular_roughness = 0.15f, .specular_ior = 1.001f, .transmission_weight = 1.0f}});

    out.push_back({"scattering interior",
                   {.specular_roughness = 0.2f,
                    .transmission_weight = 1.0f,
                    .transmission_depth = 0.4f,
                    .transmission_scatter = tg::vec3f(2.0f, 2.0f, 2.0f),
                    .transmission_scatter_anisotropy = 0.4f}});

    out.push_back({"dispersive glass",
                   {.specular_roughness = 0.1f,
                    .transmission_weight = 1.0f,
                    .transmission_dispersion_scale = 1.0f,
                    .transmission_dispersion_abbe_number = 20.0f}});

    out.push_back({"anisotropic, tangent rotated",
                   {.specular_roughness = 0.4f,
                    .specular_roughness_anisotropy = 0.9f,
                    .geometry_tangent = tg::vec3f(0.6f, 0.8f, 0.0f)}});
    out.push_back({"everything at once",
                   {.base_color = tg::vec3f(0.5f, 0.3f, 0.2f),
                    .base_metalness = 0.4f,
                    .base_diffuse_roughness = 0.5f,
                    .specular_roughness = 0.35f,
                    .coat_weight = 0.8f,
                    .coat_roughness = 0.25f,
                    .fuzz_weight = 0.6f,
                    .fuzz_roughness = 0.5f}});

    return out;
}

/// The outgoing directions each surface is measured from: normal incidence, a slant, and near-grazing.
/// Grazing is where the layer coupling and the Fresnel terms are least forgiving, which is why it is not left out.
constexpr tg::vec3f probe_directions[] = {
    tg::vec3f(0.0f, 0.0f, 1.0f),
    tg::vec3f(0.5f, 0.0f, 0.8660254f),
    tg::vec3f(0.9396926f, 0.0f, 0.3420201f),
};

/// A dx12 WARP context, or nullptr where none is available.
sg::context_handle make_probe_context()
{
    auto r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (r.has_error())
        return nullptr;
    return r.value();
}
} // namespace

// On the main thread for the same reason the path-traced tests are: the shader compile is driven inline through
// `try_async_blocking_get`, which does not complete from inside a pool worker.
TEST("sv - OpenPBR closure, measured", nx::config::main_thread)
{
    auto const ctx_h = make_probe_context();
    if (ctx_h == nullptr)
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context& ctx = *ctx_h;

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the probe shader");

    // The probe lives in the test binary's own package, so the shared library has to be told about it.
    // Once per process: `add_package` writes the generated globals the asset handles are read through.
    static auto const registered = [&]
    {
        env.lib->add_package(sv_test::shaders::package());
        return true;
    }();
    CHECK(registered);

    auto const surfaces = surfaces_under_test();

    // The layout pin, first: every number below is decoded through `probe_case`, so a packing disagreement would
    // show up as a wrong result with nothing pointing at the cause.
    {
        auto echo = probe_case{.mode = probe_mode::echo, .samples = 1};
        echo.s.base_color = tg::vec3f(0.125f, 0, 0);
        echo.s.specular_roughness = 0.375f;
        echo.s.geometry_tangent_frame = tg::vec4f(0, 0, 0, 0.625f);

        auto const r = run_probe(ctx, cc::span<probe_case const>(&echo, 1));
        REQUIRE(r.size() == 1);
        CHECK(r[0].mean[0] == 0.125f).context("base_color.x, the first float3 in the struct");
        CHECK(r[0].mean[1] == 0.375f).context("specular_roughness, past two float3s");
        CHECK(r[0].mean[2] == 0.625f).context("the tangent frame's w, the last float4");
    }

    // Build one case per (surface, direction, mode) and measure them all in one dispatch.
    auto cases = cc::vector<probe_case>();
    for (auto const& ns : surfaces)
        for (auto const& wo : probe_directions)
            for (auto const mode :
                 {probe_mode::albedo, probe_mode::pdf_norm, probe_mode::reciprocity, probe_mode::medium})
                cases.push_back({.wo = wo, .mode = mode, .samples = samples_per_block, .seed = 7u, .s = ns.s});

    auto const results = run_probe(ctx, cases);
    REQUIRE(results.size() == cases.size());

    auto const modes_per_direction = 4;
    auto const directions = isize(sizeof(probe_directions) / sizeof(probe_directions[0]));

    for (auto si = isize(0); si < surfaces.size(); ++si)
    {
        auto const& ns = surfaces[si];
        auto const is_furnace = ns.name.starts_with("white ");

        for (auto di = isize(0); di < directions; ++di)
        {
            auto const base = (si * directions + di) * modes_per_direction;
            auto const& albedo = results[base + 0];
            auto const& pdf_norm = results[base + 1];
            auto const& reciprocity = results[base + 2];
            auto const& medium = results[base + 3];

            // A message per case, because a bare failing CHECK in a triple loop says nothing about which surface broke.
            auto const where = cc::format("{} @ wo.z = {}", ns.name, probe_directions[di][2]);

            // Energy conservation: a closure may not return more light than it received.
            //
            // The margin is two named approximations rather than Monte-Carlo error, which at this sample budget is well
            // under a percent.
            // Turquin's analytic energy compensation overshoots a white metal by about 3%, and the Conty-Estevez sheen
            // reflects about 6% more at grazing than `sheen_albedo` charges the layers below it for.
            // Both are the fits the viewer TODO names tabulated albedos as the replacement for, so this bound is what
            // will tighten when they land.
            for (auto c = 0; c < 3; ++c)
                CHECK(albedo.mean[c] <= 1.06f).context(where).dump("albedo", albedo.mean);

            // The white furnace: a surface that absorbs nothing reflects everything.
            // This is the assertion energy compensation exists to satisfy — without it a rough metal loses the
            // multiple-scattering energy and lands around 0.8.
            //
            // The binding case for this tolerance is the ANISOTROPIC metal at grazing, which lands about 7% low.
            // Lazarov's directional-albedo fit is isotropic and `alpha_iso` reduces the stretched lobe to the round one of
            // the same solid angle, which is the closest an isotropic fit can come: a lobe stretched across the view loses
            // more multiple-scattering energy than that reduction knows about.
            // Only a tabulated albedo over both axes closes it, which is the same entry the two overshoots above wait on.
            if (is_furnace)
                for (auto c = 0; c < 3; ++c)
                    CHECK(abs_of(albedo.mean[c] - 1.0f) <= 0.08f).context(where).dump("albedo", albedo.mean);

            // What `bsdf_pdf` claims, measured against what `bsdf_sample_direction` draws.
            //
            // The upper bound is the real requirement: a pdf that claims more mass than exists makes every
            // multiple-importance weight it feeds too small, and the image is biased in a way no amount of accumulation
            // fixes.
            // Below 1 is legitimate and expected — a GGX visible-normal sample can reflect BELOW the horizon, and that
            // mass is lost rather than renormalized, which for a rough lobe is around a tenth of it.
            // The floor is a sanity bound: a pdf that collapsed entirely would sit near zero.
            CHECK(pdf_norm.mean[0] <= 1.02f).context(where).dump("pdf mass", pdf_norm.mean[0]);
            CHECK(pdf_norm.mean[0] >= 0.85f).context(where).dump("pdf mass", pdf_norm.mean[0]);

            // A lobe that returns nothing at all passes every bound above, so this is what separates "conserves energy"
            // from "was never wired up".
            auto const brightness = albedo.mean[0] + albedo.mean[1] + albedo.mean[2];
            CHECK(brightness > 0.02f).context(where).dump("albedo", albedo.mean);

            // Which interior the sampled directions crossed into, which is what the integrator switches its medium on.
            // A surface that transmits must reach one, and must reach the one it actually described.
            auto const& probe_s = ns.s;
            if (probe_s.transmission_weight > 0.0f && probe_s.geometry_thin_walled == 0.0f)
                CHECK(medium.mean[1] > 0.0f).context(where).dump("into transmission", medium.mean[1]);
            else
                CHECK(medium.mean[1] == 0.0f).context(where).dump("into transmission", medium.mean[1]);

            if (probe_s.subsurface_weight > 0.0f && probe_s.transmission_weight < 1.0f)
                CHECK(medium.mean[2] > 0.0f).context(where).dump("into subsurface", medium.mean[2]);
            else
                CHECK(medium.mean[2] == 0.0f).context(where).dump("into subsurface", medium.mean[2]);

            // Helmholtz reciprocity, as a fraction of the magnitudes compared — an absolute difference would be
            // dominated by whichever surface happens to be brightest.
            auto const relative = reciprocity.mean[0] / cc::max(reciprocity.mean[1], 1e-9f);
            CHECK(relative <= 1e-3f).context(where).dump("relative asymmetry", relative);
        }
    }
}
