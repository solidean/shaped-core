#include "agreement_test_support.hh"

#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>

// A sphere in a furnace: an environment of radiance 1 in every direction, and no light.
// A convex object sees nothing but the environment, so what it reflects is its albedo, known in advance at every viewing angle.
// A material reflecting all it receives disappears into the background, and none may come out brighter than it.
// The agreement tests hold the modes to each other, so an error they all share passes there and fails here.

namespace
{
using namespace cc::primitive_defines;
namespace agreement = sv_test::agreement;
using agreement::enforce;

/// Viewing angle bands, center to rim, so a lobe that gains energy only at grazing angles is not averaged away.
constexpr int k_rings = 6;

/// How far a ring's mean may stray beyond noise, relative to what it should be.
constexpr f64 k_floor = 0.015;

/// Whether the sphere must reflect exactly its albedo, or may lose some of it.
/// A single-scattering microfacet lobe loses what bounces between its facets, which only a multiple-scattering lobe gives back.
enum class bound : u8
{
    exact,
    at_most,
};

struct furnace_case
{
    cc::string_view name;
    cc::string_view type;
    cc::vector<sv::material_attribute_binding> bindings;
    /// what the sphere reflects of the environment
    f64 albedo = 1.0;
    bound how = bound::exact;
    /// whether a default run draws it in every mode rather than in the megakernel alone
    bool every_mode = false;
    /// the hybrid's lightings, which approximate some closures on purpose
    enforce hybrid = enforce::tiles;
    /// a known defect's issue, which makes every mode report rather than fail until it closes
    cc::string_view known;
};

[[nodiscard]] cc::vector<furnace_case> all_cases()
{
    using sv::material_attribute_binding;
    auto const of = [](cc::string_view name, auto value) { return material_attribute_binding::of(name, value); };
    auto const white = tg::vec3f(1.0f, 1.0f, 1.0f);
    auto out = cc::vector<furnace_case>();
    // First, since its image is the mask every other case is read through.
    out.push_back({.name = "diffuse-half",
                   .type = sv::builtin_material::openpbr,
                   .bindings = {of("base_color", tg::vec3f(0.5f, 0.5f, 0.5f)), of("specular_weight", 0.0f)},
                   .albedo = 0.5,
                   .every_mode = true});
    out.push_back({.name = "diffuse-white",
                   .type = sv::builtin_material::openpbr,
                   .bindings = {of("base_color", white), of("specular_weight", 0.0f)}});
    // A dielectric layer over a white base passes on what it does not reflect.
    out.push_back({.name = "plastic-white",
                   .type = sv::builtin_material::openpbr,
                   .bindings = {of("base_color", white), of("specular_roughness", 0.3f)},
                   .how = bound::at_most,
                   .every_mode = true});
    out.push_back({.name = "pbr-white",
                   .type = sv::builtin_material::pbr,
                   .bindings = {of("base_color", white), of("roughness", 0.5f)},
                   .how = bound::at_most});
    out.push_back({.name = "metal-white-mirror",
                   .type = sv::builtin_material::openpbr,
                   .bindings = {of("base_color", white), of("base_metalness", 1.0f), of("specular_roughness", 0.0f)}});
    out.push_back({.name = "metal-white-rough",
                   .type = sv::builtin_material::openpbr,
                   .bindings = {of("base_color", white), of("base_metalness", 1.0f), of("specular_roughness", 0.8f)},
                   .how = bound::at_most});
    out.push_back({.name = "coat-white",
                   .type = sv::builtin_material::openpbr,
                   .bindings = {of("base_color", white), of("coat_weight", 1.0f), of("coat_roughness", 0.2f)},
                   .how = bound::at_most,
                   .hybrid = enforce::report_only});
    // `sheen_albedo` charges the base less than the sheen reflects toward grazing, so the rim comes out brighter than the environment.
    // issue: feat-sv-openpbr-tabulated-albedo
    out.push_back({.name = "fuzz-white",
                   .type = sv::builtin_material::openpbr,
                   .bindings = {of("base_color", white), of("fuzz_weight", 1.0f), of("fuzz_color", white)},
                   .how = bound::at_most,
                   .hybrid = enforce::report_only,
                   .known = "feat-sv-openpbr-tabulated-albedo"});
    // Smooth glass reflects or refracts every ray it meets, and a ray inside a sphere always finds its way out.
    out.push_back({.name = "glass",
                   .type = sv::builtin_material::openpbr,
                   .bindings = {of("transmission_weight", 1.0f), of("specular_roughness", 0.0f)},
                   .hybrid = enforce::report_only});
    return out;
}

/// The sphere's pixels, read off an image where it is darker than the environment.
struct sphere_mask
{
    /// a pixel whose whole 3x3 neighborhood is sphere, clear of the silhouette's antialiasing
    cc::vector<bool> interior;
    tg::vec2f center = tg::vec2f(0, 0);
    f32 radius = 0;
};

[[nodiscard]] sphere_mask mask_of(agreement::converged const& image, f64 environment)
{
    auto const size = agreement::k_size;
    auto const is_sphere
        = [&](int x, int y) { return f64(image.mean_at(isize(y) * size[0] + x)[0]) < 0.75 * environment; };

    auto out = sphere_mask{};
    out.interior = cc::vector<bool>::create_filled(isize(size[0]) * size[1], false);
    auto count = 0;
    auto sum = tg::vec2f(0, 0);
    for (auto y = 0; y < size[1]; ++y)
        for (auto x = 0; x < size[0]; ++x)
        {
            if (!is_sphere(x, y))
                continue;
            ++count;
            sum += tg::vec2f(f32(x) + 0.5f, f32(y) + 0.5f);
            if (x == 0 || y == 0 || x == size[0] - 1 || y == size[1] - 1)
                continue;
            auto whole = true;
            for (auto dy = -1; dy <= 1; ++dy)
                for (auto dx = -1; dx <= 1; ++dx)
                    whole = whole && is_sphere(x + dx, y + dy);
            out.interior[isize(y) * size[0] + x] = whole;
        }
    REQUIRE(count > size[0] * size[1] / 4).dump("sphere pixels", count);
    out.center = sum / f32(count);
    out.radius = tg::sqrt(f32(count) / tg::pi<f32>);
    return out;
}

/// One viewing angle band of the sphere: its mean per channel, and that mean's standard error from the two halves.
struct ring
{
    tg::vec3d mean = tg::vec3d(0, 0, 0);
    tg::vec3d error = tg::vec3d(0, 0, 0);
    int pixels = 0;
};

[[nodiscard]] int ring_of(sphere_mask const& mask, int x, int y)
{
    auto const d = (tg::vec2f(f32(x) + 0.5f, f32(y) + 0.5f) - mask.center).length() / mask.radius;
    return cc::min(k_rings - 1, int(d * f32(k_rings)));
}

[[nodiscard]] cc::vector<ring> rings_of(agreement::converged const& image, sphere_mask const& mask)
{
    auto const size = agreement::k_size;
    auto out = cc::vector<ring>::create_defaulted(k_rings);
    auto spread = cc::vector<tg::vec3d>::create_filled(k_rings, tg::vec3d(0, 0, 0));
    for (auto y = 0; y < size[1]; ++y)
        for (auto x = 0; x < size[0]; ++x)
        {
            auto const i = isize(y) * size[0] + x;
            if (!mask.interior[i])
                continue;
            auto& r = out[ring_of(mask, x, y)];
            auto& s = spread[ring_of(mask, x, y)];
            for (auto ch = 0; ch < 3; ++ch)
            {
                r.mean[ch] += f64(image.mean_at(i)[ch]);
                auto const d = f64(image.first[i][ch]) - f64(image.second[i][ch]);
                s[ch] += d * d;
            }
            ++r.pixels;
        }
    // The two halves' difference has twice a half's variance, and the mean of the halves half of it again.
    for (auto k = 0; k < k_rings; ++k)
        if (out[k].pixels > 0)
            for (auto ch = 0; ch < 3; ++ch)
            {
                auto const n = f64(out[k].pixels);
                out[k].mean[ch] /= n;
                out[k].error[ch] = tg::sqrt(spread[k][ch] / n / 4.0 / n);
            }
    return out;
}

/// The sphere as shown, beside its deviation from `expected`: red where brighter, blue where darker, full at 10%.
void write_furnace(cc::string_view name, agreement::converged const& image, sphere_mask const& mask, f64 expected)
{
    auto const size = agreement::k_size;
    auto mean = cc::vector<tg::vec4f>();
    for (auto i = isize(0); i < image.first.size(); ++i)
        mean.push_back(image.mean_at(i));
    auto deviation = cc::vector<tg::vec3f>();
    for (auto y = 0; y < size[1]; ++y)
        for (auto x = 0; x < size[0]; ++x)
        {
            auto const i = isize(y) * size[0] + x;
            if (!mask.interior[i])
            {
                deviation.push_back(tg::vec3f(0.15f, 0.15f, 0.15f));
                continue;
            }
            auto const c = mean[i];
            auto const lum = (f64(c[0]) + f64(c[1]) + f64(c[2])) / 3.0;
            auto const t = f32(cc::clamp((lum / expected - 1.0) * 10.0, -1.0, 1.0));
            deviation.push_back(t >= 0 ? tg::vec3f(0.5f + 0.5f * t, 0.5f - 0.5f * t, 0.5f - 0.5f * t)
                                       : tg::vec3f(0.5f + 0.5f * t, 0.5f + 0.5f * t, 0.5f - 0.5f * t));
        }
    // The environment shows as mid-gray, so a sphere that disappears is one the eye cannot find either.
    cc::vector<tg::vec3f> const panels[] = {agreement::display_of(mean, 0.2f), cc::move(deviation)};
    agreement::write_panels(name, panels);
}

/// Holds every ring of `image` to `c`'s albedo, and logs where each sits.
void check_furnace(cc::string_view name,
                   agreement::converged const& image,
                   sphere_mask const& mask,
                   furnace_case const& c,
                   enforce how)
{
    auto const rings = rings_of(image, mask);
    auto line = cc::string();
    auto failed = 0;
    for (auto k = 0; k < k_rings; ++k)
    {
        auto const& r = rings[k];
        if (r.pixels < 8)
            continue;
        auto worst = 0.0;
        for (auto ch = 0; ch < 3; ++ch)
        {
            auto const excess = r.mean[ch] - c.albedo;
            auto const relative = excess / c.albedo;
            auto const z = excess / cc::max(r.error[ch], 1e-9);
            worst = tg::abs(relative) > tg::abs(worst) ? relative : worst;
            auto const too_bright = relative > k_floor && z > 5.0;
            auto const too_dark = c.how == bound::exact && relative < -k_floor && z < -5.0;
            if (too_bright || too_dark)
                ++failed;
        }
        line += cc::format(" {:+.1f}%", worst * 100.0);
    }
    CC_LOG_INFO("furnace {}: {} rings center to rim, off its albedo {:.2f} by{}{}", name,
                c.how == bound::exact ? "exact" : "at most", c.albedo, line,
                c.known.empty() ? cc::string() : cc::format(" (known: {})", c.known));
    write_furnace(name, image, mask, c.albedo);
    if (how == enforce::tiles && c.known.empty())
        CHECK(failed == 0).dump("case", name).dump("rings", line);
}

/// Every case under --thorough; a default run draws each in the megakernel, and two in every mode.
/// A hybrid's forward stage compiles per material, and a cold one takes half a minute.
[[nodiscard]] bool draws_every_mode(furnace_case const& c)
{
    return c.every_mode || nx::is_thorough();
}
} // namespace

ASYNC_INVOCABLE_TEST("sv furnace - a sphere in a uniform environment reflects its albedo at every angle, in every mode",
                     (sg::context_handle const& ctx_h),
                     nx::config::thorough_only)
{
    auto& ctx = *ctx_h;
    if (!agreement::can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither the tracers nor the hybrid build");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto& lib = sv_test::shared_material_library();
    auto const environment = 1.0;

    // About 0.88 of the image's half-height, so the sphere is most of the image and its rim is in it.
    auto camera = sv::camera{.position = tg::pos3d(0, 0, -2.92)};
    camera.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);

    auto const furnace = [&](sv::render_mode mode, sv::hybrid_lighting lighting) -> agreement::mode_config
    {
        return {.name = "furnace",
                .mode = mode,
                .lighting = lighting,
                // Only glass bounces more than once off a convex object, and a ray through a sphere crosses it twice.
                .max_bounces = 16,
                .background = sv::background::uniform(tg::vec3f(f32(environment), f32(environment), f32(environment))),
                .fallback_light = false};
    };

    auto mask = cc::optional<sphere_mask>();
    for (auto const& c : all_cases())
    {
        auto const type = lib.acquire_type(c.type);
        REQUIRE(type.has_value()).dump("type", c.type);
        auto const material = lib.acquire(sv::material::create(cc::string(c.name), type.value(), c.bindings));
        auto const items = cc::vector<sv::scene_item>{resources.acquire_scene_item(
            sv::mesh{.name = cc::string(c.name), .geometry = sv_test::sphere_geometry(1.0f, 64, 96), .material = material})};
        resources.wait_for_pending_uploads();

        struct mode_case
        {
            cc::string_view name;
            sv::render_mode mode;
            sv::hybrid_lighting lighting;
            u32 samples;
            enforce how;
        };
        auto modes = cc::vector<mode_case>();
        modes.push_back({.name = "megakernel",
                         .mode = sv::render_mode::megakernel,
                         .lighting = sv::hybrid_lighting::deferred,
                         .samples = 512,
                         .how = enforce::tiles});
        if (draws_every_mode(c))
        {
            modes.push_back({.name = "wavefront",
                             .mode = sv::render_mode::wavefront,
                             .lighting = sv::hybrid_lighting::deferred,
                             .samples = 512,
                             .how = enforce::tiles});
            modes.push_back({.name = "forward",
                             .mode = sv::render_mode::hybrid,
                             .lighting = sv::hybrid_lighting::forward,
                             .samples = 256,
                             .how = c.hybrid});
            modes.push_back({.name = "deferred",
                             .mode = sv::render_mode::hybrid,
                             .lighting = sv::hybrid_lighting::deferred,
                             .samples = 256,
                             .how = c.hybrid});
        }

        for (auto const& m : modes)
        {
            auto config = furnace(m.mode, m.lighting);
            auto const name = cc::format("furnace-{}-{}", c.name, m.name);
            config.name = name;
            auto const image = co_await agreement::converge(ctx, resources, items, camera, config, m.samples, 0,
                                                            agreement::k_size, true);
            if (!mask.has_value())
                mask = mask_of(image, environment);
            check_furnace(name, image, mask.value(), c, m.how);
        }
    }
    co_await cc::async_settled(sv::background_work(ctx));
}
