#include <nexus/test.hh>
#include <shaped-viewer/all.hh>

using namespace cc::primitive_defines;

// Drawings: 2D vector content built once and instanced, flat on screen and in the scene.
//
// One `sv::drawing_set` holds an arrow and a small logo, built before the loop like a mesh.
// Every frame then places them: the arrow along each halfedge of a cube's triangles, in 3D, and the logo on a canvas
// layer over the scene, once from the top-left corner and once from the bottom-right.
// Nothing is uploaded after the first frame, since the set is keyed by its content.
//
// The arrow is built for an edge from (0, 0) to (1, 0) with its face toward +y; an instance's first vector is the edge
// itself and its second points into the face, so one drawing follows every edge whatever its length.
//
// Run it:
//   uv run dev.py example shaped-viewer/drawings

namespace
{
/// A closed polygon through `points`.
[[nodiscard]] sv::path polygon(std::initializer_list<tg::pos2f> points)
{
    auto p = sv::path();
    auto first = true;
    for (auto const q : points)
    {
        if (first)
            p.move_to(q);
        else
            p.line_to(q);
        first = false;
    }
    p.close();
    return p;
}

/// An arrow along the unit edge, inset from both ends and set into the face.
[[nodiscard]] sv::drawing arrow()
{
    auto d = sv::drawing();
    d.add_fill(
        polygon({tg::pos2f(0.18f, 0.075f), tg::pos2f(0.66f, 0.075f), tg::pos2f(0.66f, 0.045f), tg::pos2f(0.82f, 0.1f),
                 tg::pos2f(0.66f, 0.155f), tg::pos2f(0.66f, 0.125f), tg::pos2f(0.18f, 0.125f)}));
    return d;
}

/// A two-colour badge, 1 unit square: a dark plate and a diamond on it.
[[nodiscard]] sv::drawing logo()
{
    auto d = sv::drawing();
    d.add_fill(sr::slug_outline::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(1, 1))),
               {.color = tg::vec4f(0.08f, 0.1f, 0.14f, 0.85f)});
    d.add_fill(polygon({tg::pos2f(0.5f, 0.12f), tg::pos2f(0.88f, 0.5f), tg::pos2f(0.5f, 0.88f), tg::pos2f(0.12f, 0.5f)}),
               {.color = tg::vec4f(1.0f, 0.62f, 0.1f, 1)});
    return d;
}

/// The 12 triangles of an axis-aligned cube of side `size`, centred on the origin, wound outward.
[[nodiscard]] cc::vector<tg::pos3f> cube_triangles(f32 size)
{
    auto const h = size * 0.5f;
    auto const corner = [h](int i) { return tg::pos3f((i & 1) ? h : -h, (i & 2) ? h : -h, (i & 4) ? h : -h); };
    int const quads[6][4] = {{1, 3, 7, 5}, {0, 4, 6, 2}, {2, 6, 7, 3}, {0, 1, 5, 4}, {4, 5, 7, 6}, {0, 2, 3, 1}};
    auto out = cc::vector<tg::pos3f>();
    for (auto const& q : quads)
        for (auto const i : {0, 1, 2, 0, 2, 3})
            out.push_back(corner(q[i]));
    return out;
}
} // namespace

EXAMPLE("shaped-viewer/drawings")
{
    auto const triangles = cube_triangles(2.0f);
    auto const cube = sv::mesh{.name = "cube", .geometry = sv::triangle_geometry::create_from_positions(triangles)};

    auto set = sv::drawing_set();
    auto const arrow_id = set.add(arrow());
    auto const logo_id = set.add(logo());

    for (auto f : sv::interactive("shaped-viewer/drawings"))
    {
        auto view = f.window().view();
        view.initial_orbit({.target = tg::pos3d(0, 0, 0),
                            .distance = 6.0,
                            .azimuth = tg::angle_d::make_from_degree(35.0),
                            .elevation = tg::angle_d::make_from_degree(25.0)});

        auto scene = view.add_scene();
        scene.add_mesh(cube);
        scene.add_rect_light("key", tg::pos3f(0, 3, 0), tg::vec3f(0.9f, 0, 0), tg::vec3f(0, 0, 0.9f)).nits(14.0f);
        scene.background(sv::background::gradient(tg::vec3f(0.70f, 0.96f, 1.44f), tg::vec3f(0.21f, 0.28f, 0.37f)));

        // Every halfedge of every triangle: along the edge, set into the face, lifted a hair off it.
        for (auto t = isize(0); t + 2 < triangles.size(); t += 3)
            for (auto e = 0; e < 3; ++e)
            {
                auto const from = triangles[t + e];
                auto const to = triangles[t + (e + 1) % 3];
                auto const opposite = triangles[t + (e + 2) % 3];
                auto const edge = to - from;
                auto const toward = opposite - from;
                auto const normal = tg::dual(tg::cross(edge, toward)).normalized();
                auto const into = (toward - edge * (tg::dot(toward, edge) / tg::dot(edge, edge))).normalized();
                scene.add_drawing(set, arrow_id,
                                  {.at = from + normal * 0.002f,
                                   .x_axis = edge,
                                   .y_axis = into * edge.length(),
                                   .tint = tg::vec4f(0.15f, 0.9f, 1.0f, 1)});
            }

        auto canvas = view.add_canvas();
        canvas.add_drawing(set, logo_id, {.at = tg::pos2f(16, 16), .scale = 48});
        canvas.add_drawing(set, logo_id, {.at = tg::pos2f(16, 16), .scale = 32, .from = sv::corner::bottom_right});
    }
}
