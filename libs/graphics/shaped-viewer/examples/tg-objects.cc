#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/geometry/primitives/primitives.hh>

// tg objects drawn by their parts: `add_faces`, `add_edges` and `add_vertices`, straight from the tg types.
//
// Each object takes part by what it has — vertices(), edges(), faces() — so there is no per-type drawing code here.
// The row, left to right: an integer aabb, a rotated box, a tetrahedron, a frustum, and a quad and a triangle on their own.
// Each is drawn three ways: its faces, its edges joined at its own vertices, and its vertices alone above it.
//
// The scene calls hand back refs, and the style and material can still change through them until the frame ends:
// the frustum's edges are added in the default style, then thickened and recolored afterwards.
//
// Controls
//   left-drag    orbit          middle-drag    pan          wheel    zoom
//
// Run it:
//   uv run dev.py example shaped-viewer/tg-objects
//   uv run dev.py example shaped-viewer/tg-objects --capture    # headless, writes an image

EXAMPLE("shaped-viewer/tg-objects")
{
    auto& lib = *sv::acquire_material_library().value();
    auto const openpbr = lib.acquire_type(sv::builtin_material::openpbr).value();
    auto const material = [&](cc::string_view name, tg::vec3f color, float roughness)
    {
        return lib.acquire(sv::material::create(cc::string(name), openpbr,
                                                {sv::material_attribute_binding::of("base_color", color),
                                                 sv::material_attribute_binding::of("specular_roughness", roughness)}));
    };
    auto const surface = material("tg-objects-surface", tg::vec3f(0.55f, 0.6f, 0.68f), 0.6f);
    auto const ink = material("tg-objects-ink", tg::vec3f(0.08f, 0.09f, 0.11f), 0.4f);
    auto const accent = material("tg-objects-accent", tg::vec3f(0.95f, 0.45f, 0.1f), 0.35f);

    // integer coordinates draw as they are: every scalar converts at the call
    auto const aabb = tg::aabb3i(tg::pos3i(-4, 0, -1), tg::pos3i(-2, 2, 1));
    auto const aabb_above = tg::aabb3i(aabb.min + tg::vec3i(0, 3, 0), aabb.max + tg::vec3i(0, 3, 0));

    auto const rotation
        = tg::rigid_transform3f::make_rotation(tg::quat_f::make_rotation_y(tg::angle_f::make_from_degree(30)));
    auto const placed_box = tg::aabb3f(tg::pos3f(-0.8f, 0, -0.8f), tg::pos3f(0.8f, 1.6f, 0.8f))
                                .transformed(rotation)
                                .transformed(tg::rigid_transform3f::make_translation(tg::vec3f(-0.5f, 0, 0)));

    auto const tet = tg::tetrahedron3f(tg::pos3f(1, 0, -1), tg::pos3f(2.6f, 0, -0.6f), tg::pos3f(1.6f, 0, 1),
                                       tg::pos3f(1.8f, 1.8f, -0.2f));

    // a frustum looking down +z about the line x = 3.5, y = 1: 0.8 across at z = -0.5, 1.8 across at z = 1
    auto const plane = [](tg::vec3f n, float d) { return tg::plane3f(tg::normalize(n), d / n.length()); };
    auto const spread = 1.0f / 3.0f; // the half-width grows by a third of the depth
    auto const reach = 0.4f + 0.5f * spread;
    auto const frustum
        = tg::frustum3f(plane(tg::vec3f(-1, 0, -spread), reach - 3.5f), plane(tg::vec3f(1, 0, -spread), reach + 3.5f),
                        plane(tg::vec3f(0, -1, -spread), reach - 1.0f), plane(tg::vec3f(0, 1, -spread), reach + 1.0f),
                        plane(tg::vec3f(0, 0, -1), 0.5f), plane(tg::vec3f(0, 0, 1), 1));

    auto const quad = tg::quad3f(tg::pos3f(5.5f, 0, -1), tg::pos3f(7, 0, -1), tg::pos3f(7, 1.5f, -0.5f),
                                 tg::pos3f(5.5f, 1.5f, -0.5f));
    auto const tri = tg::triangle3f(tg::pos3f(5.5f, 0, 0.5f), tg::pos3f(7, 0, 0.5f), tg::pos3f(6.25f, 1.6f, 1));

    auto const above = tg::rigid_transform3f::make_translation(tg::vec3f(0, 2.6f, 0));

    for (auto f : sv::interactive("shaped-viewer/tg-objects"))
    {
        auto view = f.window().view();
        view.initial_orbit({.target = tg::pos3d(1.5, 1.6, 0),
                            .distance = 9.5,
                            .azimuth = tg::angle_d::make_from_degree(20.0),
                            .elevation = tg::angle_d::make_from_degree(22.0)});

        auto scene = view.add_scene();

        auto const edges = sv::edge_style{.radius = 0.025f};
        auto const corners = sv::vertex_style{.radius = 0.07f};

        scene.add_faces(aabb, surface);
        scene.add_edges(aabb, edges, ink);
        scene.add_vertices(aabb_above, corners, accent);

        scene.add_faces(placed_box, surface);
        scene.add_edges(placed_box, edges, ink);
        scene.add_vertices(placed_box.transformed(above), corners, accent);

        scene.add_faces(tet, surface);
        scene.add_edges(tet, edges, ink);
        scene.add_vertices(tet.transformed(above), corners, accent);

        // restyled after the fact, through the ref the call handed back
        scene.add_faces(frustum, surface);
        scene.add_edges(frustum).radius(0.04f).vertex_radius(0.08f).material(accent);

        scene.add_faces(quad, surface);
        scene.add_edges(quad, edges, ink);
        scene.add_faces(tri, surface);
        scene.add_edges(tri, edges, ink);

        scene.add_rect_light("key", tg::pos3f(1.5f, 6, 1), tg::vec3f(3, 0, 0), tg::vec3f(0, 0, 2)).nits(10.0f);
        scene.background(sv::background::gradient(tg::vec3f(0.70f, 0.96f, 1.44f), tg::vec3f(0.21f, 0.28f, 0.37f)));
    }
}
