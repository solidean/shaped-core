#include <clean-core/common/utility.hh> // cc::clamp
#include <shaped-rendering/input.hh>
#include <shaped-viewer/view/camera_controller.hh>
#include <typed-geometry/linalg/cross.hh>  // tg::cross + tg::dual
#include <typed-geometry/scalar/scalar.hh> // tg::asin, tg::atan2, tg::sin_cos, tg::pow

namespace sv
{
camera orbit_state::to_camera() const
{
    auto cam = camera::orbiting(target, distance, azimuth, elevation);
    cam.projection.vertical_fov = vertical_fov;
    return cam;
}

orbit_state orbit_state::from_camera(camera const& cam, f64 distance)
{
    // Put the target `distance` down the view direction, then invert camera::orbiting on the resulting offset:
    // its y is distance * sin(elevation), and its x / -z carry the azimuth.
    auto const target = cam.position + cam.basis().forward * distance;
    auto const offset = cam.position - target;

    return {.target = target,
            .distance = distance,
            .azimuth = tg::atan2(offset[0], -offset[2]),
            .elevation = tg::asin(cc::clamp(offset[1] / distance, -1.0, 1.0)),
            .vertical_fov = cam.projection.vertical_fov};
}

bool orbit_camera_controller::handle(sr::input_event const& e)
{
    return e.payload.visit(
        [&](sr::mouse_button_event const& b)
        {
            if (b.button == sr::mouse_button::left)
                _orbiting = b.is_down;
            else if (b.button == sr::mouse_button::middle)
                _panning = b.is_down;
            return false; // a button press aims the drag; it moves nothing on its own
        },
        [&](sr::mouse_move_event const& m)
        {
            // Motion with no button held is just the cursor passing over the view.
            if (!_orbiting && !_panning)
                return false;
            if (m.delta[0] == 0.0f && m.delta[1] == 0.0f)
                return false;

            auto const dx = f64(m.delta[0]);
            auto const dy = f64(m.delta[1]); // screen y grows downward

            if (_orbiting)
            {
                orbit.azimuth += tg::angle_d::make_from_degree(dx * config.orbit_degrees_per_pixel);
                // Dragging down lowers the eye, so the scene tips the way the cursor goes.
                orbit.elevation -= tg::angle_d::make_from_degree(dy * config.orbit_degrees_per_pixel);

                // The view direction must never align with +y, which is what camera::orbiting needs and the clamp gives.
                auto const limit = config.max_elevation.degree();
                orbit.elevation = tg::angle_d::make_from_degree(cc::clamp(orbit.elevation.degree(), -limit, limit));
            }
            else
            {
                // Pan across the plane the camera faces, so the target follows the cursor rather than the world axes.
                // Scaled by distance, so one drag covers the same fraction of the screen at every zoom.
                auto const b = orbit.to_camera().basis();
                auto const k = config.pan_units_per_pixel * orbit.distance;
                orbit.target += b.right * (-dx * k) + b.up * (dy * k);
            }
            return true;
        },
        [&](sr::mouse_wheel_event const& w)
        {
            if (w.delta[1] == 0.0f)
                return false;

            // Scrolling away from the user moves the eye in, geometrically rather than linearly, so every tick feels equal.
            auto const scaled = orbit.distance * tg::pow(config.zoom_per_wheel_tick, -f64(w.delta[1]));
            auto const clamped = cc::clamp(scaled, config.min_distance, config.max_distance);
            if (clamped == orbit.distance)
                return false;
            orbit.distance = clamped;
            return true;
        },
        [](sr::key_event const&) { return false; }, // keys and text belong to the fps controller, or to the caller
        [](sr::text_event const&) { return false; });
}

tg::vec3d fps_state::forward() const
{
    auto const [sy, cy] = tg::sin_cos(yaw);
    auto const [sp, cp] = tg::sin_cos(pitch);
    return tg::vec3d(sy * cp, sp, cy * cp);
}

camera fps_state::to_camera() const
{
    auto cam = camera::looking_at(position, position + forward());
    cam.projection.vertical_fov = vertical_fov;
    return cam;
}

fps_state fps_state::from_camera(camera const& cam)
{
    // Invert fps_state::forward on the view direction: its y is sin(pitch), and its x / z carry the yaw.
    auto const f = cam.basis().forward;

    return {.position = cam.position,
            .yaw = tg::atan2(f[0], f[2]),
            .pitch = tg::asin(cc::clamp(f[1], -1.0, 1.0)),
            .vertical_fov = cam.projection.vertical_fov};
}

bool fps_camera_controller::is_moving() const
{
    return _key_forward || _key_back || _key_left || _key_right || _key_up || _key_down;
}

void fps_camera_controller::release_input()
{
    _looking = false;
    _key_forward = false;
    _key_back = false;
    _key_left = false;
    _key_right = false;
    _key_up = false;
    _key_down = false;
    _key_fast = false;
    _key_slow = false;
}

bool fps_camera_controller::handle(sr::input_event const& e)
{
    return e.payload.visit(
        [&](sr::key_event const& k)
        {
            // Positions, not characters, so WASD stays the same physical cluster on AZERTY.
            switch (k.scancode)
            {
            case sr::scancode::w:
                _key_forward = k.is_down;
                break;
            case sr::scancode::s:
                _key_back = k.is_down;
                break;
            case sr::scancode::a:
                _key_left = k.is_down;
                break;
            case sr::scancode::d:
                _key_right = k.is_down;
                break;
            case sr::scancode::e:
                _key_up = k.is_down;
                break;
            case sr::scancode::q:
                _key_down = k.is_down;
                break;
            case sr::scancode::left_shift:
            case sr::scancode::right_shift:
                _key_fast = k.is_down;
                break;
            case sr::scancode::left_ctrl:
            case sr::scancode::right_ctrl:
                _key_slow = k.is_down;
                break;
            default:
                break;
            }
            return false; // a key only arms update(dt); nothing has moved yet
        },
        [&](sr::mouse_button_event const& b)
        {
            if (b.button == sr::mouse_button::right)
                _looking = b.is_down;
            return false; // a button press aims the look; it turns nothing on its own
        },
        [&](sr::mouse_move_event const& m)
        {
            if (config.look_requires_button && !_looking)
                return false;
            if (m.delta[0] == 0.0f && m.delta[1] == 0.0f)
                return false;

            pose.yaw += tg::angle_d::make_from_degree(f64(m.delta[0]) * config.look_degrees_per_pixel);
            // Screen y grows downward, so moving the mouse down looks down.
            pose.pitch -= tg::angle_d::make_from_degree(f64(m.delta[1]) * config.look_degrees_per_pixel);

            // The view direction must never align with +y, or the right axis is undefined and the horizon flips.
            auto const limit = config.max_pitch.degree();
            pose.pitch = tg::angle_d::make_from_degree(cc::clamp(pose.pitch.degree(), -limit, limit));
            return true;
        },
        [&](sr::mouse_wheel_event const& w)
        {
            if (w.delta[1] == 0.0f)
                return false;

            // The wheel is the throttle here, not a zoom: geometrically, so one tick feels the same at every speed.
            auto const scaled = config.move_units_per_second * tg::pow(config.speed_per_wheel_tick, f64(w.delta[1]));
            config.move_units_per_second
                = cc::clamp(scaled, config.min_move_units_per_second, config.max_move_units_per_second);
            return false; // retuning the speed moves nothing until the next update
        },
        [](sr::text_event const&) { return false; }); // text belongs to the caller
}

bool fps_camera_controller::update(f64 dt)
{
    if (dt <= 0 || !is_moving())
        return false;

    auto const f = pose.forward();
    // Level with the horizon by construction, since it comes from world up rather than from the camera's own up.
    auto const right = tg::dual(tg::cross(tg::vec3d(0, 1, 0), f)).normalized();

    auto direction = tg::vec3d::zero;
    if (_key_forward)
        direction += f;
    if (_key_back)
        direction -= f;
    if (_key_right)
        direction += right;
    if (_key_left)
        direction -= right;
    if (_key_up)
        direction += tg::vec3d(0, 1, 0);
    if (_key_down)
        direction -= tg::vec3d(0, 1, 0);

    // Opposing keys cancel exactly, and a diagonal must not outrun a straight line — hence normalizing rather than summing.
    auto const length = direction.length();
    if (length <= 0)
        return false;

    auto speed = config.move_units_per_second;
    if (_key_fast)
        speed *= config.fast_multiplier;
    if (_key_slow)
        speed *= config.slow_multiplier;

    pose.position += direction * (speed * dt / length);
    return true;
}
} // namespace sv
