#include <nexus/test.hh>
#include <shaped-viewer/all.hh>

using namespace cc::primitive_defines;

// Drawings: 2D vector content built once and instanced, flat on screen and in the scene.
//
// One `sv::drawing_set` holds an arrow, a small logo and a legend, built before the loop like a mesh.
// Every frame then places them: the arrow along each halfedge of a cube's triangles, in 3D, and the logo on a canvas
// layer over the scene, once from the top-left corner and once from the bottom-right.
// The legend shows strokes: dashed, round-capped and mitred, and the logo nested inside it.
// Nothing is uploaded after the first frame, since the set is keyed by its content.
//
// The arrow is built for an edge from (0, 0) to (1, 0) with its face toward +y; an instance's first vector is the edge
// itself and its second points into the face, so one drawing follows every edge whatever its length.
//
// Run it:
//   uv run dev.py example shaped-viewer/drawings

namespace
{
/// A halfedge arrow: x runs along the edge as a fraction of it, y into the face in world units.
/// So one drawing fits every edge length, while its thickness and its distance from the edge stay the same.
/// It is a half arrow, a harpoon: the side along the edge runs straight into the tip, and the one barb points into the
/// face, so of an edge's two halfedges each shows which face it belongs to.
[[nodiscard]] sv::drawing arrow()
{
    tg::pos2f const outline[] = {tg::pos2f(0.24f, 0.065f), tg::pos2f(0.78f, 0.065f), tg::pos2f(0.66f, 0.145f),
                                 tg::pos2f(0.67f, 0.095f), tg::pos2f(0.24f, 0.095f)};
    auto d = sv::drawing();
    d.add_fill(sv::path::polygon(outline));
    return d;
}

/// A two-color badge, 1 unit square: a dark plate, a ring around its edge, and a diamond on it.
[[nodiscard]] sv::drawing logo()
{
    tg::pos2f const diamond[]
        = {tg::pos2f(0.5f, 0.12f), tg::pos2f(0.88f, 0.5f), tg::pos2f(0.5f, 0.88f), tg::pos2f(0.12f, 0.5f)};
    auto const plate = tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(1, 1));
    auto d = sv::drawing();
    d.add_fill(sv::path::rounded_rectangle(plate, 0.12f), {.color = tg::vec4f(0.08f, 0.1f, 0.14f, 0.85f)});
    d.add_stroke(sv::path::rounded_rectangle(tg::aabb2f(tg::pos2f(0.04f, 0.04f), tg::pos2f(0.96f, 0.96f)), 0.09f),
                 {.color = tg::vec4f(1.0f, 0.62f, 0.1f, 1), .width = 0.03f});
    d.add_fill(sv::path::polygon(diamond), {.color = tg::vec4f(1.0f, 0.62f, 0.1f, 1)});
    return d;
}

/// A legend in pixels, 220 by 96: a framed panel holding a dashed line, a round-capped curve, a mitred zigzag, and the
/// badge nested twice, built once and stamped where the panel wants it.
[[nodiscard]] sv::drawing legend(sv::drawing const& badge)
{
    auto const white = tg::vec4f(1, 1, 1, 1);
    auto d = sv::drawing();
    d.add_fill(sv::path::rounded_rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(220, 96)), 10),
               {.color = tg::vec4f(0.08f, 0.1f, 0.14f, 0.8f)});
    d.add_stroke(sv::path::rounded_rectangle(tg::aabb2f(tg::pos2f(0.5f, 0.5f), tg::pos2f(219.5f, 95.5f)), 9.5f),
                 {.color = tg::vec4f(1, 1, 1, 0.35f), .width = 1});

    tg::pos2f const dashed[] = {tg::pos2f(14, 20), tg::pos2f(150, 20)};
    d.add_stroke(sv::path::polyline(dashed), {.color = tg::vec4f(0.15f, 0.9f, 1.0f, 1), .width = 2, .dashes = {8, 5}});

    auto wave = sv::path();
    wave.move_to(tg::pos2f(14, 52))
        .quad_to(tg::pos2f(48, 20), tg::pos2f(82, 52))
        .quad_to(tg::pos2f(116, 84), tg::pos2f(150, 52));
    d.add_stroke(wave, {.color = white, .width = 5, .cap = sr::stroke_cap::round});

    tg::pos2f const zigzag[] = {tg::pos2f(14, 84),  tg::pos2f(38, 68),  tg::pos2f(62, 84), tg::pos2f(86, 68),
                                tg::pos2f(110, 84), tg::pos2f(134, 68), tg::pos2f(150, 78)};
    d.add_stroke(sv::path::polyline(zigzag),
                 {.color = tg::vec4f(1.0f, 0.62f, 0.1f, 1), .width = 4, .join = sr::stroke_join::miter});

    d.add_drawing(badge, {.at = tg::pos2f(166, 12), .scale = 40});
    d.add_drawing(badge, {.at = tg::pos2f(170, 58), .scale = 28, .tint = tg::vec4f(1, 1, 1, 0.5f)});
    return d;
}

/// The 12 triangles of an axis-aligned cube of side `size`, centered on the origin, wound outward.
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
    auto const legend_id = set.add(legend(logo()));

    for (auto f : sv::interactive("shaped-viewer/drawings"))
    {
        auto view = f.window().view();
        view.initial_orbit({.target = tg::pos3d(0, 0, 0),
                            .distance = 6.0,
                            .azimuth = tg::angle_d::make_from_degree(35.0),
                            .elevation = tg::angle_d::make_from_degree(32.0)});

        auto scene = view.add_scene();
        scene.add_mesh(cube);
        scene.add_rect_light("key", tg::pos3f(0, 3, 0), tg::vec3f(0.9f, 0, 0), tg::vec3f(0, 0, 0.9f)).nits(4.0f);
        scene.background(sv::background::gradient(tg::vec3f(0.70f, 0.96f, 1.44f), tg::vec3f(0.21f, 0.28f, 0.37f)));

        // Every halfedge of every triangle: along the edge, a fixed distance into the face, lifted a hair off it.
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
                                   .y_axis = into,
                                   .tint = tg::vec4f(0.07f, 0.12f, 0.25f, 0.9f)});
            }

        // A label on each side face, lying on it: x along the face, y down it, lifted a hair off it.
        struct face_label
        {
            char const* text;
            tg::vec3f normal;
            tg::vec3f right;
        };
        face_label const labels[] = {{"+X", tg::vec3f(1, 0, 0), tg::vec3f(0, 0, 1)},
                                     {"-X", tg::vec3f(-1, 0, 0), tg::vec3f(0, 0, -1)},
                                     {"+Z", tg::vec3f(0, 0, 1), tg::vec3f(-1, 0, 0)},
                                     {"-Z", tg::vec3f(0, 0, -1), tg::vec3f(1, 0, 0)}};
        for (auto const& l : labels)
            scene.add_text(l.text,
                           {.at = tg::pos3f(0, 0, 0) + l.normal * 1.003f - l.right * 0.25f + tg::vec3f(0, 0.2f, 0),
                            .x_axis = l.right,
                            .y_axis = tg::vec3f(0, -1, 0)},
                           {.size = 0.4f, .color = tg::vec4f(1, 0.85f, 0.3f, 1)});

        // Annotations on four corners and the center: flat on screen, kept clear of the view's edges, and drawn as
        // hidden lines while the corner behind the cube is out of sight; the center is always inside it, so it shows.
        for (auto const& c : {tg::pos3f(1, 1, 1), tg::pos3f(-1, 1, -1), tg::pos3f(1, -1, -1), tg::pos3f(-1, -1, 1)})
            scene.add_annotation(c, cc::format("corner ({}, {}, {})", c[0], c[1], c[2]), {.offset = tg::vec2f(64, 56)});
        scene.add_annotation(tg::pos3f(0, 0, 0), "center of mass",
                             {.side = sv::annotation_side::below_right,
                              .offset = tg::vec2f(220, 40),
                              .leader = {.shape = sv::leader_shape::straight, .color = tg::vec4f(1, 0.62f, 0.1f, 1)},
                              .occluded = sv::annotation_occluded::show});

        auto canvas = view.add_canvas();
        canvas.add_drawing(set, logo_id, {.at = tg::pos2f(16, 16), .scale = 48});
        canvas.add_text("Drawings", tg::pos2f(76, 18), {.size = 22});
        canvas.add_text("halfedge arrows, face labels, strokes, annotations", tg::pos2f(76, 46),
                        {.size = 13, .color = tg::vec4f(0.08f, 0.1f, 0.14f, 1)});
        canvas.add_drawing(set, legend_id, {.at = tg::pos2f(16, 16), .from = sv::corner::bottom_left});
        canvas.add_drawing(set, logo_id, {.at = tg::pos2f(16, 16), .scale = 32, .from = sv::corner::bottom_right});
        canvas.add_text("bottom-right, 16 px in", {.at = tg::pos2f(56, 22), .from = sv::corner::bottom_right},
                        {.size = 13});
    }
}
