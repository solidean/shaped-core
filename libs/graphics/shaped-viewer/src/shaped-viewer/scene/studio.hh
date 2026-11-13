#pragma once

#include <clean-core/error/optional.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/scene/background.hh>
#include <shaped-viewer/scene/mesh.hh>
#include <shaped-viewer/view/camera.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>

/// Studio mode: single-asset renders lit as Philip's studio spec describes.
///
/// The pieces are public and compose: the dome `studio_dome`, the exposure `studio_exposure` derives for it, the floor `studio_floor`, and the framing `studio_camera`.
/// `scene_ref::studio` sets all of them on a scene layer at once.
/// libs/graphics/shaped-viewer/docs/issues/feat-sv-studio-mode.md has the design and the spec it follows.

/// Which of the spec's two setups.
enum class sv::studio_setup : sv::u8
{
    /// An asset standing on an infinite diffuse floor at y = 0, the key 45 degrees above the horizon.
    floor,

    /// A hanging asset: no floor, a dark zenith, the rise starting at the horizon, and the key 25 degrees below it.
    ceiling,
};

/// The six views of the spec's standard sheet; each turns the dome with it.
///
/// The spec names them and gives no angles, so these are ours: the side views circle the asset at 10 degrees above its center,
/// `front` looking along +z, and `top` straight down with +z at the top of the image.
enum class sv::studio_view : sv::u8
{
    front,
    front_right,
    right,
    back,
    front_left,
    top,
};

/// What `scene_ref::studio` sets up; the defaults are the spec's values.
struct sv::studio_options
{
    studio_setup setup = studio_setup::floor;

    /// H, the dome's radiance at the horizon, which every other radiance of the dome is a multiple of.
    f32 horizon = 1.0f;

    /// Unset, the lobes turn with the view's camera every frame; set, they stay where this camera sees them.
    cc::optional<sv::camera> lights_from;

    /// In stops; unset derives it from the dome, `studio_exposure`.
    cc::optional<f32> exposure;

    /// Whether the floor setup draws its floor; the ceiling setup never does.
    bool draw_floor = true;

    /// Where the floor lies, and how far it reaches from the origin in x and z.
    f32 floor_height = 0.0f;
    f32 floor_half_extent = 1000.0f;
};

namespace sv
{
/// The floor's albedo, and the card the exposure is calibrated against.
inline constexpr f32 studio_floor_albedo = 0.35f;
inline constexpr f32 studio_card_albedo = 0.19f;

/// The spec's dome for `setup` at horizon radiance `horizon`, its lobes following the camera.
///
/// The gradient is H at and below 25 degrees (the ceiling: at and below the horizon), rising by a smoothstep to 1.6 H straight up (the ceiling: 0.04 H).
/// The key is 40 H, 50 degrees to the camera's left and 45 degrees up (the ceiling: 25 degrees down).
/// The fill is 8 H, 75 degrees right and 18 up; the rim is 10 H, behind the asset and 55 up.
/// A camera ray that misses sees the gradient alone at 2.4 times.
[[nodiscard]] dome studio_dome(studio_setup setup = studio_setup::floor, f32 horizon = 1.0f);

/// The exposure in stops under which a diffuse card of `studio_card_albedo`, square to the key, reads sRGB 122 under either curve.
///
/// Derived from the dome alone, as if nothing but the dome surrounded the card: a floor below brightens that half of its view, and is not counted.
[[nodiscard]] f32 studio_exposure(dome const& d);

/// The floor: a square at `height`, `half_extent` from the origin in x and z, facing up.
/// A grid whose lines halve toward the origin, so the triangles under an asset are small: a hit is only as precise as its triangle's corners.
/// Openpbr, `base_color` `studio_floor_albedo` and `specular_weight` 0, so fully diffuse.
[[nodiscard]] mesh studio_floor(f32 height = 0.0f, f32 half_extent = 1000.0f);

/// A camera framing `bounds` from `view`: 50 mm on a 36 mm-wide sensor, at the least distance every corner of the box is in an image of `aspect` (width over height), with a tenth to spare.
/// `front_azimuth` turns every view about +y, for an asset whose front does not face -z: the orbit azimuth `front` is seen from.
/// The near plane is set from the bounds, and the projection's aspect to `aspect`.
[[nodiscard]] camera studio_camera(tg::aabb3f const& bounds, studio_view view, f64 aspect, tg::angle_d front_azimuth = {});
} // namespace sv
