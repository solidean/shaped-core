#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/linalg/cross.hh>   // tg::cross, tg::dual — the wedge and its Hodge dual
#include <typed-geometry/linalg/pos_ops.hh> // tg::distance
#include <typed-geometry/linalg/vec_ops.hh> // tg::dot, tg::normalize

// Drawing a mesh's STRUCTURE rather than its surface: every vertex a sphere, every edge a tube, as analytic quadrics.
//
// This is what quadrics are for.
// Tessellating thirty smooth tubes and twelve smooth spheres would be thousands of triangles; here it is 42 primitives of
// about ninety bytes each, traced by a custom intersection shader over a procedural acceleration structure.
// The cost is the same whether the tubes are drawn at four pixels across or four hundred — there is no tessellation to be
// too coarse.
//
// Two things in here are worth reading for, beyond "it draws":
//
//   The edges are OPEN tubes, with their end caps not drawn.
//   The clipper that cuts a cylinder to length has a surface of its own — two planes — and whether that surface is drawn is
//   one bit on the primitive, not a second piece of geometry.
//   Here it is off: the joints already carry vertex spheres, so a cap would draw something nothing can see.
//
//   The colour is per PRIMITIVE, at `per_triangle` — the same frequency a mesh reads a per-face colour at.
//   That is not a coincidence: a quadric batch numbers its primitives and nothing else, so it admits exactly the
//   frequencies a mesh's primitive stream does, and one material definition generates one shader body for both.
//
// See libs/graphics/shaped-viewer/docs/quadrics.md for the design behind all of it.
//
// Controls
//   left-drag    orbit          middle-drag    pan          wheel    zoom
//
// Run it:
//   uv run dev.py example shaped-viewer/mesh-structure
//   uv run dev.py example shaped-viewer/mesh-structure --capture    # headless, writes an image

namespace
{
/// The 12 vertices of a regular icosahedron: the cyclic permutations of (0, ±1, ±φ).
/// Edge length is exactly 2, which is what `edges_of` keys on.
cc::vector<tg::pos3f> icosahedron_vertices()
{
    auto const phi = (1.0f + tg::sqrt(5.0f)) * 0.5f;

    auto out = cc::vector<tg::pos3f>();
    out.reserve(12);
    for (auto const s0 : {-1.0f, 1.0f})
        for (auto const s1 : {-1.0f, 1.0f})
        {
            out.push_back(tg::pos3f(0, s0 * 1.0f, s1 * phi));
            out.push_back(tg::pos3f(s0 * 1.0f, s1 * phi, 0));
            out.push_back(tg::pos3f(s1 * phi, 0, s0 * 1.0f));
        }
    return out;
}

/// Whether two icosahedron vertices share an edge, which for this solid is exactly "they are the minimum distance apart".
bool adjacent(tg::pos3f const& a, tg::pos3f const& b)
{
    return tg::distance(a, b) < 2.1f;
}

/// Every unordered pair of adjacent vertices — 30 of them.
/// Derived rather than tabulated, so it cannot disagree with the vertices above.
cc::vector<tg::comp2i> edges_of(cc::span<tg::pos3f const> vertices)
{
    auto out = cc::vector<tg::comp2i>();
    for (auto i = 0; i < int(vertices.size()); ++i)
        for (auto j = i + 1; j < int(vertices.size()); ++j)
            if (adjacent(vertices[i], vertices[j]))
                out.push_back(tg::comp2i(i, j));
    return out;
}

/// The 20 faces, as a raw triangle list wound outward.
///
/// A face is three mutually adjacent vertices; the winding is fixed against the centroid, which is the origin here — so a
/// triangle whose normal points inward has two of its corners swapped.
cc::vector<tg::pos3f> faces_of(cc::span<tg::pos3f const> vertices)
{
    auto out = cc::vector<tg::pos3f>();
    for (auto i = 0; i < int(vertices.size()); ++i)
        for (auto j = i + 1; j < int(vertices.size()); ++j)
            for (auto k = j + 1; k < int(vertices.size()); ++k)
            {
                if (!adjacent(vertices[i], vertices[j]) || !adjacent(vertices[j], vertices[k])
                    || !adjacent(vertices[i], vertices[k]))
                    continue;

                auto const a = vertices[i];
                auto b = vertices[j];
                auto c = vertices[k];
                if (tg::dot(tg::dual(tg::cross(b - a, c - a)), a - tg::pos3f::zero) < 0.0f)
                {
                    auto const t = b;
                    b = c;
                    c = t;
                }

                out.push_back(a);
                out.push_back(b);
                out.push_back(c);
            }
    return out;
}

/// A vertex's own colour, from the direction it points — so the structure reads as a whole rather than as 42 unrelated bits.
tg::vec3f colour_of(tg::pos3f const& v)
{
    auto const d = tg::normalize(v - tg::pos3f::zero);
    return tg::vec3f(0.5f + 0.5f * d[0], 0.5f + 0.5f * d[1], 0.5f + 0.5f * d[2]);
}
} // namespace

EXAMPLE("shaped-viewer/mesh-structure")
{
    auto const vertices = icosahedron_vertices();
    auto const edges = edges_of(vertices);

    // The surface itself, deliberately dark: a default-bright one competes with the structure instead of sitting under it.
    // `create_value` is the per_instance shorthand — one colour for the whole mesh, read out of the parameter block rather
    // than off a buffer.
    auto const surface
        = sv::mesh{.name = "icosahedron",
                   .geometry = sv::triangle_geometry::create_from_positions(faces_of(vertices)),
                   .attributes = {sv::mesh_attribute::create_value("base_color", tg::vec3f(0.14f, 0.15f, 0.17f))}};

    // The structure: one batch, built once, so placing it every frame uploads nothing.
    //
    // Both kinds go in the SAME set, which is what keeps it one acceleration structure and one instance whatever the
    // primitive count.
    // A set is one material, and per-primitive variation travels in an attribute instead.
    auto structure = sv::quadric_set();
    structure.name = "structure";
    structure.reserve(vertices.size() + edges.size());

    // One colour per primitive, in primitive order — which is what `PrimitiveIndex()` reads.
    auto colours = cc::vector<tg::vec3f>();
    colours.reserve(vertices.size() + edges.size());

    for (auto const& v : vertices)
    {
        structure.add(tg::sphere3f(v, 0.13f));
        colours.push_back(colour_of(v));
    }

    for (auto const& e : edges)
    {
        // OPEN tubes, deliberately: the joints already carry vertex spheres, so drawing the caps would be geometry
        // nothing can see.
        // `add(segment, radius, true)` closes them, at no change to the primitive's box.
        structure.add(tg::segment3f(vertices[e[0]], vertices[e[1]]), 0.045f);

        auto const a = colour_of(vertices[e[0]]);
        auto const b = colour_of(vertices[e[1]]);
        colours.push_back((a + b) * 0.5f);
    }

    structure.attributes.push_back(
        sv::mesh_attribute::create("base_color", sv::attribute_frequency::per_triangle, cc::move(colours)));

    for (auto f : sv::interactive("shaped-viewer/mesh-structure"))
    {
        auto view = f.window().view();
        view.initial_orbit({.target = tg::pos3d(0, 0, 0),
                            .distance = 5.4,
                            .azimuth = tg::angle_d::make_from_degree(30.0),
                            .elevation = tg::angle_d::make_from_degree(20.0)});

        auto scene = view.add_scene();
        scene.add_mesh(surface);
        scene.add_quadrics(structure);

        scene.add_light({.center = tg::pos3f(0, 4, 0),
                         .half_extent_u = tg::vec3f(1.2f, 0, 0),
                         .half_extent_v = tg::vec3f(0, 0, 1.2f),
                         .emission = tg::vec3f(18.0f, 18.0f, 18.0f)});

        scene.background(sv::background::gradient(tg::vec3f(0.36f, 0.42f, 0.55f), tg::vec3f(0.10f, 0.11f, 0.14f)));
    }
}
