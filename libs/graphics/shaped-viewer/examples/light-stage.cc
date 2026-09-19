#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-viewer/all.hh>

// One scene under many colored lights at once — every kind the viewer has, in the roles they are for.
//
//   three spots   red, green and blue from three sides onto a white sphere.
//                 Light adds, so where all three land the sphere is white, and each of its shadows is lit by the other
//                 two — the shadows come out cyan, magenta and yellow.
//   two neons     thin rects on the back wall, seen by the camera.
//   a bulb        a warm point light by the steps, falling off with the square of the distance.
//   the moon      a small cool sun, low from the left, the dim blue fill the night sky would give.
//
// Every light is additive with every other, so what a region shows is the sum of what reaches it.
// ../docs/lights.md has the design behind all of it; shaped-viewer/lights shows each kind alone, side by side.
//
// Controls
//   left-drag    orbit          middle-drag    pan          wheel    zoom
//
// Run it:
//   uv run dev.py example shaped-viewer/light-stage
//   uv run dev.py example shaped-viewer/light-stage --capture    # headless, writes an image

namespace
{
/// The two triangles of the rect spanned from `corner` along `u` and `v`, facing along cross(u, v).
void push_quad(cc::vector<tg::pos3f>& out, tg::pos3f corner, tg::vec3f u, tg::vec3f v)
{
    auto const a = corner;
    auto const b = corner + u;
    auto const c = corner + u + v;
    auto const d = corner + v;
    for (auto const& p : {a, b, c, a, c, d})
        out.push_back(p);
}

/// The 12 triangles of an axis-aligned box from `lo` to `hi`, wound so every face's normal points outward.
void push_box(cc::vector<tg::pos3f>& out, tg::pos3f lo, tg::pos3f hi)
{
    // Corner i: bit 0 is +x, bit 1 is +y, bit 2 is +z.
    auto const corner
        = [&](int i) { return tg::pos3f((i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2]); };

    int const quads[6][4] = {{1, 3, 7, 5}, {0, 4, 6, 2}, {2, 6, 7, 3}, {0, 1, 5, 4}, {4, 5, 7, 6}, {0, 2, 3, 1}};
    for (auto const& q : quads)
        for (auto const k : {0, 1, 2, 0, 2, 3})
            out.push_back(corner(q[k]));
}

/// A mesh of `positions` shaded by `material`.
sv::mesh mesh_of(cc::string_view name, cc::vector<tg::pos3f> const& positions, sv::material_id material)
{
    return sv::mesh{.name = cc::string(name),
                    .geometry = sv::triangle_geometry::create_from_positions(positions),
                    .material = material};
}
} // namespace

EXAMPLE("shaped-viewer/light-stage")
{
    using namespace tg::literals;

    auto& lib = *sv::acquire_material_library().value();
    auto const openpbr = lib.acquire_type(sv::builtin_material::openpbr).value();
    auto const material = [&](cc::string_view name, cc::vector<sv::material_attribute_binding> bindings)
    { return lib.acquire(sv::material::create(cc::string(name), openpbr, cc::move(bindings))); };

    using binding = sv::material_attribute_binding;

    // A light floor, for the colored shadows to fall on, with a sheen the neons glow in.
    //
    // Not glossier than this, and neither is the chrome below: a spot is a point with a cone, so nothing but next-event
    // estimation can reach it, and a sharp lobe caught pointing at one from a bounce is a single enormous sample.
    // A sphere light would weight that against the bounce ray instead; it is designed and not landed (../docs/lights.md).
    auto const floor_material = material("stage-floor", {binding::of("base_color", tg::vec3f(0.55f, 0.55f, 0.56f)),
                                                         binding::of("specular_roughness", 0.45f)});
    auto const wall_material = material("stage-wall", {binding::of("base_color", tg::vec3f(0.5f, 0.5f, 0.52f)),
                                                       binding::of("specular_roughness", 0.8f)});
    auto const white = material("stage-white", {binding::of("base_color", tg::vec3f(0.88f, 0.88f, 0.88f)),
                                                binding::of("specular_roughness", 0.45f)});
    // Brushed rather than mirror-smooth, for the same reason, and because a perfect mirror turns the moon into a caustic
    // on the floor that only a bounce ray can find.
    auto const chrome
        = material("stage-chrome", {binding::of("base_color", tg::vec3f(0.95f, 0.95f, 0.96f)),
                                    binding::of("base_metalness", 1.0f), binding::of("specular_roughness", 0.28f)});

    // Built once, so every buffer is pinned and hashed and a frame uploads nothing.
    auto floor_positions = cc::vector<tg::pos3f>();
    push_quad(floor_positions, tg::pos3f(-8, 0, -8), tg::vec3f(0, 0, 16), tg::vec3f(16, 0, 0)); // faces +y
    auto const floor = mesh_of("floor", floor_positions, floor_material);

    auto wall_positions = cc::vector<tg::pos3f>();
    push_quad(wall_positions, tg::pos3f(-8, 0, 3.2f), tg::vec3f(0, 6, 0), tg::vec3f(16, 0, 0)); // faces -z
    auto const wall = mesh_of("wall", wall_positions, wall_material);

    // Three steps on the right, which the bulb beside them lights from close up.
    auto step_positions = cc::vector<tg::pos3f>();
    for (auto i = 0; i < 3; ++i)
        push_box(step_positions, tg::pos3f(2.2f + 0.55f * float(i), 0, 0.6f),
                 tg::pos3f(3.9f, 0.35f * float(i + 1), 2.0f));
    auto const steps = mesh_of("steps", step_positions, white);

    // The white sphere the three spots meet on.
    auto const center = tg::pos3f(-0.2f, 0.75f, 0.4f);

    for (auto f : sv::interactive("shaped-viewer/light-stage"))
    {
        auto view = f.window().view();
        view.initial_orbit({.target = tg::pos3d(0.2, 0.9, 0.6),
                            .distance = 9.5,
                            .azimuth = tg::angle_d::make_from_degree(12.0),
                            .elevation = tg::angle_d::make_from_degree(24.0)});

        auto scene = view.add_scene();
        scene.add_mesh(floor);
        scene.add_mesh(wall);
        scene.add_mesh(steps);

        scene.add_sphere(tg::sphere3f(center, 0.75f), white);
        scene.add_sphere(tg::sphere3f(tg::pos3f(1.4f, 0.5f, -0.9f), 0.5f), chrome);

        // A row of posts on the left, which the spots' spill picks out against the wall.
        for (auto i = 0; i < 5; ++i)
        {
            auto const x = -4.2f + 0.55f * float(i);
            scene.add_line(tg::segment3f(tg::pos3f(x, 0, 1.6f), tg::pos3f(x, 1.8f, 1.6f)), 0.07f, white);
        }

        // A night sky: dark, and only just blue enough to separate the wall's top from it.
        scene.background(sv::background::gradient(tg::vec3f(0.015f, 0.02f, 0.05f), tg::vec3f(0.005f, 0.005f, 0.008f)));

        // Red, green and blue from three sides, all aimed at the white sphere.
        // Candela chosen for about the same illuminance at the sphere from each, so their sum there is white.
        auto const spot_at = [&](cc::string_view id, tg::pos3f from, tg::vec3f color)
        { scene.add_spot_light(id, from, center - from, 32_deg_f, 20_deg_f).candela(70).color(color); };
        spot_at("red", tg::pos3f(-3.0f, 3.4f, -1.6f), tg::vec3f(1.0f, 0.06f, 0.04f));
        spot_at("green", tg::pos3f(0.4f, 3.6f, -3.4f), tg::vec3f(0.06f, 1.0f, 0.08f));
        spot_at("blue", tg::pos3f(2.6f, 3.4f, -0.8f), tg::vec3f(0.08f, 0.22f, 1.0f));

        // Neon strips on the back wall, facing the stage: cross(+y, +x) is -z.
        // Visible, so they read as the tubes they are.
        // Dim on purpose: a few nits is already past white on two channels, and much more clips all three to white.
        scene
            .add_rect_light("neon-magenta", tg::pos3f(-2.6f, 3.3f, 3.15f), tg::vec3f(0, 0.08f, 0), tg::vec3f(1.5f, 0, 0))
            .nits(2.5f)
            .color(tg::vec3f(1.0f, 0.12f, 0.75f))
            .visible_to_camera();
        scene.add_rect_light("neon-cyan", tg::pos3f(2.6f, 3.3f, 3.15f), tg::vec3f(0, 0.08f, 0), tg::vec3f(1.5f, 0, 0))
            .nits(2.5f)
            .color(tg::vec3f(0.1f, 0.85f, 1.0f))
            .visible_to_camera();

        // A warm bulb beside the steps.
        scene.add_point_light("bulb", tg::pos3f(3.2f, 1.9f, -0.2f)).candela(6).color(tg::vec3f(1.0f, 0.62f, 0.28f));

        // The moon: low from the left and a little behind, a cool fill under the colored lights.
        // Seven times the real moon's width, which is what keeps it from speckling: a tiny disc lighting the floor at all
        // has an enormous radiance, and the rare bounce ray that escapes into it lands one bright dot.
        scene.add_sun_light("moon", tg::vec3f(1.0f, -0.42f, -0.35f), 3.5_deg_f).lux(1.2f).color(tg::vec3f(0.55f, 0.65f, 1.0f));
    }
}
