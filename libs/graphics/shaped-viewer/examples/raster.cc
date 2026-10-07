#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/linalg/cross.hh> // tg::cross + tg::dual
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/linalg/vec_ops.hh>   // tg::normalize
#include <typed-geometry/scalar/constants.hh> // tg::pi

using namespace cc::primitive_defines;

// One scene twice: path-traced on the left, rasterized on the right.
//
// Both panels draw the same meshes with the same OpenPBR materials — the raster one runs the very material code the tracer's hit groups do, as a pixel stage.
// What differs is the light transport: the raster panel lights each surface directly from every light and from the sky's SH, with no shadow and no bounce.
// So the spheres read alike, and the floor under them is where the two part: the tracer's has their shadows and their colored bounce, the raster one has neither.
// The cut-out fence at the back is a texture-free `opacity` below its `alpha_cutoff`, which both panels cut.
// ../docs/raster.md is the design.
//
// Controls, per panel
//   left-drag    orbit          middle-drag    pan          wheel    zoom
//
// Run it:
//   uv run dev.py example shaped-viewer/raster
//   uv run dev.py example shaped-viewer/raster --capture    # headless, writes an image

namespace
{
constexpr float sphere_radius = 0.5f;

/// An indexed UV sphere of `radius` with a tangent frame per vertex, wound so every face's normal points outward.
///
/// TEMPORARY: the third caller hand-rolling a sphere, after `openpbr-spheres`; a `tg::` sphere tessellation is the library
/// extension that replaces all of them — see the viewer TODO.
struct sphere_mesh
{
    cc::vector<tg::pos3f> positions;
    cc::vector<u32> indices;
    cc::vector<tg::quat_f> frames;
};

[[nodiscard]] sphere_mesh make_sphere(float radius, int rings, int segments)
{
    auto out = sphere_mesh();
    for (auto r = 0; r <= rings; ++r)
    {
        float const theta = float(r) / float(rings) * tg::pi<float>;
        for (auto s = 0; s <= segments; ++s)
        {
            float const phi = float(s) / float(segments) * 2.0f * tg::pi<float>;
            auto const n = tg::vec3f(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
            out.positions.push_back(tg::pos3f(radius * n[0], radius * n[1], radius * n[2]));
            auto const tangent = tg::normalize(tg::vec3f(-std::sin(phi), 0.0f, std::cos(phi)));
            auto const bitangent = tg::dual(tg::cross(n, tangent));
            out.frames.push_back(tg::quat_f::make_from_basis(tangent, bitangent, n).normalized());
        }
    }

    auto const index_of = [segments](int r, int s) { return u32(r * (segments + 1) + s); };
    for (auto r = 0; r < rings; ++r)
    {
        for (auto s = 0; s < segments; ++s)
        {
            for (auto const i : {index_of(r, s), index_of(r + 1, s), index_of(r + 1, s + 1)})
                out.indices.push_back(i);
            for (auto const i : {index_of(r, s), index_of(r + 1, s + 1), index_of(r, s + 1)})
                out.indices.push_back(i);
        }
    }
    return out;
}

/// A quad from `a` through `b`, `c` and `d`, as two triangles.
[[nodiscard]] cc::vector<tg::pos3f> quad(tg::pos3f a, tg::pos3f b, tg::pos3f c, tg::pos3f d)
{
    return {a, b, c, a, c, d};
}

/// Everything both panels draw: a floor, five spheres of different materials, a box, and a fence cut out of a quad.
[[nodiscard]] cc::vector<sv::mesh> make_scene(sv::material_library& lib)
{
    using binding = sv::material_attribute_binding;
    auto const openpbr = lib.acquire_type(sv::builtin_material::openpbr).value();
    auto const material = [&](cc::string_view name, cc::vector<binding> bindings)
    { return lib.acquire(sv::material::create(cc::string(name), openpbr, cc::move(bindings))); };

    auto out = cc::vector<sv::mesh>();
    out.push_back({.name = "floor",
                   .geometry = sv::triangle_geometry::create_from_positions(
                       quad(tg::pos3f(-6, 0, -6), tg::pos3f(-6, 0, 6), tg::pos3f(6, 0, 6), tg::pos3f(6, 0, -6))),
                   .material = material("floor", {binding::of("base_color", tg::vec3f(0.7f, 0.7f, 0.68f)),
                                                  binding::of("specular_roughness", 0.6f)})});

    auto const sphere = make_sphere(sphere_radius, 32, 48);
    auto const geometry = sv::triangle_geometry::create_from_indexed_triangles(sphere.positions, sphere.indices);
    auto const frames = sv::mesh_attribute::create("tangent_frame", sv::attribute_frequency::per_vertex, sphere.frames);

    struct sphere_kind
    {
        cc::string_view name;
        cc::vector<binding> bindings;
    };
    auto kinds = cc::vector<sphere_kind>();
    kinds.push_back({.name = "plastic",
                     .bindings = {binding::of("base_color", tg::vec3f(0.8f, 0.15f, 0.1f)),
                                  binding::of("specular_roughness", 0.3f)}});
    kinds.push_back({.name = "gold",
                     .bindings = {binding::of("base_color", tg::vec3f(1.0f, 0.78f, 0.34f)),
                                  binding::of("base_metalness", 1.0f), binding::of("specular_roughness", 0.25f)}});
    kinds.push_back(
        {.name = "coated",
         .bindings = {binding::of("base_color", tg::vec3f(0.1f, 0.25f, 0.7f)), binding::of("specular_roughness", 0.6f),
                      binding::of("coat_weight", 1.0f), binding::of("coat_roughness", 0.05f)}});
    kinds.push_back({.name = "chrome",
                     .bindings = {binding::of("base_color", tg::vec3f(0.95f, 0.95f, 0.95f)),
                                  binding::of("base_metalness", 1.0f), binding::of("specular_roughness", 0.05f)}});
    kinds.push_back({.name = "rough",
                     .bindings = {binding::of("base_color", tg::vec3f(0.2f, 0.6f, 0.25f)),
                                  binding::of("specular_roughness", 1.0f)}});

    for (auto i = isize(0); i < kinds.size(); ++i)
    {
        auto const x = (float(i) - 2.0f) * 1.3f;
        out.push_back({.name = cc::string(kinds[i].name),
                       .geometry = geometry,
                       .attributes = {frames},
                       .transform = tg::affine_transform3f::make_translation(tg::vec3f(x, sphere_radius, 0)),
                       .material = material(kinds[i].name, cc::move(kinds[i].bindings))});
    }

    // A crate beside the row.
    auto crate = cc::vector<tg::pos3f>();
    auto const lo = tg::pos3f(1.8f, 0, 1.2f);
    auto const hi = tg::pos3f(2.8f, 1.0f, 2.2f);
    auto const corner
        = [&](int i) { return tg::pos3f((i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2]); };
    int const faces[6][4] = {{1, 3, 7, 5}, {0, 4, 6, 2}, {2, 6, 7, 3}, {0, 1, 5, 4}, {4, 5, 7, 6}, {0, 2, 3, 1}};
    for (auto const& f : faces)
        for (auto const& p : quad(corner(f[0]), corner(f[1]), corner(f[2]), corner(f[3])))
            crate.push_back(p);
    out.push_back({.name = "crate",
                   .geometry = sv::triangle_geometry::create_from_positions(crate),
                   .material = material("crate", {binding::of("base_color", tg::vec3f(0.75f, 0.55f, 0.3f)),
                                                  binding::of("specular_roughness", 0.7f)})});

    // A fence: alternate slats kept and cut by a per-triangle opacity thresholded at its cutoff, glTF's MASK.
    auto fence = cc::vector<tg::pos3f>();
    auto opacity = cc::vector<f32>();
    for (auto s = 0; s < 12; ++s)
    {
        auto const x0 = -3.0f + 0.5f * float(s);
        for (auto const& p : quad(tg::pos3f(x0, 0, 2.5f), tg::pos3f(x0, 1.6f, 2.5f), tg::pos3f(x0 + 0.5f, 1.6f, 2.5f),
                                  tg::pos3f(x0 + 0.5f, 0, 2.5f)))
            fence.push_back(p);
        opacity.push_back(s % 2 == 0 ? 1.0f : 0.1f);
        opacity.push_back(s % 2 == 0 ? 1.0f : 0.1f);
    }
    out.push_back({.name = "fence",
                   .geometry = sv::triangle_geometry::create_from_positions(fence),
                   .attributes = {sv::mesh_attribute::create("opacity", sv::attribute_frequency::per_triangle, opacity)},
                   .material = material("fence", {binding::of("base_color", tg::vec3f(0.9f, 0.9f, 0.85f)),
                                                  binding::of("alpha_cutoff", 0.5f)})});
    return out;
}
} // namespace

EXAMPLE("shaped-viewer/raster")
{
    auto& lib = *sv::acquire_material_library().value();
    auto const scene_meshes = make_scene(lib);
    auto const sky = sv::background::studio().scaled(0.5f);
    auto const sun_direction = tg::vec3f(0.5f, -0.75f, 0.45f);

    // A painted marking on the floor in front of the row, which both panels paint through the material it lands on.
    auto marking = sv::drawing();
    marking.add_fill(sv::path::rectangle(tg::aabb2f(tg::pos2f(-3.0f, -0.12f), tg::pos2f(3.0f, 0.12f))),
                     {.color = tg::vec4f(0.95f, 0.75f, 0.1f, 1)});

    for (auto f : sv::interactive("shaped-viewer/raster"))
    {
        auto columns = f.window().view().layout_columns({.padding = 6, .spacing = 6});

        auto const panel = [&](cc::string_view id, sv::path_integrator integrator)
        {
            auto view = columns.add_view(id);
            view.initial_orbit({.target = tg::pos3d(0, 0.6, 0.6),
                                .distance = 7.5,
                                .azimuth = tg::angle_d::make_from_degree(-25.0),
                                .elevation = tg::angle_d::make_from_degree(24.0)});
            auto scene = view.add_scene();
            for (auto const& m : scene_meshes)
                scene.add_mesh(m);
            scene.background(sky);
            scene.add_rect_light("key", tg::pos3f(-1.5f, 3.5f, -1.0f), tg::vec3f(0.8f, 0, 0), tg::vec3f(0, 0, 0.8f)).nits(6);
            scene.add_directional_light("sun", sun_direction).lux(1.5f);
            scene.settings({.integrator = integrator});

            // Seen from above, the floor's normal is +y, so the marking projects down: y_axis x x_axis is -y.
            scene.add_decal(marking, {.at = tg::pos3f(0, 0, -1.0f),
                                      .x_axis = tg::vec3f(1, 0, 0),
                                      .y_axis = tg::vec3f(0, 0, -1),
                                      .scale = 1,
                                      .depth = 0.3f});

            // A label lying on the crate's lid: a drawing over the image, occluded by the image's own primary depth.
            scene.add_text(
                id, {.at = tg::pos3f(1.92f, 1.005f, 1.95f), .x_axis = tg::vec3f(1, 0, 0), .y_axis = tg::vec3f(0, 0, -1)},
                {.size = 0.24f, .color = tg::vec4f(1, 1, 1, 1)});
        };

        panel("traced", sv::path_integrator::wavefront);
        panel("raster", sv::path_integrator::raster);
    }
}
