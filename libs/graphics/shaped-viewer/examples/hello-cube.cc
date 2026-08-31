#include <nexus/test.hh>
#include <shaped-viewer/all.hh>

// The smallest thing that shows a viewer: one mesh, one light, one sky, in a window that orbits.
//
// Nothing here is set up by hand — no context, no window, no swapchain, no shader library, no resource acquire.
// `sv::interactive` owns the viewer for the loop's lifetime and acquires its context through `sv::set_acquire_context`,
// which is unset here, so the built-in default answers.
//
// The mesh is built ONCE, outside the loop, because that is what pins its buffers and hashes their contents.
// Placing it every frame then uploads nothing after the first: `add_mesh` is keyed by those hashes.
//
// Controls
//   left-drag    orbit          middle-drag    pan          wheel    zoom
//   Ctrl+wheel   magnify the image at the cursor, without touching the camera or restarting the accumulation
//
// Run it:
//   uv run dev.py example shaped-viewer/hello-cube

namespace
{
/// The 12 triangles of an axis-aligned cube of side `size`, centered on the origin, as a raw triangle list.
/// Wound counter-clockwise seen from outside, so every face's geometric normal points away from the cube.
cc::vector<tg::pos3f> cube_triangles(float size)
{
    auto const h = size * 0.5f;

    // Corner i: bit 0 is +x, bit 1 is +y, bit 2 is +z.
    auto const corner = [h](int i) { return tg::pos3f((i & 1) ? h : -h, (i & 2) ? h : -h, (i & 4) ? h : -h); };

    // One quad per face, each listed so its cross product points outward.
    int const quads[6][4] = {
        {1, 3, 7, 5}, // +x
        {0, 4, 6, 2}, // -x
        {2, 6, 7, 3}, // +y
        {0, 1, 5, 4}, // -y
        {4, 5, 7, 6}, // +z
        {0, 2, 3, 1}, // -z
    };

    auto out = cc::vector<tg::pos3f>();
    out.reserve(36);
    for (auto const& q : quads)
    {
        out.push_back(corner(q[0]));
        out.push_back(corner(q[1]));
        out.push_back(corner(q[2]));

        out.push_back(corner(q[0]));
        out.push_back(corner(q[2]));
        out.push_back(corner(q[3]));
    }
    return out;
}

/// One base color per face, repeated for both of its triangles — the `pbr` material type reads this by name.
/// The five attributes it also declares (metallic, roughness, emissive, normal, occlusion) are absent, so the type's own defaults win.
cc::vector<tg::vec3f> face_colors()
{
    tg::vec3f const per_face[6] = {
        tg::vec3f(0.85f, 0.25f, 0.20f), tg::vec3f(0.20f, 0.55f, 0.85f), tg::vec3f(0.30f, 0.75f, 0.35f),
        tg::vec3f(0.90f, 0.70f, 0.20f), tg::vec3f(0.70f, 0.35f, 0.80f), tg::vec3f(0.85f, 0.85f, 0.85f),
    };

    auto out = cc::vector<tg::vec3f>();
    out.reserve(12);
    for (auto const& c : per_face)
    {
        out.push_back(c);
        out.push_back(c);
    }
    return out;
}
} // namespace

EXAMPLE("shaped-viewer/hello-cube")
{
    // Built once: this is what pins the buffers and hashes them.
    // A mesh naming no material draws with `sv::default_material` — the builtin unbound `pbr` — so none is set here.
    auto const cube = sv::mesh_data{
        .name = "cube",
        .geometry = sv::triangle_geometry::create_from_positions(cube_triangles(2.0f)),
        .attributes = {sv::mesh_attribute::create("base_color", sv::attribute_frequency::per_triangle, face_colors())}};

    for (auto f : sv::interactive("shaped-viewer/hello-cube"))
    {
        // The window's own view, which fills the window when nothing lays it out.
        auto view = f.window().view();

        // Applied the first time this view id is seen; after that whatever the user orbited to wins.
        view.initial_orbit({.target = tg::pos3d(0, 0, 0),
                            .distance = 6.0,
                            .azimuth = tg::angle_d::make_from_degree(35.0),
                            .elevation = tg::angle_d::make_from_degree(25.0)});

        auto scene = view.add_scene();
        scene.add_mesh(cube);

        // An overhead rect facing down: cross(+x, +z) is -y, so the emitting face looks at the cube.
        scene.add_light({.center = tg::pos3f(0, 3, 0),
                         .half_extent_u = tg::vec3f(0.9f, 0, 0),
                         .half_extent_v = tg::vec3f(0, 0, 0.9f),
                         .emission = tg::vec3f(14.0f, 14.0f, 14.0f)});

        // A cool-blue sky, brighter overhead: the miss shader shows it behind the cube and it lights the cube too.
        scene.background(sv::background::gradient(tg::vec3f(0.70f, 0.96f, 1.44f), tg::vec3f(0.21f, 0.28f, 0.37f)));
    }
}
