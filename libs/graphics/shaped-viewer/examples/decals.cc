#include <nexus/test.hh>
#include <shaped-viewer/all.hh>

using namespace cc::primitive_defines;

// Decals: drawings projected onto the traced surfaces of a scene, painted into their material.
//
// A badge is projected straight down over a metal ball resting on the floor.
// It wraps over the ball's cap and carries on across the floor, and the ball's shadow falls across it, since it is part
// of the surfaces rather than drawn over them; on the ball it is paint, which takes the metal's shine away where it covers.
// A target is projected onto the cube's near face; it overhangs the face, and fades out where the projector meets the
// neighboring faces and the floor edge-on.
//
// Run it:
//   uv run dev.py example shaped-viewer/decals

namespace
{
/// A two-color badge, 1 unit square: a dark plate, a ring around its edge, and a diamond on it.
[[nodiscard]] sv::drawing badge()
{
    auto const orange = tg::vec4f(1.0f, 0.62f, 0.1f, 1);
    tg::pos2f const diamond[]
        = {tg::pos2f(0.5f, 0.12f), tg::pos2f(0.88f, 0.5f), tg::pos2f(0.5f, 0.88f), tg::pos2f(0.12f, 0.5f)};
    auto d = sv::drawing();
    d.add_fill(sv::path::rounded_rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(1, 1)), 0.12f),
               {.color = tg::vec4f(0.08f, 0.1f, 0.14f, 1)});
    d.add_stroke(sv::path::rounded_rectangle(tg::aabb2f(tg::pos2f(0.04f, 0.04f), tg::pos2f(0.96f, 0.96f)), 0.09f),
                 {.color = orange, .width = 0.03f});
    d.add_fill(sv::path::polygon(diamond), {.color = orange});
    return d;
}

/// A target 1 unit across, centered on (0.5, 0.5): two rings, a dashed one between them, and a cross through the middle.
[[nodiscard]] sv::drawing target()
{
    auto const red = tg::vec4f(0.85f, 0.12f, 0.1f, 1);
    auto const center = tg::pos2f(0.5f, 0.5f);
    auto d = sv::drawing();
    d.add_stroke(sv::path::circle(center, 0.45f), {.color = red, .width = 0.05f});
    d.add_stroke(sv::path::circle(center, 0.32f), {.color = red, .width = 0.02f, .dashes = {0.06f, 0.04f}});
    d.add_fill(sv::path::circle(center, 0.16f), {.color = red});
    tg::pos2f const across[] = {tg::pos2f(0.02f, 0.5f), tg::pos2f(0.98f, 0.5f)};
    tg::pos2f const down[] = {tg::pos2f(0.5f, 0.02f), tg::pos2f(0.5f, 0.98f)};
    d.add_stroke(sv::path::polyline(across), {.color = red, .width = 0.015f});
    d.add_stroke(sv::path::polyline(down), {.color = red, .width = 0.015f});
    return d;
}

/// The 12 triangles of an axis-aligned box from `lo` to `hi`.
[[nodiscard]] cc::vector<tg::pos3f> box_triangles(tg::pos3f lo, tg::pos3f hi)
{
    auto const corner
        = [&](int i) { return tg::pos3f((i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2]); };
    int const quads[6][4] = {{1, 3, 7, 5}, {0, 4, 6, 2}, {2, 6, 7, 3}, {0, 1, 5, 4}, {4, 5, 7, 6}, {0, 2, 3, 1}};
    auto out = cc::vector<tg::pos3f>();
    for (auto const& q : quads)
        for (auto const i : {0, 1, 2, 0, 2, 3})
            out.push_back(corner(q[i]));
    return out;
}
} // namespace

EXAMPLE("shaped-viewer/decals")
{
    auto const floor
        = sv::mesh{.name = "floor",
                   .geometry = sv::triangle_geometry::create_from_positions(
                       cc::vector<tg::pos3f>{tg::pos3f(-6, 0, -6), tg::pos3f(6, 0, -6), tg::pos3f(6, 0, 6),
                                             tg::pos3f(-6, 0, -6), tg::pos3f(6, 0, 6), tg::pos3f(-6, 0, 6)}),
                   .attributes = {sv::mesh_attribute::create_value("base_color", tg::vec3f(0.5f, 0.5f, 0.48f))}};

    // The face the camera sees is the plane z = -0.5.
    auto const cube
        = sv::mesh{.name = "cube",
                   .geometry = sv::triangle_geometry::create_from_positions(
                       box_triangles(tg::pos3f(0.4f, 0, -0.5f), tg::pos3f(1.8f, 1.4f, 0.9f))),
                   .attributes = {sv::mesh_attribute::create_value("base_color", tg::vec3f(0.55f, 0.66f, 0.8f))}};

    auto ball = sv::quadric_set();
    ball.name = "ball";
    ball.add_sphere(tg::sphere3f(tg::pos3f(-1.1f, 0.8f, 0.2f), 0.8f));
    ball.attributes.push_back(sv::mesh_attribute::create_value("base_color", tg::vec3f(0.9f, 0.9f, 0.88f)));
    ball.attributes.push_back(sv::mesh_attribute::create_value("metallic", 1.0f));
    ball.attributes.push_back(sv::mesh_attribute::create_value("roughness", 0.2f));

    auto const plate = badge();
    auto const rings = target();

    for (auto f : sv::interactive("shaped-viewer/decals"))
    {
        auto view = f.window().view();
        view.initial_orbit({.target = tg::pos3d(0, 0.6, 0),
                            .distance = 6.5,
                            .azimuth = tg::angle_d::make_from_degree(20.0),
                            .elevation = tg::angle_d::make_from_degree(30.0)});

        auto scene = view.add_scene();
        scene.add_mesh(floor);
        scene.add_mesh(cube);
        scene.add_quadrics(ball);
        scene.add_rect_light("key", tg::pos3f(-2.5f, 4.5f, 3), tg::vec3f(0.6f, 0, 0), tg::vec3f(0, 0, 0.6f)).nits(30.0f);
        scene.background(sv::background::gradient(tg::vec3f(0.30f, 0.40f, 0.60f), tg::vec3f(0.08f, 0.10f, 0.13f)));

        // Straight down from above the ball, 2.4 units square: x along +x and y along -z, read from above, so the
        // projection runs along y cross x, -y; its reach takes in the whole ball and the floor under it.
        scene.add_decal(plate, {.at = tg::pos3f(-1.1f - 1.2f, 2, 0.2f + 1.2f),
                                .x_axis = tg::vec3f(1, 0, 0),
                                .y_axis = tg::vec3f(0, 0, -1),
                                .scale = 2.4f,
                                .depth = 2.2f});

        // Onto the cube's near face, overhanging it: x along +x and y down, read from the camera's side, so the projection
        // runs along +z.
        scene.add_decal(rings, {.at = tg::pos3f(1.1f - 0.8f, 0.7f + 0.8f, -0.5f),
                                .x_axis = tg::vec3f(1, 0, 0),
                                .y_axis = tg::vec3f(0, -1, 0),
                                .scale = 1.6f,
                                .depth = 0.3f});

        auto canvas = view.add_canvas();
        canvas.add_text("Decals", tg::pos2f(16, 14), {.size = 22});
        canvas.add_text("drawings projected onto traced surfaces: lit, shadowed and curved with them",
                        tg::pos2f(16, 44), {.size = 13, .color = tg::vec4f(0.8f, 0.84f, 0.9f, 1)});
    }
}
