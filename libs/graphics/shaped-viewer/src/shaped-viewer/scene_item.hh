#pragma once

#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/resources/resource_ids.hh>
#include <typed-geometry/linalg/mat.hh>

namespace sv
{
/// What kind of thing a scene item is.
/// Exactly one kind exists today; the tag is here so more kinds (point clouds, procedural primitives, volumes, …) slot in without every consumer switching on a variant yet.
/// Lights are *not* items — a view holds them in its own typed lists (see view.hh / light.hh).
enum class scene_item_kind : u8
{
    triangle_mesh,
};

/// One concrete object in a view: a mesh placed into the world with a transform, shaded by a material set.
///
/// It names its resources by id — `mesh` (geometry + BLAS) and `materials` (one PBR material per triangle, indexed by `PrimitiveIndex()` in the closest-hit) — which the view_renderer resolves through the managers.
/// `transform` is a standard column-major `tg::mat4f`.
/// The renderer repacks it into the row-major 3x4 the TLAS instance wants.
struct scene_item
{
    scene_item_kind kind = scene_item_kind::triangle_mesh;

    mesh_id mesh = mesh_id::invalid;
    material_set_id materials = material_set_id::invalid;

    tg::mat4f transform = tg::mat4f::identity;
};
} // namespace sv
