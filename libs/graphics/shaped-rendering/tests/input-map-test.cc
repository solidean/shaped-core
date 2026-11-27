#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-rendering/input_map.hh>
#include <typed-geometry/scalar/scalar.hh>

// sr::input_map on synthetic events and pad states, which is all it reads: no window system, no display.

using namespace sr;

namespace
{
[[nodiscard]] bool near(float a, float b)
{
    return tg::abs(a - b) < 1e-5f;
}

[[nodiscard]] input_event key(scancode code, bool is_down, bool is_repeat = false)
{
    return {.payload = key_event{.scancode = code, .is_down = is_down, .is_repeat = is_repeat}};
}

[[nodiscard]] input_event pad_button(u32 pad, gamepad_button button, bool is_down)
{
    return {.payload = gamepad_button_event{.gamepad = pad, .button = button, .is_down = is_down}};
}

[[nodiscard]] input_event mouse_move(tg::vec2f delta)
{
    return {.payload = mouse_move_event{.delta = delta}};
}

/// A map, and an update that takes events one batch at a time, with pads and focus as given.
struct fixture
{
    input_map map;
    cc::vector<gamepad_state> pads;
    bool has_focus = true;

    void update(cc::span<input_event const> events = {}) { map.update(events, pads, has_focus); }
    void update(input_event const& e) { update(cc::span<input_event const>(&e, 1)); }
};
} // namespace

TEST("sr - input_map: a key bound to a button is held from its press to its release, with one edge each")
{
    auto f = fixture();
    auto const jump = f.map.add_action("jump", input_action_kind::button);
    f.map.bind(jump, {.source = key_binding{scancode::space}});

    f.update(key(scancode::space, true));
    CHECK(f.map.is_held(jump));
    CHECK(f.map.was_pressed(jump));
    CHECK(!f.map.was_released(jump));

    // Held across updates with nothing new, and auto-repeat is no new press.
    f.update(key(scancode::space, true, true));
    CHECK(f.map.is_held(jump));
    CHECK(!f.map.was_pressed(jump));

    f.update(key(scancode::space, false));
    CHECK(!f.map.is_held(jump));
    CHECK(f.map.was_released(jump));

    f.update();
    CHECK(!f.map.was_released(jump));
}

TEST("sr - input_map: a tap within one update is a press and a release, though never held")
{
    auto f = fixture();
    auto const use = f.map.add_action("use", input_action_kind::button);
    f.map.bind(use, {.source = mouse_button_binding{mouse_button::left}});

    input_event const tap[] = {{.payload = mouse_button_event{.button = mouse_button::left, .is_down = true}},
                               {.payload = mouse_button_event{.button = mouse_button::left, .is_down = false}}};
    f.update(tap);
    CHECK(f.map.was_pressed(use));
    CHECK(f.map.was_released(use));
    CHECK(!f.map.is_held(use));
}

TEST("sr - input_map: keys make a vector, and a diagonal is shortened to length 1")
{
    auto f = fixture();
    auto const move = f.map.add_action("move", input_action_kind::vector);
    f.map.bind(move, {.source = key_binding{scancode::w}, .direction = tg::vec2f(0, 1)});
    f.map.bind(move, {.source = key_binding{scancode::s}, .direction = tg::vec2f(0, -1)});
    f.map.bind(move, {.source = key_binding{scancode::d}, .direction = tg::vec2f(1, 0)});

    f.update(key(scancode::w, true));
    CHECK(f.map.vector(move) == tg::vec2f(0, 1));

    f.update(key(scancode::d, true));
    auto const v = f.map.vector(move);
    CHECK(near(v[0], 0.70710678f));
    CHECK(near(v[1], 0.70710678f));

    // Opposite keys cancel.
    f.update(key(scancode::s, true));
    CHECK(f.map.vector(move) == tg::vec2f(1, 0));
}

TEST("sr - input_map: an axis sums its bindings and clamps to [-1, 1]")
{
    auto f = fixture();
    auto const rise = f.map.add_action("rise", input_action_kind::axis);
    f.map.bind(rise, {.source = key_binding{scancode::space}});
    f.map.bind(rise, {.source = key_binding{scancode::left_shift}});
    f.map.bind(rise, {.source = key_binding{scancode::left_ctrl}, .direction = tg::vec2f(-1, 0)});

    input_event const both_up[] = {key(scancode::space, true), key(scancode::left_shift, true)};
    f.update(both_up);
    CHECK(f.map.value(rise) == 1.f);

    f.update(key(scancode::left_ctrl, true));
    CHECK(f.map.value(rise) == 1.f);

    input_event const ups[] = {key(scancode::space, false), key(scancode::left_shift, false)};
    f.update(ups);
    CHECK(f.map.value(rise) == -1.f);
}

TEST("sr - input_map: losing the focus lets go of everything held, as a release")
{
    auto f = fixture();
    auto const sprint = f.map.add_action("sprint", input_action_kind::button);
    f.map.bind(sprint, {.source = key_binding{scancode::left_shift}});

    f.update(key(scancode::left_shift, true));
    REQUIRE(f.map.is_held(sprint));

    // The key's release happens in another application, and never arrives.
    f.has_focus = false;
    f.update();
    CHECK(!f.map.is_held(sprint));
    CHECK(f.map.was_released(sprint));

    f.has_focus = true;
    f.update();
    CHECK(!f.map.is_held(sprint));
}

TEST("sr - input_map: each scroll of the wheel is a press and a release, and the wheel is never held")
{
    auto f = fixture();
    auto const next = f.map.add_action("next", input_action_kind::button);
    auto const previous = f.map.add_action("previous", input_action_kind::button);
    f.map.bind(next, {.source = mouse_wheel_binding{wheel_direction::down}});
    f.map.bind(previous, {.source = mouse_wheel_binding{wheel_direction::up}});

    f.update({.payload = mouse_wheel_event{.delta = tg::vec2f(0, -1)}});
    CHECK(f.map.was_pressed(next));
    CHECK(f.map.was_released(next));
    CHECK(!f.map.is_held(next));
    CHECK(!f.map.was_pressed(previous));

    f.update();
    CHECK(!f.map.was_pressed(next));
}

TEST("sr - input_map: mouse motion sums into motion, in logical pixels, and never into the vector")
{
    auto f = fixture();
    auto const look = f.map.add_action("look", input_action_kind::vector);
    f.map.bind(look, {.source = mouse_motion_binding{}});
    f.map.bind(look, {.source = gamepad_stick_binding{gamepad_stick::right}});

    input_event const moves[]
        = {mouse_move(tg::vec2f(4, 2)), {.payload = mouse_move_event{.delta = tg::vec2f(4, 4), .pixel_density = 2.f}}};
    f.update(moves);
    CHECK(f.map.motion(look) == tg::vec2f(6, 4));
    CHECK(f.map.vector(look) == tg::vec2f::zero);

    // Motion is per update: nothing moved, nothing to read.
    f.update();
    CHECK(f.map.motion(look) == tg::vec2f::zero);
}

TEST("sr - input_map: a stick reads nothing inside its dead zone, and rises to 1 past it")
{
    auto f = fixture();
    auto const move = f.map.add_action("move", input_action_kind::vector);
    auto const walk = f.map.add_action("walk", input_action_kind::button);
    f.map.bind(move, {.source = gamepad_stick_binding{gamepad_stick::left}});
    f.map.bind(walk, {.source = gamepad_stick_binding{gamepad_stick::left}});
    f.pads.push_back({.gamepad = 7});

    // The default dead zone is 0.2.
    f.pads[0].left_stick = tg::vec2f(0.15f, 0);
    f.update();
    CHECK(f.map.vector(move) == tg::vec2f::zero);
    CHECK(!f.map.is_held(walk));

    f.pads[0].left_stick = tg::vec2f(0, 0.6f);
    f.update();
    CHECK(f.map.vector(move)[0] == 0.f);
    CHECK(near(f.map.vector(move)[1], 0.5f));
    CHECK(f.map.is_held(walk)); // 0.5 is the press threshold

    f.pads[0].left_stick = tg::vec2f(0, -1);
    f.update();
    CHECK(f.map.vector(move) == tg::vec2f(0, -1));
}

TEST("sr - input_map: a trigger is half an axis, held once pressed halfway past its dead zone")
{
    auto f = fixture();
    auto const use = f.map.add_action("use", input_action_kind::button);
    f.map.bind(use, {.source = gamepad_axis_binding{gamepad_axis::right_trigger}});
    f.pads.push_back({.gamepad = 1});

    f.pads[0].right_trigger = 0.5f; // (0.5 - 0.1) / 0.9 is less than half
    f.update();
    CHECK(!f.map.is_held(use));
    CHECK(near(f.map.value(use), 0.4f / 0.9f));

    f.pads[0].right_trigger = 0.6f;
    f.update();
    CHECK(f.map.is_held(use));
    CHECK(f.map.was_pressed(use));
}

TEST("sr - input_map: a pad button counts from its event and from the pad's state")
{
    auto f = fixture();
    auto const jump = f.map.add_action("jump", input_action_kind::button);
    f.map.bind(jump, {.source = gamepad_button_binding{gamepad_button::south}});
    f.pads.push_back({.gamepad = 3});

    f.pads[0].held_buttons = gamepad_button_bit(gamepad_button::south);
    f.update(pad_button(3, gamepad_button::south, true));
    CHECK(f.map.is_held(jump));
    CHECK(f.map.was_pressed(jump));

    // Any pad counts, and one let go while the other holds the button is still held.
    f.pads.push_back({.gamepad = 4, .held_buttons = gamepad_button_bit(gamepad_button::south)});
    f.pads[0].held_buttons = 0;
    f.update(pad_button(3, gamepad_button::south, false));
    CHECK(f.map.is_held(jump));
}

TEST("sr - input_map: the active device is the one used last, with the pad's make and face")
{
    auto f = fixture();
    CHECK(f.map.active_device().kind == input_device_kind::keyboard_mouse);

    f.pads.push_back({.gamepad = 9, .kind = gamepad_kind::playstation_5, .face = gamepad_face::shapes});
    f.update({.payload = gamepad_connection_event{.gamepad = 9, .is_connected = true}});
    CHECK(f.map.active_device().kind == input_device_kind::keyboard_mouse); // plugged in is not used

    f.update(pad_button(9, gamepad_button::east, true));
    auto const dualsense = input_device{.kind = input_device_kind::gamepad,
                                        .gamepad = 9,
                                        .pad_kind = gamepad_kind::playstation_5,
                                        .face = gamepad_face::shapes};
    CHECK(f.map.active_device() == dualsense);

    // A mouse nudged by the desk is not a switch back; a mouse moved is.
    f.update(mouse_move(tg::vec2f(1, 1)));
    CHECK(f.map.active_device().kind == input_device_kind::gamepad);
    f.update(mouse_move(tg::vec2f(10, 0)));
    CHECK(f.map.active_device().kind == input_device_kind::keyboard_mouse);

    // A stick pushed far switches to its pad, and the pad going away switches back.
    f.update({.payload = gamepad_axis_event{.gamepad = 9, .axis = gamepad_axis::left_x, .value = 0.9f}});
    CHECK(f.map.active_device().gamepad == 9);
    f.pads.clear();
    f.update({.payload = gamepad_connection_event{.gamepad = 9, .is_connected = false}});
    CHECK(f.map.active_device().kind == input_device_kind::keyboard_mouse);
}

TEST("sr - input_map: an action names its bindings per device, the first one first, for prompts")
{
    auto f = fixture();
    auto const jump = f.map.add_action("jump", input_action_kind::button);
    auto const pause = f.map.add_action("pause", input_action_kind::button);
    f.map.bind(jump, {.source = gamepad_button_binding{gamepad_button::south}});
    f.map.bind(jump, {.source = key_binding{scancode::space}});
    f.map.bind(jump, {.source = key_binding{scancode::kp_0}});

    CHECK(f.map.find("jump") == jump);
    CHECK(!f.map.find("crouch").has_value());
    CHECK(f.map.name_of(pause) == "pause");
    CHECK(f.map.bindings_of(jump).size() == 3);

    auto const keyboard = f.map.binding_for(jump, input_device_kind::keyboard_mouse);
    REQUIRE(keyboard.has_value());
    CHECK(keyboard.value().source.as<key_binding>().key == scancode::space);
    auto const pad = f.map.binding_for(jump, input_device_kind::gamepad);
    REQUIRE(pad.has_value());
    CHECK(pad.value().source.as<gamepad_button_binding>().button == gamepad_button::south);
    CHECK(!f.map.binding_for(pause, input_device_kind::gamepad).has_value());

    f.map.unbind_all(jump);
    CHECK(f.map.bindings_of(jump).empty());
}

TEST("sr - input_map: a press is captured as the binding it makes, and nothing else is")
{
    auto const captured = [](input_event const& e) { return binding_pressed_in(e); };

    REQUIRE(captured(key(scancode::q, true)).has_value());
    CHECK(captured(key(scancode::q, true)).value().source.as<key_binding>().key == scancode::q);
    CHECK(!captured(key(scancode::q, true, true)).has_value()); // auto-repeat
    CHECK(!captured(key(scancode::q, false)).has_value());
    CHECK(!captured(mouse_move(tg::vec2f(50, 0))).has_value());

    auto const wheel = captured({.payload = mouse_wheel_event{.delta = tg::vec2f(0, -2)}});
    REQUIRE(wheel.has_value());
    CHECK(wheel.value().source.as<mouse_wheel_binding>().direction == wheel_direction::down);

    // A stick resting near the middle is no press; pushed far, it is the half of its axis it went along.
    CHECK(!captured({.payload = gamepad_axis_event{.axis = gamepad_axis::left_y, .value = 0.1f}}).has_value());
    auto const stick = captured({.payload = gamepad_axis_event{.axis = gamepad_axis::left_y, .value = -0.8f}});
    REQUIRE(stick.has_value());
    CHECK(stick.value().source.as<gamepad_axis_binding>().axis == gamepad_axis::left_y);
    CHECK(!stick.value().source.as<gamepad_axis_binding>().positive);

    CHECK(captured(pad_button(2, gamepad_button::north, true)).value().source.as<gamepad_button_binding>().button
          == gamepad_button::north);
}

TEST("sr - input_map: every input's text names it back, and text of no input names none")
{
    auto sources = cc::vector<input_binding>();
    for (auto k = int(scancode::a); k <= int(scancode::kp_period); ++k)
        sources.push_back({.source = key_binding{scancode(k)}});
    for (auto b = 0; b <= int(mouse_button::x2); ++b)
        sources.push_back({.source = mouse_button_binding{mouse_button(b)}});
    for (auto w = 0; w <= int(wheel_direction::right); ++w)
        sources.push_back({.source = mouse_wheel_binding{wheel_direction(w)}});
    sources.push_back({.source = mouse_motion_binding{}});
    for (auto b = 0; b <= int(gamepad_button::dpad_right); ++b)
        sources.push_back({.source = gamepad_button_binding{gamepad_button(b)}});
    for (auto a = 0; a <= int(gamepad_axis::right_trigger); ++a)
        for (auto const positive : {true, false})
            sources.push_back({.source = gamepad_axis_binding{gamepad_axis(a), positive}});
    sources.push_back({.source = gamepad_stick_binding{gamepad_stick::left}});
    sources.push_back({.source = gamepad_stick_binding{gamepad_stick::right}});

    for (auto const& b : sources)
    {
        auto const text = source_name_of(b);
        auto const back = parse_binding_source(text);
        REQUIRE(back.has_value());
        CHECK(source_name_of(back.value()) == text);
    }
    CHECK(source_name_of({.source = key_binding{scancode::left_shift}}) == "key:left_shift");
    CHECK(source_name_of({.source = gamepad_axis_binding{gamepad_axis::left_y, false}}) == "axis:left_y-");

    for (auto const text : {"", "key:", "key:unknown", "key:shift", "pad", "axis:left_y", "stick:middle", "motion:x"})
        CHECK(!parse_binding_source(text).has_value()).dump("text", text);
}

TEST("sr - input_map: bindings written as JSON load back, and replace only the actions they name")
{
    auto saved = input_map();
    auto const move = saved.add_action("move", input_action_kind::vector);
    auto const jump = saved.add_action("jump", input_action_kind::button);
    saved.bind(move, {.source = key_binding{scancode::up}, .direction = tg::vec2f(0, 1)});
    saved.bind(move, {.source = gamepad_stick_binding{gamepad_stick::left}});
    saved.bind(jump, {.source = gamepad_button_binding{gamepad_button::east}});

    auto writer = babel::json::string_writer();
    writer.underlying().begin_object();
    saved.write_bindings(writer.underlying());
    writer.underlying().end_object();
    auto const json = writer.finish();
    REQUIRE(json.has_value());

    // A map of the same actions, bound otherwise, with one action the file does not name.
    auto loaded = input_map();
    auto const loaded_move = loaded.add_action("move", input_action_kind::vector);
    auto const loaded_jump = loaded.add_action("jump", input_action_kind::button);
    auto const crouch = loaded.add_action("crouch", input_action_kind::button);
    loaded.bind(loaded_move, {.source = key_binding{scancode::w}, .direction = tg::vec2f(0, 1)});
    loaded.bind(crouch, {.source = key_binding{scancode::c}});

    auto const doc = babel::json::read(json.value());
    REQUIRE(doc.has_value());
    loaded.load_bindings(doc.value().root());

    REQUIRE(loaded.bindings_of(loaded_move).size() == 2);
    CHECK(source_name_of(loaded.bindings_of(loaded_move)[0]) == "key:up");
    CHECK(loaded.bindings_of(loaded_move)[0].direction == tg::vec2f(0, 1));
    CHECK(source_name_of(loaded.bindings_of(loaded_move)[1]) == "stick:left");
    CHECK(loaded.bindings_of(loaded_move)[1].direction == tg::vec2f(1, 0));
    REQUIRE(loaded.bindings_of(loaded_jump).size() == 1);
    CHECK(source_name_of(loaded.bindings_of(loaded_jump)[0]) == "pad:east");
    REQUIRE(loaded.bindings_of(crouch).size() == 1); // kept
}

TEST("sr - input_map: a file of old bindings loads what it can, and skips the rest")
{
    nx::expect_warning("this map has none of");
    nx::expect_warning("no input sr knows");
    auto map = input_map();
    auto const jump = map.add_action("jump", input_action_kind::button);
    auto const doc = babel::json::read(R"({"jump": [{"input": "key:space"}, {"input": "key:hyperspace"}],
                                           "teleport": [{"input": "key:t"}]})");
    REQUIRE(doc.has_value());
    map.load_bindings(doc.value().root());
    REQUIRE(map.bindings_of(jump).size() == 1);
    CHECK(source_name_of(map.bindings_of(jump)[0]) == "key:space");

    // An action index stays put across set and remove.
    map.bind(jump, {.source = key_binding{scancode::enter}});
    map.set_binding(jump, 0, {.source = key_binding{scancode::j}});
    CHECK(source_name_of(map.bindings_of(jump)[0]) == "key:j");
    map.remove_binding(jump, 0);
    REQUIRE(map.bindings_of(jump).size() == 1);
    CHECK(source_name_of(map.bindings_of(jump)[0]) == "key:enter");
}
