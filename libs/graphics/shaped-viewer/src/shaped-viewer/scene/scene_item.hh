#pragma once

#include <clean-core/bytes/hash128.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/scene/triangle_geometry.hh>
#include <typed-geometry/transform/transform.hh>

/// What kind of thing a scene item is.
/// More kinds (point clouds, volumes, …) slot in the same way, without every consumer switching on a variant.
/// Lights are *not* items — a view holds them in its own typed lists (see view.hh / light.hh).
enum class sv::scene_item_kind : sv::u8
{
    triangle_mesh,

    /// A batch of analytic quadrics, traced through a procedural BLAS — see libs/graphics/shaped-viewer/docs/quadrics.md.
    quadric_set,
};

/// How long a placed mesh's geometry lives on the GPU.
enum class sv::mesh_lifetime : sv::u8
{
    /// Held by the mesh manager under its content key, streamed in on the copy queue, and drawn as a placeholder box until it lands.
    /// What a mesh placed again and again unchanged wants: every later placement is a lookup.
    resident,

    /// Uploaded inline on the frame's own command list into transient buffers, its BLAS built there too, and gone with the epoch.
    /// What geometry that changes every frame wants, a simulation's above all: it draws in the frame it was placed, never as a placeholder.
    /// Topology may change from one frame to the next at no extra cost.
    /// It suits small meshes, since every frame pays the upload and the BLAS build; attributes and textures stay resident and stream as usual.
    /// It lights nothing: an emissive material on it shades the mesh itself, but no light record samples it.
    frame,
};

/// One concrete object in a view: a mesh placed into the world with a transform, shaded by one material permutation.
///
/// It names its resources by id — `mesh` (geometry + BLAS) and `instance` (the material parameter block a generated
/// closest-hit reads through `InstanceID()`).
/// `shader_key` names the generated permutation that block is read by, which is what decides the hit group the TLAS instance
/// selects.
/// It is `material_shader_key`'s, NOT `resolved_material::permutation_key` — the resolution's shape plus how the cache spells
/// it — so the two are deliberately not spelled alike.
/// `gpu_resource_manager::acquire_scene_item` is what fills all three; nothing else should mint one by hand, since the
/// three have to come from one resolution.
/// There is one overload per geometry kind, and each sets `kind` along with the id that kind uses.
///
/// `transform` is affine because a placement may scale or shear; build one from tg's factories
/// (`make_rotation`, `make_translation`, `make_from_linear_mat`, …) and `tg::compose` them.
/// The renderer reads its linear part and translation straight into the row-major 3x4 the TLAS instance wants.
struct sv::scene_item
{
    scene_item_kind kind = scene_item_kind::triangle_mesh;

    /// live at `triangle_mesh`
    mesh_id mesh = mesh_id::invalid;

    /// live at `quadric_set`
    quadric_set_id quadrics = quadric_set_id::invalid;

    instance_id instance = instance_id::invalid;

    cc::hash128 shader_key;

    /// At `triangle_mesh`, whether the geometry is `mesh`'s or `frame_geometry`, uploaded by the renderer this frame.
    mesh_lifetime lifetime = mesh_lifetime::resident;

    /// live at `triangle_mesh` with `mesh_lifetime::frame`, in place of `mesh`
    triangle_geometry frame_geometry;

    tg::affine_transform3f transform = {};
};
