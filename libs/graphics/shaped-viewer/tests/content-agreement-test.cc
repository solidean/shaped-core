#include "agreement_test_support.hh"

#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>

// The lit Cornell box holding every kind of content a material and a mesh can carry, drawn by each mode and held to the megakernel.
//
// A checker texture through uvs past [0, 1], a tangent-space normal map with per-vertex color and an anisotropic roughness,
// a stochastic cutout, a quadric batch of spheres, a cone and a line, and an emissive panel the tracers must find as a light.
// One scene for all of them: each is a region of the image, so a tile that disagrees says which content a mode draws wrong.
// The wavefront is held to the megakernel; each hybrid lighting to the megakernel's path one bounce longer, as the material test holds them.

namespace
{
using namespace cc::primitive_defines;
namespace agreement = sv_test::agreement;

[[nodiscard]] sv::material_id material_of(cc::string name,
                                          cc::string_view type,
                                          cc::vector<sv::material_attribute_binding> overrides)
{
    auto& lib = sv_test::shared_material_library();
    return lib.acquire(sv::material::create(cc::move(name), lib.acquire_type(type).value(), cc::move(overrides)));
}

/// The quad `a b c d` as two triangles of six unshared vertices.
[[nodiscard]] cc::vector<tg::pos3f> quad(tg::pos3f a, tg::pos3f b, tg::pos3f c, tg::pos3f d)
{
    auto out = cc::vector<tg::pos3f>();
    for (auto const& p : {a, b, c, a, c, d})
        out.push_back(p);
    return out;
}

/// A per-vertex attribute of such a quad: one value per corner, spread over its six vertices.
template <class T>
[[nodiscard]] sv::mesh_attribute corner_attribute(cc::string name, T const& a, T const& b, T const& c, T const& d)
{
    auto values = cc::vector<T>();
    for (auto const& v : {a, b, c, a, c, d})
        values.push_back(v);
    return sv::mesh_attribute::create(cc::move(name), sv::attribute_frequency::per_vertex, cc::move(values));
}

/// A rotation about the tangent-space normal by `degrees`, as the xyzw quaternion a tangent frame attribute holds.
[[nodiscard]] tg::vec4f rotation_about_z(f32 degrees)
{
    auto const half = tg::angle_f::make_from_degree(0.5f * degrees);
    return tg::vec4f(0, 0, tg::sin(half), tg::cos(half));
}

/// A 4x4 checker of distinct colors, so a nearest sample tells its texels apart and an address mode shows at the edges.
[[nodiscard]] sv::texture_data checker_texture()
{
    auto pixels = cc::array<byte>::create_defaulted(4 * 4 * 4);
    for (auto y = 0; y < 4; ++y)
        for (auto x = 0; x < 4; ++x)
        {
            auto const i = (y * 4 + x) * 4;
            pixels[i + 0] = byte(40 + 60 * x);
            pixels[i + 1] = byte(220 - 50 * y);
            pixels[i + 2] = byte((x + y) % 2 == 0 ? 200 : 30);
            pixels[i + 3] = byte(255);
        }
    return sv::texture_data::create(cc::move(pixels), sg::pixel_format::rgba8_unorm, 4, 4);
}

/// The content, as scene items added to the lit box.
[[nodiscard]] cc::vector<sv::scene_item> content_items(sv::gpu_resource_manager& resources)
{
    auto items = cc::vector<sv::scene_item>();

    // On the back wall above the short block: a texture past [0, 1], clamped across and mirrored up.
    {
        auto mesh = sv::mesh{.name = "content texture",
                             .geometry = sv::triangle_geometry::create_from_positions(
                                 quad(tg::pos3f(-0.1f, -0.2f, 0.99f), tg::pos3f(-0.8f, -0.2f, 0.99f),
                                      tg::pos3f(-0.8f, 0.6f, 0.99f), tg::pos3f(-0.1f, 0.6f, 0.99f)))};
        mesh.attributes.push_back(corner_attribute<tg::vec2f>("uv", tg::vec2f(-0.5f, -0.5f), tg::vec2f(1.5f, -0.5f),
                                                              tg::vec2f(1.5f, 1.5f), tg::vec2f(-0.5f, 1.5f)));
        mesh.textures.push_back({.name = "base_color",
                                 .source = {.texture = checker_texture(),
                                            .sampler = {.min_filter = sg::sampler_filter::nearest,
                                                        .mag_filter = sg::sampler_filter::nearest,
                                                        .address_u = sg::sampler_address_mode::clamp_edge,
                                                        .address_v = sg::sampler_address_mode::mirror_repeat}}});
        mesh.material = material_of("content textured", sv::builtin_material::openpbr, {});
        items.push_back(resources.acquire_scene_item(mesh));
    }

    // On the back wall left of the tall block: a normal map turned by a tangent frame, a color and a roughness per corner, an anisotropic lobe.
    // Everything sits inside the narrow view the lit box is seen through, whose side walls and front floor lie outside it.
    {
        auto mesh = sv::mesh{.name = "content attributes",
                             .geometry = sv::triangle_geometry::create_from_positions(
                                 quad(tg::pos3f(0.95f, -0.6f, 0.99f), tg::pos3f(0.65f, -0.6f, 0.99f),
                                      tg::pos3f(0.65f, 0.6f, 0.99f), tg::pos3f(0.95f, 0.6f, 0.99f)))};
        mesh.attributes.push_back(corner_attribute<tg::vec3f>("base_color", tg::vec3f(0.9f, 0.1f, 0.1f),
                                                              tg::vec3f(0.1f, 0.9f, 0.1f), tg::vec3f(0.1f, 0.1f, 0.9f),
                                                              tg::vec3f(0.9f, 0.9f, 0.1f)));
        mesh.attributes.push_back(corner_attribute<tg::vec3f>("normal", tg::vec3f(0.3f, 0.0f, 1.0f),
                                                              tg::vec3f(-0.2f, 0.2f, 1.0f), tg::vec3f(0.0f, -0.3f, 1.0f),
                                                              tg::vec3f(0.1f, 0.1f, 1.0f)));
        mesh.attributes.push_back(corner_attribute<tg::vec4f>("tangent_frame", rotation_about_z(0.0f),
                                                              rotation_about_z(20.0f), rotation_about_z(45.0f),
                                                              rotation_about_z(-30.0f)));
        mesh.attributes.push_back(corner_attribute<f32>("specular_roughness", 0.2f, 0.4f, 0.6f, 0.3f));
        mesh.material = material_of("content attributes", sv::builtin_material::openpbr,
                                    {sv::material_attribute_binding::of("specular_roughness_anisotropy", 0.5f)});
        items.push_back(resources.acquire_scene_item(mesh));
    }

    // Standing in front of the tall block: a coverage per corner under an alpha cutoff of one half, which cuts the panel sharply where it falls below.
    // Thresholded rather than kept as a fraction: the hybrid's rays skip an instance whose coverage is a fraction and that lights nothing,
    // taking it for a decal on a surface, so a free-standing one casts no shadow there, issue: bug-sv-hybrid-skips-free-standing-coverage.
    {
        auto mesh = sv::mesh{.name = "content cutout",
                             .geometry = sv::triangle_geometry::create_from_positions(
                                 quad(tg::pos3f(0.85f, -1.0f, -0.45f), tg::pos3f(0.2f, -1.0f, -0.45f),
                                      tg::pos3f(0.2f, -0.2f, -0.45f), tg::pos3f(0.85f, -0.2f, -0.45f)))};
        mesh.attributes.push_back(corner_attribute<f32>("opacity", 0.2f, 0.9f, 0.5f, 1.0f));
        mesh.material = material_of("content cutout", sv::builtin_material::openpbr,
                                    {sv::material_attribute_binding::of("base_color", tg::vec3f(0.9f, 0.7f, 0.2f)),
                                     sv::material_attribute_binding::of("alpha_cutoff", 0.5f)});
        items.push_back(resources.acquire_scene_item(mesh));
    }

    // On top of the blocks: two spheres and a capped cone on the tall one, a flat-ended line across the short one.
    {
        auto set = sv::quadric_set();
        set.add_sphere(tg::sphere3f(tg::pos3f(0.25f, 0.4f, 0.3f), 0.15f));
        set.add_sphere(tg::sphere3f(tg::pos3f(0.5f, 0.33f, 0.45f), 0.08f));
        set.add_cone(tg::segment3f(tg::pos3f(0.45f, 0.25f, 0.15f), tg::pos3f(0.45f, 0.55f, 0.15f)), 0.08f, true);
        set.add_line(tg::segment3f(tg::pos3f(-0.5f, -0.3f, 0.0f), tg::pos3f(-0.1f, -0.3f, 0.2f)),
                     {.radius = 0.04f, .ends = sv::line_ends::flat});
        auto colors = cc::vector<tg::vec3f>();
        for (auto i = 0; i < set.primitive_count(); ++i)
            colors.push_back(tg::vec3f(0.2f + 0.15f * f32(i % 5), 0.8f - 0.1f * f32(i % 7), 0.4f));
        set.attributes.push_back(
            sv::mesh_attribute::create("base_color", sv::attribute_frequency::per_triangle, cc::move(colors)));
        set.material = sv::default_material(sv_test::shared_material_library());
        items.push_back(resources.acquire_scene_item(set));
    }

    // High on the back wall above the texture: a blue strip that emits, a second light the tracers sample and the hybrid's bounces reach.
    {
        auto mesh = sv::mesh{.name = "content emitter",
                             .geometry = sv::triangle_geometry::create_from_positions(
                                 quad(tg::pos3f(-0.1f, 0.7f, 0.99f), tg::pos3f(-0.8f, 0.7f, 0.99f),
                                      tg::pos3f(-0.8f, 0.85f, 0.99f), tg::pos3f(-0.1f, 0.85f, 0.99f)))};
        mesh.material = material_of("content emitter", sv::builtin_material::openpbr,
                                    {sv::material_attribute_binding::of("base_color", tg::vec3f(0.0f, 0.0f, 0.0f)),
                                     sv::material_attribute_binding::of("emission_color", tg::vec3f(0.3f, 0.5f, 1.0f)),
                                     sv::material_attribute_binding::of("emission_luminance", 8.0f)});
        items.push_back(resources.acquire_scene_item(mesh));
    }

    resources.wait_for_pending_uploads();
    return items;
}
/// The pixels the cutout panel covers, inset a pixel: it reaches past the image's left and bottom edges.
constexpr auto k_cutout_lo = tg::vec2i(1, 42);
constexpr auto k_cutout_hi = tg::vec2i(22, 63);

/// Holds the mean of `contender` over the cutout's pixels to `reference`'s, with the halves' difference as each one's noise.
/// The tile comparison masks much of the cutout as an edge, where the panel's cut and the surfaces behind it meet.
void check_cutout(cc::string_view name, agreement::converged const& contender, agreement::converged const& reference)
{
    auto const size = agreement::k_size;
    auto const region = [&](agreement::converged const& c, f64& mean, f64& noise)
    {
        auto sum = 0.0;
        auto spread = 0.0;
        auto n = 0;
        for (auto y = k_cutout_lo[1]; y < k_cutout_hi[1]; ++y)
            for (auto x = k_cutout_lo[0]; x < k_cutout_hi[0]; ++x)
            {
                auto const i = isize(y) * size[0] + x;
                sum += agreement::luminance_of(c.mean_at(i));
                auto const d = agreement::luminance_of(c.first[i]) - agreement::luminance_of(c.second[i]);
                spread += d * d;
                ++n;
            }
        mean = sum / f64(n);
        // The mean of the halves has a quarter of their difference's variance, and the region's mean an n-th of that.
        noise = tg::sqrt(spread / 4.0) / f64(n);
    };
    auto contender_mean = 0.0;
    auto contender_noise = 0.0;
    auto reference_mean = 0.0;
    auto reference_noise = 0.0;
    region(contender, contender_mean, contender_noise);
    region(reference, reference_mean, reference_noise);
    auto const difference = contender_mean - reference_mean;
    auto const z = tg::abs(difference) / tg::sqrt(contender_noise * contender_noise + reference_noise * reference_noise);
    auto const relative = difference / reference_mean;
    CC_LOG_INFO("{}: the cutout's region {:+.1f}% off the reference, {:.2f} standard errors", name, 100.0 * relative, z);
    auto const agrees = z < 5.0 || tg::abs(relative) < 0.02;
    CHECK(agrees).dump("test", name).dump("relative", relative).dump("z", z);
}
} // namespace

ASYNC_INVOCABLE_TEST("sv agreement - every kind of content draws the same in every mode",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!agreement::can_trace(ctx))
        SKIP("no inline ray tracing or no SGL compiler, so neither the tracers nor the hybrid build");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const box = agreement::make_lit_box(resources);
    auto items = box.items;
    for (auto const& item : content_items(resources))
        items.push_back(item);

    auto const full = co_await agreement::converge(ctx, resources, items, box.camera,
                                                   {.name = "content", .mode = sv::render_mode::megakernel}, 1024,
                                                   agreement::k_reference_restarts);
    auto const one_bounce = co_await agreement::converge(
        ctx, resources, items, box.camera, {.name = "content", .mode = sv::render_mode::megakernel, .max_bounces = 2},
        1024, agreement::k_reference_restarts);
    auto const wavefront = co_await agreement::converge(ctx, resources, items, box.camera,
                                                        {.name = "content", .mode = sv::render_mode::wavefront}, 1024);
    auto const forward = co_await agreement::converge(
        ctx, resources, items, box.camera,
        {.name = "content", .mode = sv::render_mode::hybrid, .lighting = sv::hybrid_lighting::forward}, 512);
    auto const deferred = co_await agreement::converge(
        ctx, resources, items, box.camera,
        {.name = "content", .mode = sv::render_mode::hybrid, .lighting = sv::hybrid_lighting::deferred}, 512);

    agreement::check_agreement("content-wavefront", wavefront, full, 0.01);
    agreement::check_agreement("content-forward", forward, one_bounce, 0.02);
    agreement::check_agreement("content-deferred", deferred, one_bounce, 0.02);
    check_cutout("content-wavefront", wavefront, full);
    check_cutout("content-forward", forward, one_bounce);
    check_cutout("content-deferred", deferred, one_bounce);
    co_await cc::async_settled(sv::background_work(ctx));
}
