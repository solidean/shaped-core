#pragma once

#include <clean-core/string/string.hh>
#include <shaped-rendering/fwd.hh>
#include <typed-geometry/linalg/pos.hh> // tg::pos2f
#include <typed-geometry/linalg/vec.hh> // tg::vec2f

#include <variant>

namespace sr
{
/// A key by physical position on the keyboard, independent of layout.
/// W is the key above A on QWERTY and above Q on AZERTY, so WASD stays WASD everywhere.
/// For "which character did this produce", read key_event::character or take a text_event instead.
///
/// Names describe the US-QWERTY legend, which is only a naming convention — the value is the position.
/// Our own vocabulary of positions, not a platform scancode number: the underlying values mean nothing outside sr.
enum class scancode : u16
{
    unknown = 0,

    // letters, by US-QWERTY legend
    a,
    b,
    c,
    d,
    e,
    f,
    g,
    h,
    i,
    j,
    k,
    l,
    m,
    n,
    o,
    p,
    q,
    r,
    s,
    t,
    u,
    v,
    w,
    x,
    y,
    z,

    // the number row
    num_0,
    num_1,
    num_2,
    num_3,
    num_4,
    num_5,
    num_6,
    num_7,
    num_8,
    num_9,

    // whitespace and editing
    enter,
    escape,
    backspace,
    tab,
    space,
    insert,
    del,
    home,
    end,
    page_up,
    page_down,

    // arrows
    left,
    right,
    up,
    down,

    // punctuation, by US-QWERTY legend
    minus,
    equals,
    left_bracket,
    right_bracket,
    backslash,
    semicolon,
    apostrophe,
    grave,
    comma,
    period,
    slash,

    // modifiers — the left and right instances are distinct positions
    left_shift,
    right_shift,
    left_ctrl,
    right_ctrl,
    left_alt,
    right_alt,
    left_super,
    right_super,

    // locks and system
    caps_lock,
    num_lock,
    scroll_lock,
    print_screen,
    pause,
    menu,

    // function row
    f1,
    f2,
    f3,
    f4,
    f5,
    f6,
    f7,
    f8,
    f9,
    f10,
    f11,
    f12,

    // keypad
    kp_0,
    kp_1,
    kp_2,
    kp_3,
    kp_4,
    kp_5,
    kp_6,
    kp_7,
    kp_8,
    kp_9,
    kp_divide,
    kp_multiply,
    kp_minus,
    kp_plus,
    kp_enter,
    kp_period,
};

enum class mouse_button : u8
{
    left,
    middle,
    right,
    x1, ///< the first extra button, usually "back"
    x2, ///< the second extra button, usually "forward"
};

/// Modifier keys held when an event was produced, as a bit set.
/// Each covers both instances — shift is set for either shift key.
enum class key_modifiers : u8
{
    none = 0,
    shift = 1 << 0,
    ctrl = 1 << 1,
    alt = 1 << 2,
    super = 1 << 3, ///< Windows key / Command
};

[[nodiscard]] constexpr key_modifiers operator|(key_modifiers a, key_modifiers b)
{
    return key_modifiers(u8(a) | u8(b));
}
[[nodiscard]] constexpr key_modifiers operator&(key_modifiers a, key_modifiers b)
{
    return key_modifiers(u8(a) & u8(b));
}
constexpr key_modifiers& operator|=(key_modifiers& a, key_modifiers b)
{
    return a = a | b;
}

/// Whether every modifier in `query` is held.
[[nodiscard]] constexpr bool has_all(key_modifiers held, key_modifiers query)
{
    return (held & query) == query;
}

/// A key going down or coming up.
/// Auto-repeat produces further is_down events with is_repeat set; there is no repeat for release.
struct key_event
{
    /// Physical position (see sr::scancode).
    /// What to test for movement and anything spatial.
    sr::scancode scancode = scancode::unknown;

    /// The character this key produces under the current layout, or 0 when it produces none.
    /// What to test for a shortcut a user would describe by letter, like ctrl+Z, which follows the layout.
    /// Not text input — a held key repeats here but composes properly only through text_event.
    ///
    /// Non-zero is not the same as printable: enter, tab, escape and backspace report the C0 control they
    /// stand for (U+000D, U+0009, U+001B, U+0008). Match those on scancode instead.
    char32_t character = 0;

    key_modifiers modifiers = key_modifiers::none;

    bool is_down = false;
    bool is_repeat = false;
};

/// Committed text, UTF-8.
/// Only delivered between window::start_text_input and stop_text_input.
///
/// One event is not one key: an IME commits a whole composed phrase at once, a dead key commits nothing until
/// the following keystroke, and a paste arrives as one event.
/// Append it to your buffer — never reconstruct text from key_events.
struct text_event
{
    cc::string text;
};

/// Cursor motion.
/// In relative mouse mode cursor_pos stops being meaningful and only delta is — see
/// window::set_relative_mouse_mode.
struct mouse_move_event
{
    /// Cursor position in pixels, relative to the window's client area.
    tg::pos2f cursor_pos;

    /// Motion since the previous event, in pixels.
    /// This is the value to drive a camera with: it stays correct when the cursor is captured or hits the screen edge.
    tg::vec2f delta;
};

struct mouse_button_event
{
    sr::mouse_button button = mouse_button::left;

    /// Modifiers as of this event's position in the stream.
    /// The platform does not stamp them onto a mouse event, so they are carried forward from the last key event,
    /// which does carry them — a shift pressed earlier in this same frame is therefore already accounted for.
    /// Re-synced when a window takes focus, since modifiers can change while another application has it.
    key_modifiers modifiers = key_modifiers::none;

    bool is_down = false;

    /// Cursor position when the button changed, in pixels relative to the window's client area.
    tg::pos2f cursor_pos;
};

/// A scroll, already corrected for the platform's scroll direction.
struct mouse_wheel_event
{
    /// Scroll amount in ticks; positive is right and away from the user.
    /// Fractional on trackpads and high-resolution wheels, so do not assume whole steps.
    tg::vec2f delta;

    /// Cursor position when the scroll happened, in pixels relative to the window's client area.
    /// Where the pointer was, not any position of the wheel itself.
    tg::pos2f cursor_pos;
};

/// One thing the user did.
///
/// Obtained from window_system::events(), oldest first, across every window in the order the OS reported them.
/// Both the span and any text inside it live until the next poll_events.
///
///     for (auto const& e : wsys->events())
///         if (auto const* k = std::get_if<sr::key_event>(&e.payload))
///             if (k->is_down && k->scancode == sr::scancode::escape)
///                 e.window->request_close();
struct input_event
{
    /// The window the event went to, or null when none had focus.
    /// Never dangles within one frame: a window destroyed mid-frame drops its events from this span.
    sr::window* window = nullptr;

    /// std::variant until cc::variant is implemented (clean-core's is a declared stub today).
    /// Switch this to cc::variant when it lands — the alternatives are the API, the holder is not.
    std::variant<key_event, text_event, mouse_move_event, mouse_button_event, mouse_wheel_event> payload;
};
} // namespace sr
