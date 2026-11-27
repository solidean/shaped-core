#include "input_map.hh"

#include <clean-core/common/log.hh>
#include <clean-core/string/format.hh>
#include <typed-geometry/scalar/scalar.hh>

// The text form of a binding and the JSON of a map's bindings, which a settings file stores.
// Every name below is written to users' files, so renaming one breaks the files that hold it.

namespace
{
using namespace cc::primitive_defines;

/// sr::scancode's enumerators by name, in their order.
constexpr char const* scancode_names[] = {
    "unknown",
    "a",
    "b",
    "c",
    "d",
    "e",
    "f",
    "g",
    "h",
    "i",
    "j",
    "k",
    "l",
    "m",
    "n",
    "o",
    "p",
    "q",
    "r",
    "s",
    "t",
    "u",
    "v",
    "w",
    "x",
    "y",
    "z",
    "num_0",
    "num_1",
    "num_2",
    "num_3",
    "num_4",
    "num_5",
    "num_6",
    "num_7",
    "num_8",
    "num_9",
    "enter",
    "escape",
    "backspace",
    "tab",
    "space",
    "insert",
    "del",
    "home",
    "end",
    "page_up",
    "page_down",
    "left",
    "right",
    "up",
    "down",
    "minus",
    "equals",
    "left_bracket",
    "right_bracket",
    "backslash",
    "semicolon",
    "apostrophe",
    "grave",
    "comma",
    "period",
    "slash",
    "left_shift",
    "right_shift",
    "left_ctrl",
    "right_ctrl",
    "left_alt",
    "right_alt",
    "left_super",
    "right_super",
    "caps_lock",
    "num_lock",
    "scroll_lock",
    "print_screen",
    "pause",
    "menu",
    "f1",
    "f2",
    "f3",
    "f4",
    "f5",
    "f6",
    "f7",
    "f8",
    "f9",
    "f10",
    "f11",
    "f12",
    "kp_0",
    "kp_1",
    "kp_2",
    "kp_3",
    "kp_4",
    "kp_5",
    "kp_6",
    "kp_7",
    "kp_8",
    "kp_9",
    "kp_divide",
    "kp_multiply",
    "kp_minus",
    "kp_plus",
    "kp_enter",
    "kp_period",
};
static_assert(sizeof(scancode_names) / sizeof(scancode_names[0]) == int(sr::scancode::kp_period) + 1,
              "every scancode needs its name");

constexpr char const* mouse_button_names[] = {"left", "middle", "right", "x1", "x2"};
constexpr char const* wheel_names[] = {"up", "down", "left", "right"};
constexpr char const* gamepad_button_names[] = {
    "south",       "east",          "west",           "north",   "back",      "guide",     "start",      "left_stick",
    "right_stick", "left_shoulder", "right_shoulder", "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};
static_assert(sizeof(gamepad_button_names) / sizeof(gamepad_button_names[0]) == int(sr::gamepad_button::dpad_right) + 1,
              "every gamepad button needs its name");
constexpr char const* gamepad_axis_names[]
    = {"left_x", "left_y", "right_x", "right_y", "left_trigger", "right_trigger"};
constexpr char const* stick_names[] = {"left", "right"};

/// The index of `name` in `names`, or -1.
template <isize N>
[[nodiscard]] isize index_of(char const* const (&names)[N], cc::string_view name)
{
    for (auto i = isize(0); i < N; ++i)
        if (cc::string_view(names[i]) == name)
            return i;
    return -1;
}
} // namespace

cc::optional<sr::input_binding> sr::binding_pressed_in(input_event const& e, float threshold)
{
    using result = cc::optional<input_binding>;
    return e.payload.visit(
        [](key_event const& k) -> result
        {
            if (!k.is_down || k.is_repeat || k.scancode == scancode::unknown)
                return {};
            return input_binding{.source = key_binding{k.scancode}};
        },
        [](mouse_button_event const& m) -> result
        { return m.is_down ? result(input_binding{.source = mouse_button_binding{m.button}}) : result(); },
        [](mouse_wheel_event const& w) -> result
        {
            if (w.delta[1] != 0.f)
                return input_binding{
                    .source = mouse_wheel_binding{w.delta[1] > 0.f ? wheel_direction::up : wheel_direction::down}};
            if (w.delta[0] != 0.f)
                return input_binding{
                    .source = mouse_wheel_binding{w.delta[0] > 0.f ? wheel_direction::right : wheel_direction::left}};
            return {};
        },
        [](gamepad_button_event const& g) -> result
        { return g.is_down ? result(input_binding{.source = gamepad_button_binding{g.button}}) : result(); },
        [&](gamepad_axis_event const& g) -> result
        {
            if (tg::abs(g.value) < threshold)
                return {};
            return input_binding{.source = gamepad_axis_binding{g.axis, g.value > 0.f}};
        },
        [](mouse_move_event const&) -> result { return {}; }, [](text_event const&) -> result { return {}; },
        [](file_drop_event const&) -> result { return {}; },
        [](gamepad_connection_event const&) -> result { return {}; });
}

cc::string sr::source_name_of(input_binding const& binding)
{
    return binding.source.visit(
        [](key_binding const& k) { return cc::format("key:{}", scancode_names[int(k.key)]); },
        [](mouse_button_binding const& m) { return cc::format("mouse:{}", mouse_button_names[int(m.button)]); },
        [](mouse_wheel_binding const& w) { return cc::format("wheel:{}", wheel_names[int(w.direction)]); },
        [](mouse_motion_binding const&) { return cc::string("motion"); }, [](gamepad_button_binding const& g)
        { return cc::format("pad:{}", gamepad_button_names[int(g.button)]); }, [](gamepad_axis_binding const& a)
        { return cc::format("axis:{}{}", gamepad_axis_names[int(a.axis)], a.positive ? "+" : "-"); },
        [](gamepad_stick_binding const& s) { return cc::format("stick:{}", stick_names[int(s.stick)]); });
}

cc::optional<sr::input_binding> sr::parse_binding_source(cc::string_view text)
{
    if (text == "motion")
        return input_binding{.source = mouse_motion_binding{}};
    auto const colon = text.find(':');
    if (colon <= 0)
        return {};
    auto const kind = text.subview({.offset = 0, .size = colon});
    auto name = text.subview(colon + 1);

    if (kind == "key")
    {
        auto const i = index_of(scancode_names, name);
        return i > 0 ? cc::optional<input_binding>(input_binding{.source = key_binding{scancode(i)}}) : cc::nullopt;
    }
    if (kind == "mouse")
    {
        auto const i = index_of(mouse_button_names, name);
        return i >= 0 ? cc::optional<input_binding>(input_binding{.source = mouse_button_binding{mouse_button(i)}})
                      : cc::nullopt;
    }
    if (kind == "wheel")
    {
        auto const i = index_of(wheel_names, name);
        return i >= 0 ? cc::optional<input_binding>(input_binding{.source = mouse_wheel_binding{wheel_direction(i)}})
                      : cc::nullopt;
    }
    if (kind == "pad")
    {
        auto const i = index_of(gamepad_button_names, name);
        return i >= 0 ? cc::optional<input_binding>(input_binding{.source = gamepad_button_binding{gamepad_button(i)}})
                      : cc::nullopt;
    }
    if (kind == "axis")
    {
        if (!name.ends_with('+') && !name.ends_with('-'))
            return {};
        auto const positive = name.ends_with('+');
        name.remove_suffix(1);
        auto const i = index_of(gamepad_axis_names, name);
        return i >= 0
                 ? cc::optional<input_binding>(input_binding{.source = gamepad_axis_binding{gamepad_axis(i), positive}})
                 : cc::nullopt;
    }
    if (kind == "stick")
    {
        auto const i = index_of(stick_names, name);
        return i >= 0 ? cc::optional<input_binding>(input_binding{.source = gamepad_stick_binding{gamepad_stick(i)}})
                      : cc::nullopt;
    }
    return {};
}

void sr::input_map::write_bindings(babel::json::writer& out) const
{
    for (auto const& a : _actions)
    {
        out.begin_array(a.name);
        for (auto const& b : a.bindings)
        {
            out.begin_object(babel::json::layout::compact);
            out.write("input", source_name_of(b));
            if (b.direction != tg::vec2f(1, 0))
            {
                out.begin_array("direction");
                out.write(b.direction[0]);
                out.write(b.direction[1]);
                out.end_array();
            }
            out.end_object();
        }
        out.end_array();
    }
}

void sr::input_map::load_bindings(babel::json::ref bindings)
{
    for (auto i = isize(0); i < bindings.size(); ++i)
    {
        auto const entry = bindings[i];
        auto const action = find(entry.key());
        if (!action.has_value())
        {
            CC_LOG_WARNING("the bindings name an action '{}' this map has none of, and are skipped", entry.key());
            continue;
        }
        unbind_all(action.value());
        for (auto j = isize(0); j < entry.size(); ++j)
        {
            auto const input = entry[j]["input"].as_string();
            auto binding = parse_binding_source(input);
            if (!binding.has_value())
            {
                CC_LOG_WARNING("'{}' binds '{}', which is no input sr knows, and is skipped", entry.key(), input);
                continue;
            }
            if (auto const direction = entry[j]["direction"]; direction.size() == 2)
                binding.value().direction = tg::vec2f(float(direction[0].as_double(0)), float(direction[1].as_double(0)));
            bind(action.value(), binding.value());
        }
    }
}
