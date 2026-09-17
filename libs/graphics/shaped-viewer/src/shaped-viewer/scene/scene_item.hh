#pragma once

#include <clean-core/bytes/hash128.hh>
#include <shaped-viewer/fwd.hh>
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

    tg::affine_transform3f transform = {};
};
