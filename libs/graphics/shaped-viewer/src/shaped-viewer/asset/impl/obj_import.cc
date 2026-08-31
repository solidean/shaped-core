#include <babel-serializer/geometry/obj.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <shaped-viewer/asset/asset_loader.hh>
#include <shaped-viewer/asset/impl/asset_import.hh>
#include <shaped-viewer/material/material.hh>
#include <shaped-viewer/material/material_library.hh>
#include <shaped-viewer/material/material_type.hh>
#include <shaped-viewer/scene/mesh_data.hh>
#include <typed-geometry/linalg/quat.hh>

// Wavefront OBJ into sv's vocabulary.
//
// babel hands back a faithful mirror of the file — parallel attribute arrays plus faces as runs of corners — and
// deliberately does no triangulation and no vertex dedup, because both belong to whoever is building a mesh.
// So both are here.
//
// `.mtl` is planned in babel and deferred here, which is why an import carries geometry plus material NAMES only:
// each `usemtl` name is minted as an unbound `openpbr` material, so the slot exists for `override_material` to aim at.

namespace sv
{
namespace
{
namespace bo = babel::obj;

/// One contiguous run of faces that becomes one mesh: the plan's "one mesh per (geometry, material) pair".
struct face_run
{
    cc::string material_name; ///< empty for faces before the first `usemtl`
    i32 first_face = 0;
    i32 face_count = 0;
};

/// The file's faces split at every `usemtl`, in file order.
/// A file naming no material at all is one run over everything, which is the degenerate case rather than a branch.
[[nodiscard]] cc::vector<face_run> runs_of(bo::data const& doc)
{
    auto runs = cc::vector<face_run>();
    if (doc.materials.empty())
    {
        runs.push_back({.first_face = 0, .face_count = i32(doc.faces.size())});
        return runs;
    }

    // Faces before the first `usemtl` belong to no material and would otherwise be dropped silently.
    if (doc.materials[0].first_face > 0)
        runs.push_back({.first_face = 0, .face_count = doc.materials[0].first_face});

    for (auto const& m : doc.materials)
        if (m.face_count > 0)
            runs.push_back({.material_name = m.name, .first_face = m.first_face, .face_count = m.face_count});

    return runs;
}

/// Welds the corners of one face run into unique (position, texcoord, normal) vertices, triangulating as it goes.
///
/// Bucketed by position index rather than hashed: a position carries only a handful of distinct attribute
/// combinations, so the scan inside a bucket is short and — unlike a hash of the triple — exact.
struct run_builder
{
    bo::data const& doc;

    cc::vector<tg::pos3f> positions;
    cc::vector<tg::vec2f> uvs;
    cc::vector<tg::vec3f> normals;
    cc::vector<u32> indices;

    bool any_uv = false;
    bool any_normal = false;

    /// per source position, the new vertices built from it
    cc::vector<cc::vector<u32>> buckets;
    /// the corner each new vertex came from, so a bucket scan can compare the other two indices
    cc::vector<bo::corner> source_corners;

    [[nodiscard]] u32 vertex_for(bo::corner const& c)
    {
        auto& bucket = buckets[isize(c.position)];
        for (auto const existing : bucket)
        {
            auto const& other = source_corners[isize(existing)];
            if (other.texcoord == c.texcoord && other.normal == c.normal)
                return existing;
        }

        auto const index = u32(positions.size());
        positions.push_back(doc.positions[isize(c.position)]);

        // A corner with no uv or no normal still gets an element: the arrays are per vertex, and a hole would misalign
        // every element after it.
        uvs.push_back(c.texcoord >= 0 && c.texcoord < doc.texcoords.size() ? doc.texcoords[isize(c.texcoord)]
                                                                           : tg::vec2f::zero);
        normals.push_back(c.normal >= 0 && c.normal < doc.normals.size() ? doc.normals[isize(c.normal)]
                                                                         : tg::vec3f(0, 0, 1));
        any_uv = any_uv || c.texcoord >= 0;
        any_normal = any_normal || c.normal >= 0;

        bucket.push_back(index);
        source_corners.push_back(c);
        return index;
    }

    void build(face_run const& run)
    {
        buckets = cc::vector<cc::vector<u32>>::create_defaulted(doc.positions.size());

        for (auto f = run.first_face; f < run.first_face + run.face_count; ++f)
        {
            auto const& face = doc.faces[isize(f)];
            if (face.corner_count < 3)
                continue;

            // A fan is the right triangulation for the convex polygons OBJ actually carries; a concave one would need
            // an ear clip, which is mesh processing rather than importing.
            auto const first = doc.corners[isize(face.first_corner)];
            if (first.position < 0 || first.position >= doc.positions.size())
                continue;

            for (auto i = 1; i + 1 < face.corner_count; ++i)
            {
                auto const& b = doc.corners[isize(face.first_corner + i)];
                auto const& c = doc.corners[isize(face.first_corner + i + 1)];
                if (b.position < 0 || b.position >= doc.positions.size() || c.position < 0
                    || c.position >= doc.positions.size())
                    continue;

                indices.push_back(vertex_for(first));
                indices.push_back(vertex_for(b));
                indices.push_back(vertex_for(c));
            }
        }
    }
};
} // namespace

cc::result<asset_data> impl::import_obj(babel::obj::data const& doc,
                                        asset_loader_config const& cfg,
                                        material_library& lib,
                                        cc::string_view asset_name)
{
    auto const type = lib.acquire_type(builtin_material::openpbr);
    if (!type.has_value())
        return cc::error("shaped-viewer: the material library carries no 'openpbr' type to import into");

    auto out = asset_data();
    out.name = asset_name.empty() ? cc::string("obj") : cc::string(asset_name);

    if (!doc.material_libraries.empty() && cfg.import_materials)
        out.issues.push_back("obj: .mtl is not read yet, so every material carries openpbr's defaults under its own "
                             "name");

    auto const runs = runs_of(doc);

    // A repeated `usemtl` is several runs sharing one material, so the name alone would not tell two meshes apart.
    auto const name_count = [&](cc::string_view name)
    {
        auto n = isize(0);
        for (auto const& r : runs)
            if (r.material_name == name)
                ++n;
        return n;
    };

    auto seen = isize(0);
    for (auto ri = isize(0); ri < runs.size(); ++ri)
    {
        auto const& run = runs[ri];

        auto const base = run.material_name.empty() ? out.name : run.material_name;
        auto const name = name_count(run.material_name) > 1 ? cc::format("{}.{}", base, ri) : cc::string(base);

        if (cfg.include_mesh && !cfg.include_mesh(name))
            continue;

        auto builder = run_builder{.doc = doc};
        builder.build(run);
        if (builder.indices.empty())
            continue;

        auto attributes = cc::vector<mesh_attribute>();
        if (builder.any_uv)
            attributes.push_back(mesh_attribute::create("uv", attribute_frequency::per_vertex, builder.uvs));

        // A frame per vertex, with an arbitrary tangent: OBJ carries no tangent at all, and a normal alone is still
        // what separates a smooth surface from a faceted one.
        if (builder.any_normal && cfg.frames.prefer_file)
        {
            auto frames = cc::vector<tg::quat_f>();
            auto handedness = cc::vector<f32>();
            frames.reserve(builder.normals.size());
            handedness.reserve(builder.normals.size());
            for (auto const& n : builder.normals)
            {
                frames.push_back(impl::tangent_frame_of(n));
                handedness.push_back(1.0f);
            }
            attributes.push_back(
                mesh_attribute::create("tangent_frame", attribute_frequency::per_vertex, cc::move(frames)));
            attributes.push_back(
                mesh_attribute::create("tangent_handedness", attribute_frequency::per_vertex, cc::move(handedness)));
        }

        auto material = material_id::invalid;
        auto slot = isize(-1);
        if (cfg.import_materials && !run.material_name.empty())
        {
            for (auto i = isize(0); i < out.materials.size(); ++i)
                if (out.materials[i].name == run.material_name)
                    slot = i;

            if (slot < 0)
            {
                if (cfg.material_override)
                    material = cfg.material_override(run.material_name);

                // With no `.mtl`, every name mints the same unbound openpbr material — and a content-addressed library
                // hands back one id for all of them.
                // That is why a slot owns its meshes: the id cannot tell two of the file's names apart.
                if (material == material_id::invalid)
                    material = lib.acquire(
                        sv::material::create(cc::format("{}/{}", out.name, run.material_name), type.value(), {}));

                slot = out.materials.size();
                out.materials.push_back({.name = run.material_name, .material = material});
            }
            else
                material = out.materials[slot].material;

            out.materials[slot].meshes.push_back(i32(out.meshes.size()));
        }

        out.meshes.push_back({.name = name,
                              .geometry = triangle_geometry::create_from_indexed_triangles(cc::move(builder.positions),
                                                                                           cc::move(builder.indices)),
                              .attributes = cc::move(attributes),
                              .material = material});
        ++seen;
    }

    // One node per mesh, all roots at identity: OBJ describes no hierarchy, so the tree is flat by nature rather than
    // by flattening.
    for (auto i = isize(0); i < out.meshes.size(); ++i)
        out.nodes.push_back({.name = out.meshes[i].name, .parent = -1, .first_mesh = i32(i), .mesh_count = 1});

    if (seen == 0)
        return cc::error(cc::format("shaped-viewer: nothing to import from '{}'", out.name));

    return cc::move(out);
}
} // namespace sv
