#include <nexus/test.hh>
#include <shaped-rendering/input.hh>
#include <shaped-viewer/view/camera_controller.hh>
#include <typed-geometry/linalg/pos_ops.hh> // tg::distance

using namespace cc::primitive_defines;

// CPU-only: an sr::input_event is a plain value with a null window, so the controller runs without a window system, a display or a device.
// That is the whole reason it consumes events rather than polling a window.

namespace
{
sr::input_event button(sr::mouse_button b, bool is_down)
{
    return {.payload = sr::mouse_button_event{.button = b, .is_down = is_down}};
}

sr::input_event motion(float dx, float dy)
{
    return {.payload = sr::mouse_move_event{.delta = tg::vec2f(dx, dy)}};
}

sr::input_event wheel(float ticks)
{
    return {.payload = sr::mouse_wheel_event{.delta = tg::vec2f(0, ticks)}};
}

sr::input_event key(sr::scancode s, bool is_down)
{
    return {.payload = sr::key_event{.scancode = s, .is_down = is_down}};
}

// typed-geometry carries no scalar abs yet, so this one stays local to the test.
[[nodiscard]] f64 abs_of(f64 v)
{
    return v < 0 ? -v : v;
}

constexpr f64 eps = 1e-9;
} // namespace

TEST("sv - orbit controller turns a drag into azimuth and elevation")
{
    auto c = sv::orbit_camera_controller{};
    c.orbit.azimuth = tg::angle_d::make_from_degree(0);
    c.orbit.elevation = tg::angle_d::make_from_degree(0);
    auto const step = c.config.orbit_degrees_per_pixel;

    // Motion before a button goes down is the cursor merely passing over the view.
    CHECK(!c.handle(motion(50, 50)));
    CHECK(c.orbit.azimuth.degree() == 0.0);

    CHECK(!c.handle(button(sr::mouse_button::left, true))); // the press itself moves nothing
    CHECK(c.is_dragging());

    CHECK(c.handle(motion(10, 0)));
    CHECK(abs_of(c.orbit.azimuth.degree() - 10 * step) < eps);

    // Dragging down lowers the eye.
    CHECK(c.handle(motion(0, 10)));
    CHECK(abs_of(c.orbit.elevation.degree() + 10 * step) < eps);

    (void)c.handle(button(sr::mouse_button::left, false));
    CHECK(!c.is_dragging());
    CHECK(!c.handle(motion(100, 100))); // the drag is over; nothing follows the cursor
}

TEST("sv - orbit controller keeps the eye off the pole")
{
    auto c = sv::orbit_camera_controller{};
    (void)c.handle(button(sr::mouse_button::left, true));

    // camera::orbiting needs the view direction never to align with +y, so elevation must stop short of 90.
    auto const limit = c.config.max_elevation.degree();
    (void)c.handle(motion(0, -100000));
    CHECK(abs_of(c.orbit.elevation.degree() - limit) < eps);

    (void)c.handle(motion(0, 200000));
    CHECK(abs_of(c.orbit.elevation.degree() + limit) < eps);
}

TEST("sv - orbit controller zooms geometrically and clamps the distance")
{
    auto c = sv::orbit_camera_controller{};
    c.orbit.distance = 10.0;

    // Scrolling away from the user moves the eye in.
    CHECK(c.handle(wheel(1)));
    CHECK(abs_of(c.orbit.distance - 10.0 / c.config.zoom_per_wheel_tick) < 1e-12);

    CHECK(c.handle(wheel(-1)));
    CHECK(abs_of(c.orbit.distance - 10.0) < 1e-12);

    // A scroll that changes nothing must report no motion, or it would restart the accumulation for free.
    CHECK(!c.handle(wheel(0)));

    for (auto i = 0; i < 500; ++i)
        (void)c.handle(wheel(1));
    CHECK(c.orbit.distance == c.config.min_distance);

    for (auto i = 0; i < 1000; ++i)
        (void)c.handle(wheel(-1));
    CHECK(c.orbit.distance == c.config.max_distance);
    CHECK(!c.handle(wheel(-1))); // already at the limit
}

TEST("sv - orbit controller pans the target across the view plane")
{
    auto c = sv::orbit_camera_controller{};
    c.orbit.elevation = tg::angle_d::make_from_degree(0); // eye on -z looking along +z, so right is +x and up is +y

    (void)c.handle(button(sr::mouse_button::middle, true));
    CHECK(c.is_dragging());
    CHECK(c.handle(motion(10, 0)));

    // The scene follows the cursor, so dragging right moves the target left.
    CHECK(c.orbit.target[0] < 0);
    CHECK(abs_of(c.orbit.target[1]) < eps);

    (void)c.handle(button(sr::mouse_button::middle, false));
    CHECK(!c.is_dragging());
}

TEST("sv - orbit controller ignores keys and unrelated events")
{
    auto c = sv::orbit_camera_controller{};
    // A key must not report motion: the caller reads that as "restart the accumulation".
    CHECK(!c.handle({.payload = sr::key_event{.scancode = sr::scancode::w, .is_down = true}}));
    CHECK(!c.handle({.payload = sr::text_event{.text = "w"}}));
    CHECK(!c.is_dragging());
}

TEST("sv - orbit_state and camera::orbiting agree")
{
    auto const o = sv::orbit_state{.target = tg::pos3d(1, 2, 3),
                                   .distance = 7.5,
                                   .azimuth = tg::angle_d::make_from_degree(35),
                                   .elevation = tg::angle_d::make_from_degree(20)};

    auto const from_orbit = o.to_camera();
    auto const direct = sv::camera::orbiting(o.target, o.distance, o.azimuth, o.elevation);
    CHECK(tg::distance(from_orbit.position, direct.position) < 1e-9);

    // The projection is the one thing to_camera carries that camera::orbiting does not.
    auto with_fov = o;
    with_fov.vertical_fov = tg::angle_d::make_from_degree(35);
    CHECK(with_fov.to_camera().projection.vertical_fov == with_fov.vertical_fov);

    // Round-tripping recovers the orbit, given the distance it was built at.
    auto const back = sv::orbit_state::from_camera(from_orbit, o.distance);
    CHECK(tg::distance(back.target, o.target) < 1e-9);
    CHECK(abs_of(back.azimuth.degree() - o.azimuth.degree()) < 1e-9);
    CHECK(abs_of(back.elevation.degree() - o.elevation.degree()) < 1e-9);
}

TEST("sv - fps controller turns a right-drag into yaw and pitch")
{
    auto c = sv::fps_camera_controller{};
    auto const step = c.config.look_degrees_per_pixel;

    // Motion before the button goes down is the cursor merely passing over the view.
    CHECK(!c.handle(motion(50, 50)));
    CHECK(c.pose.yaw.degree() == 0.0);

    CHECK(!c.handle(button(sr::mouse_button::right, true))); // the press itself turns nothing
    CHECK(c.is_looking());

    // Moving right turns right, and yaw +90 degrees looks along +x.
    CHECK(c.handle(motion(10, 0)));
    CHECK(abs_of(c.pose.yaw.degree() - 10 * step) < eps);

    // Moving down looks down.
    CHECK(c.handle(motion(0, 10)));
    CHECK(abs_of(c.pose.pitch.degree() + 10 * step) < eps);

    (void)c.handle(button(sr::mouse_button::right, false));
    CHECK(!c.is_looking());
    CHECK(!c.handle(motion(100, 100)));

    // A captured cursor has no button to hold, so the look follows every motion.
    c.config.look_requires_button = false;
    CHECK(c.handle(motion(10, 0)));
}

TEST("sv - fps controller keeps the view off the pole")
{
    auto c = sv::fps_camera_controller{};
    (void)c.handle(button(sr::mouse_button::right, true));

    // A view direction aligned with +y leaves the right axis undefined, so pitch must stop short of 90.
    auto const limit = c.config.max_pitch.degree();
    (void)c.handle(motion(0, -100000));
    CHECK(abs_of(c.pose.pitch.degree() - limit) < eps);

    (void)c.handle(motion(0, 200000));
    CHECK(abs_of(c.pose.pitch.degree() + limit) < eps);
}

TEST("sv - fps controller moves only while update integrates")
{
    auto c = sv::fps_camera_controller{};
    c.pose.position = tg::pos3d::zero;
    c.config.move_units_per_second = 2.0;

    // A key press is not motion; it arms update, which is where the camera actually travels.
    CHECK(!c.handle(key(sr::scancode::w, true)));
    CHECK(c.is_moving());
    CHECK(tg::distance(c.pose.position, tg::pos3d::zero) < eps);

    // yaw 0 looks along +z, so a second of forward covers the base speed along +z.
    CHECK(c.update(1.0));
    CHECK(tg::distance(c.pose.position, tg::pos3d(0, 0, 2)) < eps);

    // Half the time is half the distance.
    CHECK(c.update(0.5));
    CHECK(tg::distance(c.pose.position, tg::pos3d(0, 0, 3)) < eps);

    // Shift accelerates, ctrl slows, and holding both multiplies.
    (void)c.handle(key(sr::scancode::left_shift, true));
    CHECK(c.update(1.0));
    CHECK(tg::distance(c.pose.position, tg::pos3d(0, 0, 3 + 2 * c.config.fast_multiplier)) < eps);
    (void)c.handle(key(sr::scancode::left_shift, false));

    (void)c.handle(key(sr::scancode::w, false));
    CHECK(!c.is_moving());
    CHECK(!c.update(1.0)); // nothing is held; a frame passing must not move the camera

    // A frame that took no time cannot move anything either.
    (void)c.handle(key(sr::scancode::w, true));
    CHECK(!c.update(0.0));
}

TEST("sv - fps controller normalizes its movement direction")
{
    auto c = sv::fps_camera_controller{};
    c.pose.position = tg::pos3d::zero;
    c.config.move_units_per_second = 2.0;

    // Diagonal travel must not outrun a straight line, so forward + right still covers exactly the base speed.
    (void)c.handle(key(sr::scancode::w, true));
    (void)c.handle(key(sr::scancode::d, true));
    CHECK(c.update(1.0));
    CHECK(abs_of(tg::distance(c.pose.position, tg::pos3d::zero) - 2.0) < eps);
    CHECK(abs_of(c.pose.position[0] - c.pose.position[2]) < eps); // yaw 0: right is +x, forward is +z

    // Opposing keys cancel exactly rather than drifting or normalizing a zero vector.
    auto const before = c.pose.position;
    (void)c.handle(key(sr::scancode::d, false));
    (void)c.handle(key(sr::scancode::s, true));
    CHECK(c.is_moving());
    CHECK(!c.update(1.0));
    CHECK(tg::distance(c.pose.position, before) < eps);
}

TEST("sv - fps controller flies along its view direction")
{
    auto c = sv::fps_camera_controller{};
    c.pose.position = tg::pos3d::zero;
    c.pose.yaw = tg::angle_d::make_from_degree(90); // looking along +x
    c.config.move_units_per_second = 1.0;

    (void)c.handle(key(sr::scancode::w, true));
    CHECK(c.update(1.0));
    CHECK(tg::distance(c.pose.position, tg::pos3d(1, 0, 0)) < 1e-12);

    // E and Q are world-vertical, independent of where the view points.
    (void)c.handle(key(sr::scancode::w, false));
    (void)c.handle(key(sr::scancode::e, true));
    CHECK(c.update(1.0));
    CHECK(tg::distance(c.pose.position, tg::pos3d(1, 1, 0)) < 1e-12);

    // Pitched up, forward climbs — this is a free-fly camera, not a walking one.
    (void)c.handle(key(sr::scancode::e, false));
    (void)c.handle(key(sr::scancode::w, true));
    c.pose.pitch = tg::angle_d::make_from_degree(45);
    CHECK(c.update(1.0));
    CHECK(c.pose.position[1] > 1.5);
}

TEST("sv - fps controller wheel retunes the speed without moving")
{
    auto c = sv::fps_camera_controller{};
    c.config.move_units_per_second = 2.0;

    // The wheel is a throttle here: it must never report motion, or the caller restarts the accumulation for free.
    CHECK(!c.handle(wheel(1)));
    CHECK(abs_of(c.config.move_units_per_second - 2.0 * c.config.speed_per_wheel_tick) < 1e-12);

    CHECK(!c.handle(wheel(-1)));
    CHECK(abs_of(c.config.move_units_per_second - 2.0) < 1e-12);

    for (auto i = 0; i < 500; ++i)
        (void)c.handle(wheel(-1));
    CHECK(c.config.move_units_per_second == c.config.min_move_units_per_second);

    for (auto i = 0; i < 1000; ++i)
        (void)c.handle(wheel(1));
    CHECK(c.config.move_units_per_second == c.config.max_move_units_per_second);
}

TEST("sv - fps controller drops its held input on release")
{
    auto c = sv::fps_camera_controller{};
    (void)c.handle(key(sr::scancode::w, true));
    (void)c.handle(button(sr::mouse_button::right, true));
    CHECK(c.is_moving());
    CHECK(c.is_looking());

    // A view that loses focus never sees the key come up, and must not keep flying on it.
    c.release_input();
    CHECK(!c.is_moving());
    CHECK(!c.is_looking());
    CHECK(!c.update(1.0));
}

TEST("sv - fps_state and camera agree")
{
    auto const s = sv::fps_state{.position = tg::pos3d(1, 2, 3),
                                 .yaw = tg::angle_d::make_from_degree(35),
                                 .pitch = tg::angle_d::make_from_degree(20),
                                 .vertical_fov = tg::angle_d::make_from_degree(35)};

    auto const cam = s.to_camera();
    CHECK(tg::distance(cam.position, s.position) < 1e-12);
    CHECK(cam.projection.vertical_fov == s.vertical_fov);
    CHECK((cam.basis().forward - s.forward()).length() < 1e-12);

    // Unlike orbit_state::from_camera this recovers the pose exactly — there is no orbit centre to guess.
    auto const back = sv::fps_state::from_camera(cam);
    CHECK(tg::distance(back.position, s.position) < 1e-12);
    CHECK(abs_of(back.yaw.degree() - s.yaw.degree()) < 1e-9);
    CHECK(abs_of(back.pitch.degree() - s.pitch.degree()) < 1e-9);
    CHECK(back.vertical_fov == s.vertical_fov);
}
