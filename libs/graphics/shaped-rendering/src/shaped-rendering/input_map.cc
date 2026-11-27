#include "input_map.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <shaped-rendering/window.hh>
#include <typed-geometry/scalar/scalar.hh>

namespace
{
[[nodiscard]] float length_of(tg::vec2f v)
{
    return tg::sqrt(v[0] * v[0] + v[1] * v[1]);
}

/// How far past `dead_zone` a reading of 0 to 1 is, rescaled so the reading still reaches 1.
[[nodiscard]] float past_dead_zone(float reading, float dead_zone)
{
    return reading <= dead_zone ? 0.f : cc::min((reading - dead_zone) / (1.f - dead_zone), 1.f);
}

[[nodiscard]] float axis_reading(sr::gamepad_state const& pad, sr::gamepad_axis axis)
{
    switch (axis)
    {
    case sr::gamepad_axis::left_x:
        return pad.left_stick[0];
    case sr::gamepad_axis::left_y:
        return pad.left_stick[1];
    case sr::gamepad_axis::right_x:
        return pad.right_stick[0];
    case sr::gamepad_axis::right_y:
        return pad.right_stick[1];
    case sr::gamepad_axis::left_trigger:
        return pad.left_trigger;
    case sr::gamepad_axis::right_trigger:
        return pad.right_trigger;
    }
    return 0.f;
}

/// The stick's deflection past its dead zone, on the pad that pushes it furthest.
[[nodiscard]] tg::vec2f stick_reading(cc::span<sr::gamepad_state const> pads, sr::gamepad_stick stick, float dead_zone)
{
    auto best = tg::vec2f::zero;
    for (auto const& pad : pads)
    {
        auto const v = stick == sr::gamepad_stick::left ? pad.left_stick : pad.right_stick;
        auto const length = length_of(v);
        auto const reading = length > 0.f ? v * (past_dead_zone(length, dead_zone) / length) : tg::vec2f::zero;
        if (length_of(reading) > length_of(best))
            best = reading;
    }
    return best;
}

/// Whether the binding is a key or a button, which is held only all the way down.
[[nodiscard]] bool is_digital(sr::input_binding const& b)
{
    return b.source.is<sr::key_binding>() || b.source.is<sr::mouse_button_binding>()
        || b.source.is<sr::gamepad_button_binding>();
}

[[nodiscard]] bool is_trigger(sr::gamepad_axis axis)
{
    return axis == sr::gamepad_axis::left_trigger || axis == sr::gamepad_axis::right_trigger;
}
} // namespace

sr::input_device_kind sr::input_binding::device() const
{
    return source.is<key_binding>() || source.is<mouse_button_binding>() || source.is<mouse_wheel_binding>()
                || source.is<mouse_motion_binding>()
             ? input_device_kind::keyboard_mouse
             : input_device_kind::gamepad;
}

sr::input_action sr::input_map::add_action(cc::string name, input_action_kind kind)
{
    CC_ASSERT(_actions.size() < isize(u16(-1)), "an input_map holds at most 65535 actions");
    _actions.push_back({.name = cc::move(name), .kind = kind});
    return {.index = u16(_actions.size() - 1)};
}

cc::optional<sr::input_action> sr::input_map::find(cc::string_view name) const
{
    for (auto i = isize(0); i < _actions.size(); ++i)
        if (cc::string_view(_actions[i].name) == name)
            return input_action{.index = u16(i)};
    return {};
}

sr::input_map::action_state const& sr::input_map::state_of(input_action action) const
{
    CC_ASSERT(action.is_valid() && isize(action.index) < _actions.size(), "the action must be one of this map's");
    return _actions[action.index];
}

void sr::input_map::bind(input_action action, input_binding binding)
{
    (void)state_of(action);
    _actions[action.index].bindings.push_back(cc::move(binding));
}

void sr::input_map::unbind_all(input_action action)
{
    (void)state_of(action);
    _actions[action.index].bindings.clear();
}

cc::string_view sr::input_map::name_of(input_action action) const
{
    return state_of(action).name;
}

sr::input_action_kind sr::input_map::kind_of(input_action action) const
{
    return state_of(action).kind;
}

cc::span<sr::input_binding const> sr::input_map::bindings_of(input_action action) const
{
    return state_of(action).bindings;
}

cc::optional<sr::input_binding> sr::input_map::binding_for(input_action action, input_device_kind device) const
{
    for (auto const& b : state_of(action).bindings)
        if (b.device() == device)
            return b;
    return {};
}

bool sr::input_map::is_held(input_action action) const
{
    return state_of(action).held;
}

bool sr::input_map::was_pressed(input_action action) const
{
    return state_of(action).pressed;
}

bool sr::input_map::was_released(input_action action) const
{
    return state_of(action).released;
}

float sr::input_map::value(input_action action) const
{
    return cc::clamp(state_of(action).sum[0], -1.f, 1.f);
}

tg::vec2f sr::input_map::vector(input_action action) const
{
    auto const sum = state_of(action).sum;
    auto const length = length_of(sum);
    return length > 1.f ? sum / length : sum;
}

tg::vec2f sr::input_map::motion(input_action action) const
{
    return state_of(action).motion;
}

void sr::input_map::mark_edges(cc::function_ref<bool(input_binding const&)> matches, bool is_down)
{
    for (auto& a : _actions)
        for (auto const& b : a.bindings)
            if (matches(b))
            {
                (is_down ? a.pressed : a.released) = true;
                break;
            }
}

float sr::input_map::amount_of(input_binding const& binding, cc::span<gamepad_state const> pads) const
{
    auto pad_max = [&](auto const& reading)
    {
        auto best = 0.f;
        for (auto const& pad : pads)
            best = cc::max(best, reading(pad));
        return best;
    };
    return binding.source.visit(
        [&](key_binding const& k) { return _held_keys[isize(k.key)] ? 1.f : 0.f; }, [&](mouse_button_binding const& m)
        { return (_held_mouse_buttons & mouse_button_bit(m.button)) != 0 ? 1.f : 0.f; }, [](mouse_wheel_binding const&)
        { return 0.f; }, [](mouse_motion_binding const&) { return 0.f; }, [&](gamepad_button_binding const& g)
        { return pad_max([&](gamepad_state const& pad) { return pad.is_held(g.button) ? 1.f : 0.f; }); },
        [&](gamepad_axis_binding const& g)
        {
            auto const dead_zone = is_trigger(g.axis) ? _config.axis_dead_zone : _config.stick_dead_zone;
            return pad_max([&](gamepad_state const& pad)
                           { return past_dead_zone(axis_reading(pad, g.axis) * (g.positive ? 1.f : -1.f), dead_zone); });
        },
        [&](gamepad_stick_binding const& g) { return length_of(stick_reading(pads, g.stick, _config.stick_dead_zone)); });
}

void sr::input_map::update(window_system const& wsys)
{
    auto has_focus = false;
    for (auto const* w : wsys.windows())
        has_focus = has_focus || w->is_focused();
    update(wsys.events(), wsys.gamepads(), has_focus);
}

void sr::input_map::update(cc::span<input_event const> events, cc::span<gamepad_state const> pads, bool has_focus)
{
    for (auto& a : _actions)
    {
        a.pressed = false;
        a.released = false;
        a.motion = tg::vec2f::zero;
    }

    // Without the focus nothing is reported, the releases of what was held included, so nothing counts as held.
    if (!has_focus)
    {
        _held_keys.unset_all();
        _held_mouse_buttons = 0;
        pads = {};
    }

    auto const activate_pad = [&](u32 id)
    {
        _active_device = {.kind = input_device_kind::gamepad, .gamepad = id};
        for (auto const& pad : pads)
            if (pad.gamepad == id)
            {
                _active_device.pad_kind = pad.kind;
                _active_device.face = pad.face;
            }
    };
    auto const activate_keyboard_mouse = [&] { _active_device = {}; };

    auto moved = tg::vec2f::zero;
    for (auto const& e : has_focus ? events : cc::span<input_event const>())
        e.payload.visit(
            [&](key_event const& k)
            {
                if (k.is_repeat || k.scancode == scancode::unknown)
                    return;
                _held_keys.set(isize(k.scancode), k.is_down);
                mark_edges(
                    [&](input_binding const& b)
                    {
                        auto const* key = b.source.try_as<key_binding>();
                        return key != nullptr && key->key == k.scancode;
                    },
                    k.is_down);
                if (k.is_down)
                    activate_keyboard_mouse();
            },
            [&](mouse_button_event const& m)
            {
                _held_mouse_buttons = m.is_down ? u8(_held_mouse_buttons | mouse_button_bit(m.button))
                                                : u8(_held_mouse_buttons & ~mouse_button_bit(m.button));
                mark_edges(
                    [&](input_binding const& b)
                    {
                        auto const* button = b.source.try_as<mouse_button_binding>();
                        return button != nullptr && button->button == m.button;
                    },
                    m.is_down);
                if (m.is_down)
                    activate_keyboard_mouse();
            },
            [&](mouse_wheel_event const& w)
            {
                auto const scroll = [&](wheel_direction direction)
                {
                    auto const matches = [&](input_binding const& b)
                    {
                        auto const* wheel = b.source.try_as<mouse_wheel_binding>();
                        return wheel != nullptr && wheel->direction == direction;
                    };
                    mark_edges(matches, true);
                    mark_edges(matches, false);
                    activate_keyboard_mouse();
                };
                if (w.delta[1] > 0.f)
                    scroll(wheel_direction::up);
                if (w.delta[1] < 0.f)
                    scroll(wheel_direction::down);
                if (w.delta[0] > 0.f)
                    scroll(wheel_direction::right);
                if (w.delta[0] < 0.f)
                    scroll(wheel_direction::left);
            },
            [&](mouse_move_event const& m)
            {
                auto const delta = m.logical_delta();
                moved = moved + delta;
                for (auto& a : _actions)
                    for (auto const& b : a.bindings)
                        if (b.source.is<mouse_motion_binding>())
                            a.motion = a.motion + delta;
            },
            [&](gamepad_button_event const& g)
            {
                mark_edges(
                    [&](input_binding const& b)
                    {
                        auto const* button = b.source.try_as<gamepad_button_binding>();
                        return button != nullptr && button->button == g.button;
                    },
                    g.is_down);
                if (g.is_down)
                    activate_pad(g.gamepad);
            },
            [&](gamepad_axis_event const& g)
            {
                if (tg::abs(g.value) >= _config.press_threshold)
                    activate_pad(g.gamepad);
            },
            [&](gamepad_connection_event const& g)
            {
                if (!g.is_connected && _active_device.kind == input_device_kind::gamepad
                    && _active_device.gamepad == g.gamepad)
                    activate_keyboard_mouse();
            },
            [](text_event const&) {}, [](file_drop_event const&) {});

    if (length_of(moved) >= _config.mouse_switch_distance)
        activate_keyboard_mouse();

    for (auto& a : _actions)
    {
        auto const was_held = a.held;
        a.held = false;
        a.sum = tg::vec2f::zero;
        for (auto const& b : a.bindings)
        {
            auto const amount = amount_of(b, pads);
            a.held = a.held || amount >= (is_digital(b) ? 1.f : _config.press_threshold);
            auto const* stick = b.source.try_as<gamepad_stick_binding>();
            a.sum
                = a.sum
                + (stick != nullptr ? stick_reading(pads, stick->stick, _config.stick_dead_zone) : b.direction * amount);
        }
        a.pressed = a.pressed || (!was_held && a.held);
        a.released = a.released || (was_held && !a.held);
    }
}
