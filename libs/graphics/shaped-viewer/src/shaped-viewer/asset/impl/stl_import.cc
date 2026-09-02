#include <babel-serializer/geometry/stl.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <shaped-viewer/asset/asset_loader.hh>
#include <shaped-viewer/asset/impl/asset_import.hh>
#include <shaped-viewer/scene/mesh.hh>

// STL into sv's vocabulary, which is nearly a copy.
//
// STL is a soup of independent triangles with no materials, no uvs and no hierarchy, so one file is one mesh and the
// positions go across as the raw triangle list they already are.
//
// The per-facet normals are DELIBERATELY dropped.
// The hit shader computes the geometric frame from the triangle it already has, and a per-triangle normal could only
// ever match it — so importing one would spend memory and bandwidth to store what is free.
// Welding the soup into an indexed mesh is the other thing worth doing here, and it waits for the mesh-processing
// helpers that eventually live on `tg::mesh`.

namespace sv
{
cc::result<asset_data> impl::import_stl(babel::stl::data const& doc,
                                        asset_loader_config const& cfg,
                                        cc::string_view asset_name,
                                        cc::vector<impl::asset_material_definition>& definitions)
{
    (void)definitions; // STL carries no materials, so there is nothing to define

    auto out = asset_data();
    out.name = asset_name.empty() ? cc::string("stl") : cc::string(asset_name);

    if (doc.is_empty())
        return cc::error(cc::format("shaped-viewer: nothing to import from '{}'", out.name));

    // The solid's own name when it has one, which only an ascii file does.
    auto const name = doc.name.empty() ? out.name : doc.name;
    if (cfg.include_mesh && !cfg.include_mesh(name))
        return cc::error(cc::format("shaped-viewer: every mesh of '{}' was filtered out", out.name));

    out.meshes.push_back({.name = name, .geometry = triangle_geometry::create_from_positions(doc.positions)});
    out.nodes.push_back({.name = name, .parent = -1, .first_mesh = 0, .mesh_count = 1});
    return cc::move(out);
}
} // namespace sv
