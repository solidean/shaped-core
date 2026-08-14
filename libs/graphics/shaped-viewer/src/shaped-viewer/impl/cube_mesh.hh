#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/scene/pbr_material.hh>
#include <typed-geometry/linalg/pos.hh>

namespace sv::impl
{
/// A cube as a non-indexed triangle list (36 vertices, 12 triangles) with one PBR material per triangle and a distinct color per face.
/// The placeholder geometry an immediate-mode view shows until real geometry authoring lands.
/// Positions and materials are kept in lockstep: positions[3*t .. 3*t+2] is triangle t, and materials[t] shades it.
struct cube_mesh
{
    cc::vector<tg::pos3f> positions;
    cc::vector<pbr_material> materials;
};

/// Builds a cube centered at the origin spanning [-half_extent, half_extent] on each axis.
[[nodiscard]] cube_mesh make_cube(float half_extent = 1.0f);
} // namespace sv::impl
