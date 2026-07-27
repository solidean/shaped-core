#pragma once

#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/vec.hh>

namespace sv
{
/// Light types a view holds.
///
/// A view keeps one typed list per kind (view::area_lights, and more as they land), rather than a single tagged
/// list — so a consumer iterates exactly the type it handles without switching on a tag or partitioning by kind.
/// This is deliberately explicit, close-to-the-renderer API; the ergonomic scene-building layer sits on top of it.

/// A rectangular area light the path tracer samples for direct lighting (next-event estimation).
/// One shadow ray per bounce is aimed at the rect, so it lights the scene even when no emissive geometry is in view.
///
/// The rectangle is defined once in a canonical local frame and placed by `transform`:
/// it lies in the local xy plane centered at the origin, half-sized by `half_extents` along local x and y, and
/// its emitting face looks along local +z. `transform` positions and orients it in the world (rotation, scale
/// and translation all apply). The view_renderer derives the world-space rect the integrator needs — center,
/// two half-edge vectors, and the outward normal — from this.
///
/// The default is a downward-facing rect three units overhead, a sensible key light for a scene near the origin.
struct area_light
{
    tg::vec2f half_extents = tg::vec2f(0.75f, 0.75f);

    // Columns are the local basis images + translation: local x -> world x, local y -> world +z,
    // local z (the emitting normal) -> world -y (down), translated three units up.
    tg::mat4f transform = tg::mat4f::make_from_cols(tg::vec4f(1, 0, 0, 0),
                                                    tg::vec4f(0, 0, 1, 0),
                                                    tg::vec4f(0, -1, 0, 0),
                                                    tg::vec4f(0, 3, 0, 1));

    tg::vec3f emission = tg::vec3f(12.0f, 12.0f, 12.0f);
};
} // namespace sv
