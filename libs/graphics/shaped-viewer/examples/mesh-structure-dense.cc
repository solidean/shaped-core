#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/record/log.hh> // CC_LOG_INFO
#include <nexus/test.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/linalg/cross.hh>   // tg::cross, tg::dual
#include <typed-geometry/linalg/pos_ops.hh> // tg::distance
#include <typed-geometry/linalg/vec_ops.hh> // tg::dot, tg::normalize

using namespace cc::primitive_defines;

// The same idea as `mesh-structure`, at the scale a real mesh actually has: a geodesic sphere's whole edge network,
// drawn as analytic tubes.
//
// Five subdivisions of an icosahedron is 10,242 vertices and 30,720 edges, so **40,962 quadric primitives** — about
// 3.8 MB of records, one acceleration structure, one instance, one draw.
// `subdivisions` below is the knob, and each level multiplies the primitive count by roughly four:
//
//     level   vertices   edges      primitives
//       3          642     1,920         2,562
//       4        2,562     7,680        10,242
//       5       10,242    30,720        40,962
//       6       40,962   122,880       163,842
//
// Tessellating thirty thousand smooth tubes would be millions of triangles, and it would still be faceted at a close
// zoom where these never are — an analytic primitive has no resolution to run out of.
//
// It is also the demonstration that the batch is the unit of cost rather than the primitive.
// Nothing here is drawn in a loop: the whole network is ONE `sv::quadric_set`, placed once per frame, and an unchanged
// one re-uploads nothing whatever its size.
//
// **The edges carry data rather than decoration.**
// Each tube is coloured by its own length, over the range the mesh actually spans — which on a subdivided icosahedron
// draws the twelve pentagonal vertices the construction cannot avoid, as a pattern you can see rather than a number
// you have to trust.
// That is `per_quadric`: one value per primitive, indexed the way `per_triangle` is.
// `mesh-structure` shows the other quadric frequency, `per_quadric_end`, which blends two values along each tube.
//
// Controls
//   left-drag    orbit          middle-drag    pan          wheel    zoom
//
// Run it:
//   uv run dev.py example shaped-viewer/mesh-structure-dense
//   uv run dev.py example shaped-viewer/mesh-structure-dense --capture

namespace
{
/// How many times the icosahedron is subdivided — see the table above for what each level costs.
/// Turn it up to watch the primitive count go with it; the authoring code below does not change.
///
/// 5 is what this is committed at because it is the last level whose tubes still RESOLVE at 1280x720.
/// 6 renders fine — 163,842 primitives, captured in about three seconds — but reads as a texture rather than as
/// geometry, which loses the point of drawing it.
constexpr int subdivisions = 5;

constexpr float sphere_radius = 2.0f;

/// A triangle mesh as the two things this example needs from one: its vertices, and its triangles as index triples.
struct indexed_mesh
{
    cc::vector<tg::pos3f> vertices;
    cc::vector<tg::comp3i> triangles;
};

/// The regular icosahedron, with its vertices pushed out onto the sphere of `sphere_radius`.
indexed_mesh icosahedron()
{
    auto const phi = (1.0f + tg::sqrt(5.0f)) * 0.5f;

    auto m = indexed_mesh();
    for (auto const s0 : {-1.0f, 1.0f})
        for (auto const s1 : {-1.0f, 1.0f})
        {
            m.vertices.push_back(tg::pos3f(0, s0, s1 * phi));
            m.vertices.push_back(tg::pos3f(s0, s1 * phi, 0));
            m.vertices.push_back(tg::pos3f(s1 * phi, 0, s0));
        }

    // A face is three mutually adjacent vertices; on this solid adjacency is exactly "the minimum distance apart".
    auto const adjacent = [&](int a, int b) { return tg::distance(m.vertices[a], m.vertices[b]) < 2.1f; };

    for (auto i = 0; i < int(m.vertices.size()); ++i)
        for (auto j = i + 1; j < int(m.vertices.size()); ++j)
            for (auto k = j + 1; k < int(m.vertices.size()); ++k)
            {
                if (!adjacent(i, j) || !adjacent(j, k) || !adjacent(i, k))
                    continue;

                // Wound outward, checked against the centroid — which is the origin here.
                auto const a = m.vertices[i];
                auto const outward
                    = tg::dot(tg::dual(tg::cross(m.vertices[j] - a, m.vertices[k] - a)), a - tg::pos3f::zero);
                m.triangles.push_back(outward < 0.0f ? tg::comp3i(i, k, j) : tg::comp3i(i, j, k));
            }

    for (auto& v : m.vertices)
        v = tg::pos3f::zero + tg::normalize(v - tg::pos3f::zero) * sphere_radius;

    return m;
}

/// One subdivision step: every triangle becomes four, and each new vertex is pushed out onto the sphere.
///
/// The midpoint cache is what keeps the mesh WELDED — without it each triangle would mint its own copy of a shared
/// edge's midpoint, and the edge network below would come out as unconnected fragments rather than as a mesh.
indexed_mesh subdivide(indexed_mesh const& in)
{
    auto out = indexed_mesh();
    out.vertices = in.vertices;

    auto midpoints = cc::map<u64, int>();
    auto const midpoint = [&](int a, int b)
    {
        auto const lo = u64(a < b ? a : b);
        auto const hi = u64(a < b ? b : a);
        auto const key = (lo << 32) | hi;

        if (auto const* const resident = midpoints.get_ptr(key); resident != nullptr)
            return *resident;

        auto const m = in.vertices[a] + (in.vertices[b] - in.vertices[a]) * 0.5f;
        out.vertices.push_back(tg::pos3f::zero + tg::normalize(m - tg::pos3f::zero) * sphere_radius);

        auto const index = int(out.vertices.size()) - 1;
        midpoints[key] = index;
        return index;
    };

    for (auto const& t : in.triangles)
    {
        auto const ab = midpoint(t[0], t[1]);
        auto const bc = midpoint(t[1], t[2]);
        auto const ca = midpoint(t[2], t[0]);

        out.triangles.push_back(tg::comp3i(t[0], ab, ca));
        out.triangles.push_back(tg::comp3i(t[1], bc, ab));
        out.triangles.push_back(tg::comp3i(t[2], ca, bc));
        out.triangles.push_back(tg::comp3i(ab, bc, ca));
    }

    return out;
}

/// Every edge of `m`, each once.
/// Keyed on the ordered index pair, so the two triangles sharing an edge contribute it a single time.
cc::vector<tg::comp2i> unique_edges(indexed_mesh const& m)
{
    auto seen = cc::map<u64, bool>();
    auto out = cc::vector<tg::comp2i>();

    for (auto const& t : m.triangles)
        for (auto const& e : {tg::comp2i(t[0], t[1]), tg::comp2i(t[1], t[2]), tg::comp2i(t[2], t[0])})
        {
            auto const lo = u64(e[0] < e[1] ? e[0] : e[1]);
            auto const hi = u64(e[0] < e[1] ? e[1] : e[0]);
            auto const key = (lo << 32) | hi;

            if (seen.get_ptr(key) != nullptr)
                continue;

            seen[key] = true;
            out.push_back(e);
        }

    return out;
}

/// The triangles of `m` as a raw position list, which is what `triangle_geometry` takes.
cc::vector<tg::pos3f> triangle_soup(indexed_mesh const& m)
{
    auto out = cc::vector<tg::pos3f>();
    out.reserve(m.triangles.size() * 3);
    for (auto const& t : m.triangles)
        for (auto const i : {0, 1, 2})
            out.push_back(m.vertices[t[i]]);
    return out;
}

/// A blue-to-red ramp over [0, 1], for reading a scalar off the picture.
tg::vec3f ramp(float t)
{
    auto const u = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return tg::vec3f(0.15f + 0.80f * u, 0.30f + 0.35f * (1.0f - tg::abs(2.0f * u - 1.0f)), 0.95f - 0.80f * u);
}
} // namespace

EXAMPLE("shaped-viewer/mesh-structure-dense")
{
    auto mesh = icosahedron();
    for (auto i = 0; i < subdivisions; ++i)
        mesh = subdivide(mesh);

    auto const edges = unique_edges(mesh);

    CC_LOG_INFO("mesh-structure-dense: {} vertices, {} edges — {} quadric primitives in one batch",
                mesh.vertices.size(), edges.size(), mesh.vertices.size() + edges.size());

    auto const surface
        = sv::mesh{.name = "geodesic sphere",
                   .geometry = sv::triangle_geometry::create_from_positions(triangle_soup(mesh)),
                   .attributes = {sv::mesh_attribute::create_value("base_color", tg::vec3f(0.10f, 0.11f, 0.13f))}};

    // The range the colouring is normalized over, so the variation fills the ramp rather than sitting in a corner of it.
    // On a subdivided icosahedron it is narrow — a few percent — which is exactly why it has to be measured rather than
    // assumed.
    auto shortest = 1e30f;
    auto longest = 0.0f;
    for (auto const& e : edges)
    {
        auto const len = tg::distance(mesh.vertices[e[0]], mesh.vertices[e[1]]);
        shortest = cc::min(shortest, len);
        longest = cc::max(longest, len);
    }

    // The radii are derived from the mesh rather than fixed, so turning `subdivisions` up thins the network instead of
    // filling it in solid.
    auto total_length = 0.0f;
    for (auto const& e : edges)
        total_length += tg::distance(mesh.vertices[e[0]], mesh.vertices[e[1]]);
    auto const mean_edge = total_length / float(edges.size());

    auto const vertex_radius = mean_edge * 0.30f;
    auto const tube_radius = mean_edge * 0.13f;

    auto structure = sv::quadric_set();
    structure.name = "edge network";
    structure.reserve(mesh.vertices.size() + edges.size());

    // One value per primitive, in primitive order — `per_quadric`, which is what `PrimitiveIndex()` reads.
    auto colours = cc::vector<tg::vec3f>();
    colours.reserve(mesh.vertices.size() + edges.size());

    for (auto const& v : mesh.vertices)
    {
        structure.add(tg::sphere3f(v, vertex_radius));
        colours.push_back(tg::vec3f(0.85f, 0.85f, 0.88f)); // neutral, so the edges carry the reading
    }

    for (auto const& e : edges)
    {
        // Flat caps: the vertex spheres already cover every joint, so round ones would be two more primitives each of
        // geometry nothing can see — 61,440 of them at this subdivision.
        structure.add(tg::segment3f(mesh.vertices[e[0]], mesh.vertices[e[1]]), tube_radius);

        auto const len = tg::distance(mesh.vertices[e[0]], mesh.vertices[e[1]]);
        colours.push_back(ramp((len - shortest) / cc::max(longest - shortest, 1e-6f)));
    }

    structure.attributes.push_back(
        sv::mesh_attribute::create("base_color", sv::attribute_frequency::per_quadric, cc::move(colours)));

    for (auto f : sv::interactive("shaped-viewer/mesh-structure-dense"))
    {
        auto view = f.window().view();
        view.initial_orbit({.target = tg::pos3d(0, 0, 0),
                            .distance = 3.9,
                            .azimuth = tg::angle_d::make_from_degree(25.0),
                            .elevation = tg::angle_d::make_from_degree(18.0)});

        auto scene = view.add_scene();
        scene.add_mesh(surface);
        scene.add_quadrics(structure);

        scene.add_light({.center = tg::pos3f(0, 5, 1.5f),
                         .half_extent_u = tg::vec3f(1.6f, 0, 0),
                         .half_extent_v = tg::vec3f(0, 0, 1.6f),
                         .emission = tg::vec3f(22.0f, 22.0f, 22.0f)});

        scene.background(sv::background::gradient(tg::vec3f(0.30f, 0.35f, 0.46f), tg::vec3f(0.07f, 0.08f, 0.10f)));
    }
}
