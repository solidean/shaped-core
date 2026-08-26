#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/linalg/cross.hh> // tg::cross + tg::dual
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/linalg/vec_ops.hh>   // tg::normalize
#include <typed-geometry/scalar/constants.hh> // tg::pi

using namespace cc::primitive_defines;

// The OpenPBR parameter sweeps, as a grid of spheres.
//
// Each ROW isolates one axis of the model and each COLUMN sweeps roughness across it, so what a parameter actually does is
// visible next to what it does not.
// Everything is the builtin `openpbr` material type — `sv::builtin_material::openpbr` — with a handful of attributes bound; an
// attribute no row mentions keeps the specification's own default.
//
// The rows, bottom to top:
//   dielectric   the default surface, a rough-to-smooth specular sweep over a grey base
//   metal        base_metalness 1, so base_color is the reflectance and specular_color its tint at grazing
//   coated       a clear coat over a red base — coat_weight 1, and its own roughness fixed while the base sweeps
//   fuzz         a sheen layer over a dark base, which is what makes velvet read as velvet
//   diffuse      base_diffuse_roughness, the Oren-Nayar axis: flat at 0, dusty toward 1
//
// Run it:
//   uv run dev.py example shaped-viewer/openpbr-spheres

namespace
{
constexpr int column_count = 7; // roughness samples per row
constexpr float sphere_radius = 0.45f;
constexpr float spacing = 1.15f;

/// An indexed UV sphere of `radius`, centered on the origin, wound so every face's geometric normal points outward.
///
/// TEMPORARY: primitive meshing belongs in typed-geometry rather than in an example, and this is the second caller that
/// would want it (`hello-cube` hand-rolls a cube). A `tg::` sphere/box tessellation is the library extension that replaces
/// this — see the viewer TODO.
struct sphere_mesh
{
    cc::vector<tg::pos3f> positions;
    cc::vector<u32> indices;

    /// one tangent frame per position, taking tangent space (+x tangent, +y bitangent, +z normal) to object space
    cc::vector<tg::quat_f> frames;
};

[[nodiscard]] sphere_mesh make_sphere(float radius, int rings, int segments)
{
    auto out = sphere_mesh();
    out.positions.reserve((rings + 1) * (segments + 1));
    out.frames.reserve((rings + 1) * (segments + 1));

    // The poles are duplicated per segment rather than shared, which keeps the ring/segment indexing uniform.
    for (auto r = 0; r <= rings; ++r)
    {
        float const v = float(r) / float(rings);
        float const theta = v * tg::pi<float>;
        float const sin_theta = std::sin(theta);
        float const cos_theta = std::cos(theta);

        for (auto s = 0; s <= segments; ++s)
        {
            float const u = float(s) / float(segments);
            float const phi = u * 2.0f * tg::pi<float>;
            auto const n = tg::vec3f(sin_theta * std::cos(phi), cos_theta, sin_theta * std::sin(phi));
            out.positions.push_back(tg::pos3f(radius * n[0], radius * n[1], radius * n[2]));

            // The frame is exact rather than fitted: the tangent is the derivative along the u parameter, so it follows the
            // uv layout the way an authored one would, and the bitangent closes a right-handed basis.
            // At a pole the derivative vanishes and any tangent perpendicular to n will do — this one is what the limit
            // approaches from the last ring.
            auto const tangent = tg::normalize(tg::vec3f(-std::sin(phi), 0.0f, std::cos(phi)));
            // cross() wedges into a bivector, so dual() is what brings the third axis back as a vector.
            auto const bitangent = tg::dual(tg::cross(n, tangent));
            out.frames.push_back(tg::quat_f::make_from_basis(tangent, bitangent, n).normalized());
        }
    }

    auto const index_of = [segments](int r, int s) { return u32(r * (segments + 1) + s); };
    for (auto r = 0; r < rings; ++r)
    {
        for (auto s = 0; s < segments; ++s)
        {
            auto const a = index_of(r, s);
            auto const b = index_of(r + 1, s);
            auto const c = index_of(r + 1, s + 1);
            auto const d = index_of(r, s + 1);

            out.indices.push_back(a);
            out.indices.push_back(b);
            out.indices.push_back(c);

            out.indices.push_back(a);
            out.indices.push_back(c);
            out.indices.push_back(d);
        }
    }
    return out;
}

/// A large ground quad at `y`, wound so its normal points up.
[[nodiscard]] cc::vector<tg::pos3f> ground_quad(float y, float half_size)
{
    auto const h = half_size;
    auto out = cc::vector<tg::pos3f>();
    out.reserve(6);

    out.push_back(tg::pos3f(-h, y, -h));
    out.push_back(tg::pos3f(-h, y, h));
    out.push_back(tg::pos3f(h, y, h));

    out.push_back(tg::pos3f(-h, y, -h));
    out.push_back(tg::pos3f(h, y, h));
    out.push_back(tg::pos3f(h, y, -h));
    return out;
}

/// One row of the grid: what it is called, and the bindings every sphere in it carries beyond its own roughness.
struct sweep_row
{
    cc::string_view name;
    cc::vector<sv::material_attribute_binding> shared;

    /// which attribute this row's columns sweep
    cc::string_view swept = "specular_roughness";

    /// What the swept attribute runs from and to across the row.
    ///
    /// The default never reaches 0: a mirror-smooth lobe is a delta the estimator cannot sample, and the BSDF floors it
    /// anyway — starting just above is what makes the first column show that floor rather than hide it.
    /// A row sweeping something that is not a roughness says so, because nanometres and radians have their own ranges.
    float from = 0.03f;
    float to = 1.0f;

    /// When set, the swept attribute is a tangent-space DIRECTION rather than a scalar, and the column's value is how far it
    /// leans off the surface normal.
    /// `coat_normal` is the one that wants this: a coat tilted away from the base is not something a number expresses.
    bool swept_is_direction = false;
};

[[nodiscard]] cc::vector<sweep_row> make_rows()
{
    using binding = sv::material_attribute_binding;

    auto rows = cc::vector<sweep_row>();

    rows.push_back({.name = "dielectric", .shared = {binding::of("base_color", tg::vec3f(0.62f, 0.64f, 0.68f))}});

    rows.push_back(
        {.name = "metal",
         .shared = {binding::of("base_color", tg::vec3f(0.94f, 0.78f, 0.38f)), binding::of("base_metalness", 1.0f),
                    // The F82 tint: what the metal does toward grazing, where a Schlick lobe would wash out.
                    binding::of("specular_color", tg::vec3f(0.97f, 0.90f, 0.72f))}});

    rows.push_back({.name = "coated",
                    .shared = {binding::of("base_color", tg::vec3f(0.55f, 0.09f, 0.07f)),
                               binding::of("coat_weight", 1.0f), binding::of("coat_roughness", 0.06f)}});

    rows.push_back(
        {.name = "fuzz",
         .shared = {binding::of("base_color", tg::vec3f(0.06f, 0.07f, 0.12f)), binding::of("specular_roughness", 0.6f),
                    binding::of("fuzz_weight", 1.0f), binding::of("fuzz_color", tg::vec3f(0.85f, 0.72f, 0.55f))},
         .swept = "fuzz_roughness"});

    // The anisotropy sweep needs the roughness held still, because it is the SHAPE of the highlight that varies here
    // rather than its size — and the tangent frame the spheres carry is what gives the stretch a direction.
    rows.push_back({.name = "anisotropic",
                    .shared = {binding::of("base_color", tg::vec3f(0.90f, 0.90f, 0.93f)),
                               binding::of("base_metalness", 1.0f), binding::of("specular_roughness", 0.35f)},
                    .swept = "specular_roughness_anisotropy",
                    .from = 0.0f,
                    .to = 0.95f});

    // Thickness in nanometres, across the range where the first interference order sweeps the whole hue circle.
    // A smooth metal underneath, because the film's color is a property of the specular reflection and a diffuse base
    // would wash it out.
    rows.push_back({.name = "thin film",
                    .shared = {binding::of("base_color", tg::vec3f(0.95f, 0.95f, 0.95f)),
                               binding::of("base_metalness", 1.0f), binding::of("specular_roughness", 0.12f),
                               binding::of("thin_film_weight", 1.0f), binding::of("thin_film_ior", 1.5f)},
                    .swept = "thin_film_thickness",
                    .from = 120.0f,
                    .to = 900.0f});

    // The coat's normal leaning further off the base's along the row, which moves the coat's highlight while the base's
    // stays put — the one thing a shared normal cannot show.
    rows.push_back(
        {.name = "coat normal",
         .shared = {binding::of("base_color", tg::vec3f(0.12f, 0.30f, 0.55f)), binding::of("specular_roughness", 0.5f),
                    binding::of("coat_weight", 1.0f), binding::of("coat_roughness", 0.08f)},
         .swept = "coat_normal",
         .from = 0.0f,
         .to = 1.4f,
         .swept_is_direction = true});

    // The transparent base, swept by the roughness of the interface it refracts through — clear glass at one end and
    // something closer to frosted at the other.
    rows.push_back({.name = "glass",
                    .shared = {binding::of("transmission_weight", 1.0f), binding::of("specular_ior", 1.5f)},
                    .swept = "specular_roughness"});

    // The same, minus the interior: a thin wall does not refract and encloses no medium, so its tint is paid at the
    // crossing and the sphere reads as a bubble rather than as a solid ball of glass.
    rows.push_back({.name = "thin walled",
                    .shared = {binding::of("transmission_weight", 1.0f), binding::of("thin_walled", 1.0f),
                               binding::of("transmission_color", tg::vec3f(0.85f, 0.62f, 0.45f))},
                    .swept = "specular_roughness"});

    // Absorption inside the solid, swept by the distance the color is reached at.
    // The near columns absorb within a fraction of a sphere and read nearly opaque; the far ones barely tint.
    // This is the one parameter whose effect is a property of the VOLUME rather than of the surface, so it needs the
    // sphere's own thickness to show at all.
    rows.push_back({.name = "absorbing",
                    .shared = {binding::of("transmission_weight", 1.0f),
                               binding::of("transmission_color", tg::vec3f(0.25f, 0.55f, 0.85f)),
                               binding::of("specular_roughness", 0.06f)},
                    .swept = "transmission_depth",
                    .from = 0.06f,
                    .to = 2.5f});

    // Dispersion, swept by how far apart the wavelengths are bent.
    // Only a channel-collapsed path shows it, so the fringes resolve as the accumulation converges rather than appearing
    // on the first frame — and they need a SMOOTH interface, since roughness blurs the three apart into grey again.
    rows.push_back(
        {.name = "dispersion",
         .shared = {binding::of("transmission_weight", 1.0f), binding::of("specular_roughness", 0.03f),
                    binding::of("specular_ior", 1.6f), binding::of("transmission_dispersion_abbe_number", 18.0f)},
         .swept = "transmission_dispersion_scale",
         .from = 0.0f,
         .to = 3.0f});

    // The subsurface base, swept by how far light travels inside before it comes back out.
    // The near columns are nearly opaque and the far ones are waxy, which is the whole range the parameter covers — and
    // the radius is per channel, red reaching furthest, which is what makes it read as flesh rather than as paint.
    rows.push_back(
        {.name = "subsurface",
         .shared
         = {binding::of("subsurface_weight", 1.0f), binding::of("subsurface_color", tg::vec3f(0.88f, 0.52f, 0.42f)),
            binding::of("subsurface_radius", tg::vec3f(1.0f, 0.35f, 0.20f)), binding::of("specular_roughness", 0.35f)},
         .swept = "subsurface_radius_scale",
         .from = 0.01f,
         .to = 0.8f});

    // Coverage, which the any-hit turns into a stochastic cutout — so these resolve as the accumulation converges rather
    // than as a blend.
    rows.push_back(
        {.name = "opacity",
         .shared = {binding::of("base_color", tg::vec3f(0.85f, 0.35f, 0.20f)), binding::of("specular_roughness", 0.25f)},
         .swept = "opacity",
         .from = 0.08f,
         .to = 1.0f});

    rows.push_back({.name = "diffuse",
                    .shared = {binding::of("base_color", tg::vec3f(0.72f, 0.55f, 0.42f)),
                               // No specular, so the row shows the diffuse lobe alone.
                               binding::of("specular_weight", 0.0f)},
                    .swept = "base_diffuse_roughness"});

    return rows;
}

/// One sphere per (row, column), each with its own material and its place in the grid.
[[nodiscard]] cc::vector<sv::mesh> make_grid(sv::triangle_geometry const& geometry,
                                             sv::mesh_attribute const& frames,
                                             sv::material_library& lib)
{
    auto const type = lib.acquire_type(sv::builtin_material::openpbr).value();
    auto const rows = make_rows();

    auto out = cc::vector<sv::mesh>();
    out.reserve(rows.size() * column_count);

    for (auto r = 0; r < rows.size(); ++r)
    {
        auto const& row = rows[r];
        for (auto c = 0; c < column_count; ++c)
        {
            float const t = float(c) / float(column_count - 1);
            float const v = row.from + (row.to - row.from) * t;

            auto overrides = row.shared;
            if (row.swept_is_direction)
            {
                // `v` is how far the direction leans off the normal, and the lean is built rather than trigonometric:
                // at 0 this is exactly (0, 0, 1), which is what "shares the base's normal" has to be.
                overrides.push_back(
                    sv::material_attribute_binding::of(cc::string(row.swept), tg::vec3f(v, 0.0f, 1.0f).normalized()));
            }
            else
            {
                overrides.push_back(sv::material_attribute_binding::of(cc::string(row.swept), v));
            }

            auto const id
                = lib.acquire(sv::material::create(cc::format("{}-{}", row.name, c), type, cc::move(overrides)));

            // Laid out ON the floor: columns run across in x, rows run away in z, and every sphere rests on the ground
            // rather than floating in a wall.
            // That is what makes the grid something to walk into rather than something to look at.
            float const x = (float(c) - float(column_count - 1) * 0.5f) * spacing;
            float const z = float(r) * spacing;

            out.push_back(
                sv::mesh{.name = cc::format("{}-{}", row.name, c),
                         .geometry = geometry,
                         // A mesh attribute is a value sharing its pinned payload, so every sphere naming the same
                         // frames costs a refcount bump rather than a copy — and one upload rather than thirty-five.
                         .attributes = {frames},
                         .transform = tg::affine_transform3f::make_translation(tg::vec3f(x, sphere_radius, z)),
                         .material = id});
        }
    }
    return out;
}
} // namespace

EXAMPLE("shaped-viewer/openpbr-spheres")
{
    // Built once, outside the loop: this is what pins the buffers and hashes their contents, so placing them every frame
    // uploads nothing after the first.
    // Every sphere shares one geometry and differs only by transform and material.
    auto const sphere = make_sphere(sphere_radius, 32, 48);
    auto const sphere_geometry = sv::triangle_geometry::create_from_indexed_triangles(sphere.positions, sphere.indices);

    // `tangent_frame` is what the openpbr type declares as a rotation, so the generated shader blends these as quaternions
    // rather than as four numbers — and the hit builds its shading frame from the result instead of the flat face normal.
    auto const sphere_frames
        = sv::mesh_attribute::create("tangent_frame", sv::attribute_frequency::per_vertex, sphere.frames);

    auto& lib = *sv::acquire_material_library().value();
    auto const grid = make_grid(sphere_geometry, sphere_frames, lib);

    // Everything below follows the grid rather than being set to fit it: a row added to `make_rows` must not fall off the
    // floor, out of the light, or behind the camera.
    float const grid_depth = float(make_rows().size() - 1) * spacing;
    float const grid_width = float(column_count - 1) * spacing;

    auto const floor = sv::mesh{.name = "ground",
                                .geometry = sv::triangle_geometry::create_from_positions(
                                    ground_quad(0.0f, cc::max(grid_width, grid_depth) + 8.0f)),
                                .material = lib.acquire(sv::material::create(
                                    "ground", lib.acquire_type(sv::builtin_material::openpbr).value(),
                                    {sv::material_attribute_binding::of("base_color", tg::vec3f(0.32f, 0.32f, 0.34f)),
                                     sv::material_attribute_binding::of("specular_roughness", 0.55f)}))};

    for (auto f : sv::interactive("shaped-viewer/openpbr-spheres"))
    {
        auto view = f.window().view();

        // Eye height at the front of the grid, looking down the rows.
        // Seeded once — after that the controller owns it, so this is where you START rather than where you are held.
        view.initial_fps({.position = tg::pos3d(0, 1.7, -4.5),
                          .yaw = tg::angle_d::make_from_degree(0.0),
                          .pitch = tg::angle_d::make_from_degree(-8.0)});

        // Right-drag looks, WASD moves, E and Q rise and fall, the wheel retunes the speed.
        // Re-asserted every frame, which is the contract: dropping the call hands the view back to the orbit controller.
        view.camera_style(sv::camera_style::fly);

        auto scene = view.add_scene();
        for (auto const& m : grid)
            scene.add_mesh(m);
        scene.add_mesh(floor);

        // A soft box over the whole grid, facing down.
        // It is what a smooth specular lobe actually reflects, so it — rather than the band-limited sky — is what gives the
        // metal and glass rows their highlights, and it has to span the grid or the far rows sit in its falloff.
        //
        // The emission falls as the rect grows: a light this large would otherwise blow out everything under it, and what
        // matters here is that each row is lit the same as its neighbours rather than that the scene is bright.
        float const light_u = grid_width * 0.5f + 2.0f;
        float const light_v = grid_depth * 0.5f + 2.0f;

        scene.add_light({.center = tg::pos3f(0, 7.0f, grid_depth * 0.5f),
                         .half_extent_u = tg::vec3f(light_u, 0, 0),
                         .half_extent_v = tg::vec3f(0, 0, light_v),
                         .emission = tg::vec3f(1, 1, 1) * (260.0f / (light_u * light_v))});

        scene.background(sv::background::studio().scaled(0.6f));
    }
}
