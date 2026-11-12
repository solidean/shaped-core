#pragma once

#include <clean-core/error/optional.hh>
#include <sgl_modules/tracer.hh> // sv::shaders::tracer::camera_record
#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/angle.hh>

/// A camera's two matrices, in `tg`'s convention — column-major, and a vector is a column.
///
/// sv traces rays rather than rasterizing, so nothing in the viewer needs these; they exist for a denoiser that
/// reprojects in world space, which is what `sr::reconstruct_guides` asks for.
struct sv::camera_matrices
{
    tg::mat4f world_to_view = tg::mat4f::identity;
    tg::mat4f view_to_clip = tg::mat4f::identity;
};

namespace sv
{
/// `cam`'s basis as the tracer reads it, module `tracer`'s `camera_record`, its pads zero.
/// A pinhole's `right_scaled` and `up_scaled` carry the aspect and the field of view baked in, `right * aspect * tan(fov_y / 2)` and
/// `up * tan(fov_y / 2)`, so the raygen forms `forward + right_scaled * ndc.x - up_scaled * ndc.y`.
/// An orthographic camera's carry half its image's width and height in meters, and the raygen moves the origin by them instead.
/// The aspect ratio is taken from `cam.projection.aspect_ratio`.
[[nodiscard]] shaders::tracer::camera_record camera_record_of(camera const& cam);

/// The matrices `cam`'s basis amounts to: a pinhole's with an infinite far plane, an orthographic camera's up to its far plane.
///
/// Derived from the SAME basis `camera_ray_offset` and `camera_project` form their rays from, so all three describe
/// one camera: `right_scaled` and `up_scaled` carry `tan(fov / 2)` in their lengths, or the half extents, which is the projection's
/// diagonal, and `forward` is its third view axis.
/// Clip depth `z / w` is `scene.camera_primary_depth`: `1 - near / depth` for a pinhole, linear from near to far for an orthographic camera.
/// A camera whose basis is degenerate — a zero `right_scaled` or `forward` — yields identities rather than NaNs.
[[nodiscard]] camera_matrices matrices_of(shaders::tracer::camera_record const& cam, f32 near_plane);

/// The world point a traced pixel's primary-hit depth names, or none where the primary ray escaped.
///
/// The inverse of the tracer's `primary_depth`, which is `1 - near_plane / view_depth` along the ray through the center of `pixel` in a `resolution`-sized image.
/// Good enough for a pick, not for geometry: a single-precision depth keeps fewer bits of `1 - depth` the further the hit lies from the near plane.
[[nodiscard]] cc::optional<tg::pos3d> primary_hit_position(shaders::tracer::camera_record const& cam,
                                                           tg::vec2i pixel,
                                                           tg::vec2i resolution,
                                                           f32 depth,
                                                           f32 near_plane);
} // namespace sv

/// How a camera's rays leave it.
enum class sv::projection_kind : sv::u8
{
    /// A pinhole at the camera's position: every ray leaves it, spread over `vertical_fov`.
    perspective,

    /// Every ray runs along the view direction, from its own point of an image plane `view_height` high through the camera's position.
    /// Parallel lines stay parallel and a thing's size does not change with its distance, as a technical view and a bake want.
    orthographic,
};

/// A camera's projection: its kind, the extent of its image, its aspect ratio (width / height), and its depth range.
///
/// `aspect_ratio` is a property of the projection, not of the render target — set it from the target size before baking the GPU basis (the view renderer does this).
struct sv::camera_projection
{
    projection_kind kind = projection_kind::perspective;

    /// A perspective camera's vertical field of view.
    tg::angle_d vertical_fov = tg::angle_d::make_from_degree(60.0);

    /// An orthographic camera's image height, in meters.
    f64 view_height = 2.0;

    f64 aspect_ratio = 1.0;
    f64 near_plane = 0.01;

    /// Where an orthographic camera's raster depth ends, in meters along the view; a perspective camera's has no end.
    /// Nothing past it is drawn by raster, and a traced hit past it writes the far depth, 1.
    f64 far_plane = 1000.0;
};

/// A camera's axes in world space — where the camera frame's +x, +y and +z point.
/// Left-handed, so `forward` points into the scene.
struct sv::camera_basis
{
    tg::vec3d right;
    tg::vec3d up;
    tg::vec3d forward;
};

/// A dev-friendly pinhole camera: a double-precision pose (position + orientation) plus a projection.
///
/// `orientation` is a unit quaternion mapping the base frame to the camera frame — it sends +x to right,
/// +y to up, +z to forward (left-handed, forward points into the scene, matching the raygen). Build one from
/// a look-at with `look_rotation` / `look_at`; the default frames the origin from `position`.
/// The GPU basis is
/// baked by `shaders::tracer::camera_record::from`, taking the aspect ratio from `projection`.
struct sv::camera
{
    tg::pos3d position = tg::pos3d(2.2, 1.8, -3.2);
    tg::quat_d orientation = look_rotation(position, tg::pos3d::zero);
    camera_projection projection = {};

    /// A camera sitting at `eye` and looking at `target`, `up` fixing the roll; the projection stays default.
    [[nodiscard]] static camera looking_at(tg::pos3d eye, tg::pos3d target, tg::vec3d up = tg::vec3d(0, 1, 0))
    {
        return {.position = eye, .orientation = look_rotation(eye, target, up)};
    }

    /// A camera orbiting `target` at `distance`, looking inward; the projection stays default.
    ///
    /// `azimuth` orbits around +y (0 puts the eye on the -z side, looking along +z into the scene, matching the
    /// default pose); `elevation` lifts the eye above the horizon and must stay within (-90, 90) degrees so the
    /// view direction never aligns with +y.
    [[nodiscard]] static camera orbiting(tg::pos3d target, f64 distance, tg::angle_d azimuth, tg::angle_d elevation);

    /// The unit quaternion looking from `eye` toward `target`, with `up` fixing the roll (left-handed:
    /// forward = normalize(target - eye)). `up` must not be parallel to the view direction.
    [[nodiscard]] static tg::quat_d look_rotation(tg::pos3d eye, tg::pos3d target, tg::vec3d up = tg::vec3d(0, 1, 0));

    /// Aims the camera at `target` from its current `position`, keeping `up` as the roll reference.
    void look_at(tg::pos3d target, tg::vec3d up = tg::vec3d(0, 1, 0))
    {
        orientation = look_rotation(position, target, up);
    }

    /// The world-space axes this camera looks along — what a screen-space drag has to be expressed in.
    [[nodiscard]] camera_basis basis() const;
};
