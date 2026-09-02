#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/scene/mesh.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/transform/transform.hh>

/// One material an asset brought with it: the id minted for it, under the name the FILE gave it.
///
/// `name` is the file's own, so a caller who read the asset in a DCC tool can name what they saw.
/// The name the material carries inside `material_library` is namespaced by the asset (`"car.glb/glass"`), because the
/// library's name lookup is last-wins and a convenience rather than an identity — two files with a `"glass"` would
/// otherwise fight over it.
/// Content addressing still dedupes genuinely identical materials across files, namespacing or not.
///
/// `meshes` is why a slot is more than a name and an id: `material_library` is content-addressed, so two of a file's
/// materials that happen to be bound identically come back as ONE id.
/// Overriding by id would then move both, which is never what a caller who named one of them meant — so a slot records
/// which meshes it covers, and that is what an override rewrites.
struct sv::asset_material
{
    cc::string name;
    material_id material = material_id::invalid;

    /// indices into `asset_data::meshes` that this slot draws
    cc::vector<i32> meshes;
};

/// One node of the imported hierarchy, kept for callers who want the tree the file described.
///
/// The importer flattens by default, so this is a record rather than something the meshes depend on.
/// `parent` is -1 for a root, and a parent always precedes its children, so one forward pass can accumulate transforms.
/// `transform` is the node's LOCAL transform whatever `flatten_hierarchy` said; what flattening changes is the transform
/// on the MESHES, not the one here.
struct sv::asset_node
{
    cc::string name;
    i32 parent = -1;

    /// the node's own transform, relative to its parent
    tg::affine_transform3f transform = {};

    /// the meshes this node placed, as a run in `asset_data::meshes`
    i32 first_mesh = 0;
    i32 mesh_count = 0;
};

/// Everything one loaded file describes, as plain data made of the types the scene API already takes.
///
/// That is the whole reason there is almost no API here: filtering and overriding are ordinary code over these vectors
/// rather than options on the loader.
///
///     for (auto const& m : car.meshes)
///         if (m.name.starts_with("wheel"))
///             scene.add_mesh(m);
///
///     car.override_material("glass", my_glass);   // rewrites every mesh bound to that slot
///
/// `meshes` is flat and world-placed by default — one per contiguous (geometry, material) pair, whatever the format.
/// A glTF mesh with three primitives becomes three meshes; an OBJ's `usemtl` runs become one mesh per run.
/// That falls out of `sv::resident_mesh` carrying exactly one `material_id`, rather than being a choice the importer made.
///
/// Nothing here has met a device: every payload is a pinned, content-hashed `mesh`, so an asset can be loaded on a
/// worker thread and long before a viewer exists.
struct sv::asset_data
{
    /// what the asset was loaded from — a uri, or whatever name the caller passed for a byte range
    cc::string name;

    cc::vector<sv::mesh> meshes;

    /// file order: each material's own name plus the id minted for it
    cc::vector<sv::asset_material> materials;

    /// the hierarchy, in a parents-first order
    cc::vector<sv::asset_node> nodes;

    /// babel's issues plus the importer's own, forwarded verbatim
    ///
    /// A successful load with a non-empty `issues` is the normal case for a real-world asset: check it before
    /// concluding that what came back is everything the file described.
    cc::vector<cc::string> issues;

    // queries
public:
    [[nodiscard]] bool is_empty() const { return meshes.empty(); }

    /// The mesh named `name`, or nullptr.
    /// First match wins — a name is not an identity here either.
    [[nodiscard]] sv::mesh const* find_mesh(cc::string_view name) const;

    /// Every mesh drawn by the material the file called `name`, in mesh order.
    /// The pointers borrow from `meshes`, so they die with this asset and with any push_back into it.
    [[nodiscard]] cc::vector<sv::mesh const*> meshes_with_material(cc::string_view name) const;

    /// The id minted for the material the file called `name`, or `material_id::invalid` when it has no such material.
    [[nodiscard]] material_id material(cc::string_view name) const;

    /// Points every mesh drawn by the file's `name` material at `replacement` instead.
    ///
    /// This is what "overriding a material" means once the file is loaded: the slot keeps its name, and everything bound
    /// to it moves together.
    /// Returns how many meshes were rewritten, so a caller can tell a typo from a material nothing used.
    isize override_material(cc::string_view name, material_id replacement);

    /// The world-space box around every mesh, empty when the asset places none.
    /// What a caller frames a camera on, and the reason `mesh` keeps its geometry rather than only an id.
    [[nodiscard]] cc::optional<tg::aabb3f> bounds() const;
};
