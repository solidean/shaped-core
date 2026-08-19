#pragma once

#include <shaped-rendering/fwd.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/view/camera.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/angle.hh>

/// Where an orbiting camera is, in the parameters a user actually manipulates.
///
/// This is the form a view's camera persists in across frames: an `sv::camera` alone cannot be orbited without
/// re-deriving a target and a distance from it every time, and those two are exactly what a drag changes.
/// `camera::orbiting` is the map to a camera, so the two never disagree.
struct sv::orbit_state
{
    /// What the camera looks at, and what a pan moves.
    tg::pos3d target = tg::pos3d::zero;

    f64 distance = 4.5;

    /// Around +y; 0 puts the eye on the -z side looking along +z, matching the default camera pose.
    tg::angle_d azimuth = tg::angle_d::make_from_degree(0);

    /// Above the horizon, kept inside (-90, 90) so the view direction never aligns with +y.
    tg::angle_d elevation = tg::angle_d::make_from_degree(20);

    /// Carried here so a controller-driven camera keeps the projection a caller seeded it with.
    tg::angle_d vertical_fov = tg::angle_d::make_from_degree(60);

    [[nodiscard]] sv::camera to_camera() const;

    /// The orbit that best reproduces `cam`, keeping this state's `distance` — the target is placed that far down the view direction.
    /// Lossy by nature: a camera carries no orbit centre, so a caller that has one should set the fields directly instead.
    [[nodiscard]] static orbit_state from_camera(camera const& cam, f64 distance = 4.5);
};

/// Sensitivities and limits for orbit_camera_controller.
struct sv::camera_controller_config
{
    /// Degrees of azimuth / elevation per pixel of drag.
    f64 orbit_degrees_per_pixel = 0.4;

    /// Target motion per pixel of drag, scaled by `distance` so a pan feels the same at every zoom.
    f64 pan_units_per_pixel = 0.0025;

    /// Distance multiplier per wheel tick.
    f64 zoom_per_wheel_tick = 1.1;

    f64 min_distance = 0.05;
    f64 max_distance = 1e4;

    /// How far the eye may be lifted; must stay under 90 degrees.
    tg::angle_d max_elevation = tg::angle_d::make_from_degree(89.0);
};

/// The built-in orbit controller: it consumes `sr::input_event`s already routed to one view and drives an `orbit_state`.
///
/// Event-driven and time-free — every input carries the delta it caused, so there is no `update(dt)` to schedule and no frame the controller has to be called on.
/// A view that gets no input produces no motion by construction.
///
/// Left drag orbits, middle drag pans the target, the wheel changes distance.
/// The keyboard is deliberately untouched: a WASD camera integrates over time, which is fps_camera_controller's job.
class sv::orbit_camera_controller
{
public:
    orbit_state orbit = {};
    camera_controller_config config = {};

    /// Apply one event, and report whether it actually moved the camera.
    /// Events for other views must be filtered out before this — the controller has no idea which view it belongs to.
    bool handle(sr::input_event const& e);

    [[nodiscard]] sv::camera camera() const { return orbit.to_camera(); }

    /// Whether a drag is in progress, so a caller can keep routing motion to this view while the button is held.
    [[nodiscard]] bool is_dragging() const { return _orbiting || _panning; }

private:
    bool _orbiting = false;
    bool _panning = false;
};

/// Where a free-look camera is, in the parameters a user actually manipulates.
///
/// Yaw and pitch rather than the camera's quaternion, because that is what keeps the horizon level:
/// accumulating a drag into an orientation lets roll creep in, and no amount of re-normalizing takes it back out.
/// `to_camera` is the map to a camera, so the two never disagree.
struct sv::fps_state
{
    tg::pos3d position = tg::pos3d(0, 0, -4.5);

    /// Around +y; 0 looks along +z, matching orbit_state's azimuth 0 and the default camera pose.
    tg::angle_d yaw = tg::angle_d::make_from_degree(0);

    /// Above the horizon, kept inside (-90, 90) so the view direction never aligns with +y.
    tg::angle_d pitch = tg::angle_d::make_from_degree(0);

    /// Carried here so a controller-driven camera keeps the projection a caller seeded it with.
    tg::angle_d vertical_fov = tg::angle_d::make_from_degree(60);

    /// The unit view direction this yaw / pitch pair looks along.
    [[nodiscard]] tg::vec3d forward() const;

    [[nodiscard]] sv::camera to_camera() const;

    /// The pose of `cam`, exactly — a position and a view direction are all an fps_state holds, so nothing is lost.
    /// Any roll in `cam.orientation` is dropped, which is the point of going through yaw / pitch at all.
    [[nodiscard]] static fps_state from_camera(camera const& cam);
};

/// Sensitivities and limits for fps_camera_controller.
struct sv::fps_camera_controller_config
{
    /// Degrees of yaw / pitch per pixel of mouse motion while looking.
    f64 look_degrees_per_pixel = 0.2;

    /// The base speed a held movement key travels at, in units per second.
    /// Live rather than fixed: the wheel retunes it, the way a fly camera's throttle works everywhere.
    f64 move_units_per_second = 2.0;

    /// Speed factor while shift is held.
    f64 fast_multiplier = 3.0;

    /// Speed factor while ctrl is held; both together multiply.
    f64 slow_multiplier = 0.25;

    /// Speed multiplier per wheel tick, applied to `move_units_per_second`.
    f64 speed_per_wheel_tick = 1.2;

    f64 min_move_units_per_second = 0.01;
    f64 max_move_units_per_second = 1e4;

    /// How far the view may be tilted; must stay under 90 degrees.
    tg::angle_d max_pitch = tg::angle_d::make_from_degree(89.0);

    /// Whether looking needs the right mouse button held.
    /// Set false once the caller has captured the cursor (`sr::window::set_relative_mouse_mode`), so every motion turns the view.
    bool look_requires_button = true;
};

/// The built-in first-person controller: mouse-look plus WASD, consuming `sr::input_event`s already routed to one view.
///
/// Free-flying — no gravity, no collision, and forward follows the pitch — so W into a raised view climbs.
/// Unlike orbit_camera_controller this one integrates over time: a held key means motion on every frame rather than on
/// every event, so it takes both halves — `handle` for the events and `update(dt)` once per frame for what they imply.
///
/// Right-drag looks (see `config.look_requires_button`), W/A/S/D move along the view, E/Q rise and fall along world +y,
/// shift accelerates, ctrl slows, and the wheel retunes the base speed instead of moving the camera.
class sv::fps_camera_controller
{
public:
    fps_state pose = {};
    fps_camera_controller_config config = {};

    /// Apply one event, and report whether it moved the camera on its own — which only a look does.
    /// A movement key merely arms `update`, so it reports nothing; the motion it causes happens there.
    /// Events for other views must be filtered out before this — the controller has no idea which view it belongs to.
    bool handle(sr::input_event const& e);

    /// Integrate the held movement keys over `dt` seconds, and report whether the camera actually moved.
    /// Call this every frame, not only on frames that saw an event: a key held across a quiet frame still moves the camera.
    bool update(f64 dt);

    [[nodiscard]] sv::camera camera() const { return pose.to_camera(); }

    /// Whether a mouse-look is in progress, so a caller can capture the cursor and keep routing motion to this view.
    [[nodiscard]] bool is_looking() const { return _looking; }

    /// Whether a movement key is held, i.e. whether `update` would move the camera.
    [[nodiscard]] bool is_moving() const;

    /// Forget every held key and the look, so a view that lost focus does not keep flying on a key it will never see released.
    void release_input();

private:
    bool _looking = false;
    bool _key_forward = false;
    bool _key_back = false;
    bool _key_left = false;
    bool _key_right = false;
    bool _key_up = false;
    bool _key_down = false;
    bool _key_fast = false;
    bool _key_slow = false;
};
