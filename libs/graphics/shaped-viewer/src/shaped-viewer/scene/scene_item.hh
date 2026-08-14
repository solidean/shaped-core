#pragma once

#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/resources/resource_ids.hh>
#include <typed-geometry/transform/transform.hh>

/// What kind of thing a scene item is.
/// Exactly one kind exists today; the tag is here so more kinds (point clouds, procedural primitives, volumes, …) slot in without every consumer switching on a variant yet.
/// Lights are *not* items — a view holds them in its own typed lists (see view.hh / light.hh).
enum class sv::scene_item_kind : sv::u8
{
    triangle_mesh,
};

/// One concrete object in a view: a mesh placed into the world with a transform, shaded by a material set.
///
/// It names its resources by id — `mesh` (geometry + BLAS) and `materials` (one PBR material per triangle, indexed by `PrimitiveIndex()` in the closest-hit).
/// The view_renderer resolves both through the managers.
///
/// `transform` is affine because a placement may scale or shear; build one from tg's factories
/// (`make_rotation`, `make_translation`, `make_from_linear_mat`, …) and `tg::compose` them.
/// The renderer reads its linear part and translation straight into the row-major 3x4 the TLAS instance wants.
struct sv::scene_item
{
    scene_item_kind kind = scene_item_kind::triangle_mesh;

    mesh_id mesh = mesh_id::invalid;
    material_set_id materials = material_set_id::invalid;

    tg::affine_transform3f transform = {};
};
