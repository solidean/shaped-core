#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/string/format.hh>
#include <shaped-viewer/asset/asset_loader.hh>
#include <shaped-viewer/asset/impl/asset_import.hh>
#include <shaped-viewer/material/material.hh>
#include <shaped-viewer/material/material_library.hh>
#include <shaped-viewer/material/material_type.hh>

// The one step of an import that must run where the material library is owned.
//
// Everything else an importer does is work over bytes, which is what lets it run on a worker; this is the seam where
// that work meets a library, and it is deliberately tiny.

namespace sv
{
void impl::acquire_asset_materials(asset_data& out,
                                   cc::span<asset_material_definition const> definitions,
                                   asset_loader_config const& cfg,
                                   material_library& lib)
{
    if (definitions.empty())
        return;

    auto const type = lib.acquire_type(builtin_material::openpbr);
    if (!type.has_value())
    {
        out.issues.push_back("shaped-viewer: the material library carries no 'openpbr' type, so no material was "
                             "imported");
        return;
    }

    CC_ASSERT(definitions.size() == out.materials.size(), "every definition names a slot the import already made");

    for (auto i = isize(0); i < definitions.size(); ++i)
    {
        auto const& definition = definitions[i];

        // The caller's hook first: what it returns is a library id, so it can only run here — and returning one saves
        // building and acquiring a material they were going to replace anyway.
        auto id = material_id::invalid;
        if (cfg.material_override)
            id = cfg.material_override(definition.name);

        if (id == material_id::invalid)
        {
            // Namespaced by the asset, because `material_library`'s name lookup is last-wins and a convenience rather
            // than an identity — two files each with a "glass" would otherwise fight over it.
            // Content addressing still dedupes genuinely identical materials across files.
            auto const library_name = cc::format("{}/{}", out.name, definition.name);
            id = lib.acquire(material::create(library_name, type.value(), definition.bindings));
        }

        // The slot's own meshes rather than everything sharing the id: a content-addressed library may well have
        // handed the same one to another slot, and that slot keeps what it had.
        out.materials[i].material = id;
        for (auto const index : out.materials[i].meshes)
            out.meshes[isize(index)].material = id;
    }
}
} // namespace sv
