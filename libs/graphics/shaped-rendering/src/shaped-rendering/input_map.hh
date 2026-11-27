#pragma once

#include <babel-data/data/json.hh>
#include <clean-core/container/fixed_bitset.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/variant.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-rendering/input.hh>
#include <typed-geometry/linalg/vec.hh>

/// One action of an input_map, as `add_action` returned it; it means nothing to another map.
struct sr::input_action
{
    u16 index = u16(-1);

    [[nodiscard]] bool is_valid() const { return index != u16(-1); }
    [[nodiscard]] bool operator==(input_action const&) const = default;
};

/// What an action reads as: a button is held or not, an axis is a value in [-1, 1], a vector a 2D value of length at most 1.
/// Every kind answers every query; the kind decides only how its bindings combine.
enum class sr::input_action_kind : sr::u8
{
    button,
    axis,
    vector,
};

/// Which family of devices a binding belongs to, and so what a prompt for it is drawn from.
enum class sr::input_device_kind : sr::u8
{
    keyboard_mouse,
    gamepad,
};

/// The device a player last used, which is what prompts should show.
struct sr::input_device
{
    input_device_kind kind = input_device_kind::keyboard_mouse;
    /// The pad, its make and its face legends; meaningful only for a gamepad.
    u32 gamepad = 0;
    gamepad_kind pad_kind = gamepad_kind::unknown;
    gamepad_face face = gamepad_face::abxy;

    [[nodiscard]] bool operator==(input_device const&) const = default;
};

/// A direction the wheel scrolls in; up is away from the user, as `mouse_wheel_event::delta` has it.
enum class sr::wheel_direction : sr::u8
{
    up,
    down,
    left,
    right,
};

/// A stick of a gamepad.
enum class sr::gamepad_stick : sr::u8
{
    left,
    right,
};

/// The real inputs an action can be bound to.
/// A key, a button or a half axis is held or not, and adds the binding's `direction` while held, a half axis scaled by how far it is pushed.
/// The wheel is never held: each scroll is a press and a release in the same update.
/// A stick adds its deflection as a vector, and mouse motion adds only to `motion`, as a distance rather than a deflection.
struct sr::key_binding
{
    scancode key = scancode::unknown;
};
struct sr::mouse_button_binding
{
    mouse_button button = mouse_button::left;
};
struct sr::mouse_wheel_binding
{
    wheel_direction direction = wheel_direction::up;
};
/// The pointer's motion in logical pixels, x right and y down, read through `input_map::motion`.
struct sr::mouse_motion_binding
{
};
struct sr::gamepad_button_binding
{
    gamepad_button button = gamepad_button::south;
};
/// One half of an axis: `positive` for the half above zero, which is all a trigger has.
struct sr::gamepad_axis_binding
{
    gamepad_axis axis = gamepad_axis::left_trigger;
    bool positive = true;
};
struct sr::gamepad_stick_binding
{
    gamepad_stick stick = gamepad_stick::left;
};

/// One real input bound to an action.
struct sr::input_binding
{
    cc::variant<key_binding, mouse_button_binding, mouse_wheel_binding, mouse_motion_binding, gamepad_button_binding, gamepad_axis_binding, gamepad_stick_binding>
        source;

    /// What the binding adds to its action while held: an axis reads x, so a key that lowers it has (-1, 0).
    /// A vector's keys each name their direction, (0, 1) for forward; a stick ignores it.
    tg::vec2f direction = tg::vec2f(1, 0);

    [[nodiscard]] input_device_kind device() const;
};

namespace sr
{
/// The binding a press in `e` makes, which is what rebinding waits for: a key or a mouse or pad button going down, a scroll,
/// or a stick or trigger pushed past `threshold`, a stick as the half of its axis it was pushed along.
/// None for anything else, a key's auto-repeat and the pointer's motion included.
[[nodiscard]] cc::optional<input_binding> binding_pressed_in(input_event const& e, float threshold = 0.5f);

/// The binding's source as stable text, for a settings file: `key:space`, `mouse:left`, `wheel:up`, `motion`,
/// `pad:south`, `axis:left_y-`, `stick:left`; keys and buttons by their enumerator's name.
[[nodiscard]] cc::string source_name_of(input_binding const& binding);
/// The binding `text` names, as `source_name_of` writes it, with the default direction; none for text it does not.
[[nodiscard]] cc::optional<input_binding> parse_binding_source(cc::string_view text);
} // namespace sr

/// What an input_map's thresholds are.
struct sr::input_map_config
{
    /// How far a stick must be pushed to read as more than zero; past it, its reading rises from 0 to 1.
    float stick_dead_zone = 0.2f;
    /// The same for one half of an axis, a trigger above all.
    float axis_dead_zone = 0.1f;
    /// How far a stick or a half axis must be pushed, after its dead zone, to count as held.
    float press_threshold = 0.5f;
    /// How far the pointer must move in one update, in logical pixels, to make the mouse the device in use again.
    float mouse_switch_distance = 4.f;
};

/// Semantic actions bound to real keys, buttons and axes, read once per frame: Godot's input map, in sr's events.
///
///     auto map = sr::input_map();
///     auto const jump = map.add_action("jump", sr::input_action_kind::button);
///     map.bind(jump, {.source = sr::key_binding{sr::scancode::space}});
///     map.bind(jump, {.source = sr::gamepad_button_binding{sr::gamepad_button::south}});
///     ...
///     wsys->poll_events();
///     map.update(*wsys);
///     if (map.was_pressed(jump)) ...
///
/// Every device counts at once, every pad included; `active_device` is only which one was used last, for prompts.
/// While no window has the focus nothing is held: keys let go elsewhere never report their release.
/// A press and a release within one update both count, so a quick tap is never lost between two frames.
class sr::input_map
{
public:
    explicit input_map(input_map_config config = {}) : _config(config) {}

    /// A new action named `name`, which should be unique within the map, with no bindings yet.
    [[nodiscard]] input_action add_action(cc::string name, input_action_kind kind);
    /// The action named `name`, or none.
    [[nodiscard]] cc::optional<input_action> find(cc::string_view name) const;

    void bind(input_action action, input_binding binding);
    void unbind_all(input_action action);
    /// Replaces the binding at `index` of `bindings_of(action)`, which must exist, keeping its place.
    void set_binding(input_action action, isize index, input_binding binding);
    /// Removes the binding at `index` of `bindings_of(action)`, which must exist; the ones after it move up.
    void remove_binding(input_action action, isize index);

    /// Every action's bindings as members of the JSON object `out` has open: an array per action, by name,
    /// each binding `{"input": "key:w", "direction": [0, 1]}`, the direction left out where it is (1, 0).
    void write_bindings(babel::json::writer& out) const;
    /// Replaces the bindings of each action `bindings` names, an object as `write_bindings` writes it; the others keep theirs.
    /// A name no action has, and a binding that does not parse, are skipped and logged, so an old file still loads.
    void load_bindings(babel::json::ref bindings);

    [[nodiscard]] cc::string_view name_of(input_action action) const;
    [[nodiscard]] input_action_kind kind_of(input_action action) const;
    /// Every binding of `action`, in the order they were bound.
    [[nodiscard]] cc::span<input_binding const> bindings_of(input_action action) const;
    /// The first binding of `action` on a device of kind `device`, which is what a prompt for it shows; none when it has none there.
    [[nodiscard]] cc::optional<input_binding> binding_for(input_action action, input_device_kind device) const;

    /// Takes in what a window system's last poll_events collected: its events, and the state of its pads.
    void update(window_system const& wsys);
    /// The same from the pieces, which is what a test feeds; `has_focus` is whether a window of the application has it.
    void update(cc::span<input_event const> events, cc::span<gamepad_state const> pads, bool has_focus);

    [[nodiscard]] bool is_held(input_action action) const;
    /// Whether the action went down during the last update, or was pressed and let go within it.
    [[nodiscard]] bool was_pressed(input_action action) const;
    [[nodiscard]] bool was_released(input_action action) const;
    /// An axis action's value, its bindings summed and clamped to [-1, 1]; for a vector, its x.
    [[nodiscard]] float value(input_action action) const;
    /// The action's bindings summed, and shortened to length 1 when longer, as moving diagonally needs.
    [[nodiscard]] tg::vec2f vector(input_action action) const;
    /// How far the pointer moved during the last update, in logical pixels, summed over the action's mouse motion bindings.
    [[nodiscard]] tg::vec2f motion(input_action action) const;

    /// The device the player last pressed, pushed or moved, which starts as the keyboard and mouse.
    [[nodiscard]] input_device active_device() const { return _active_device; }

private:
    struct action_state
    {
        cc::string name;
        input_action_kind kind = input_action_kind::button;
        cc::vector<input_binding> bindings;

        bool held = false;
        bool pressed = false;
        bool released = false;
        tg::vec2f sum = tg::vec2f::zero;
        tg::vec2f motion = tg::vec2f::zero;
    };

    [[nodiscard]] action_state const& state_of(input_action action) const;
    /// How far `binding` is pushed right now, 0 to 1, from the held keys and buttons and the pads.
    [[nodiscard]] float amount_of(input_binding const& binding, cc::span<gamepad_state const> pads) const;
    /// Marks every action with a binding `matches` accepts as pressed, or as released.
    void mark_edges(cc::function_ref<bool(input_binding const&)> matches, bool is_down);

    input_map_config _config;
    cc::vector<action_state> _actions;
    input_device _active_device;

    /// The keys and mouse buttons held, as the events reported them: a bit per scancode, and one per `mouse_button_bit`.
    cc::fixed_bitset<256> _held_keys;
    u8 _held_mouse_buttons = 0;
};
