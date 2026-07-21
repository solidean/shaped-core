#include <nexus/test.hh>
#include <shaped-rendering/window.hh>

// Input tests that run unattended, on SDL's dummy video driver.
//
// These cover the value types and the event-buffer lifetime, which is what sr owns outright.
//
// They do not cover the SDL-to-sr translation — the scancode table, the wheel-direction flip, the keycode to
// character rule — because reaching it means injecting real SDL events, and SDL lives in exactly one file
// (../docs/coding-guidelines.md). See ../docs/TODO.md.

TEST("sr - key modifiers combine and test as a bit set")
{
    auto const none = sr::key_modifiers::none;
    auto const ctrl = sr::key_modifiers::ctrl;
    auto const shift = sr::key_modifiers::shift;

    CHECK(has_all(ctrl | shift, ctrl));
    CHECK(has_all(ctrl | shift, shift));
    CHECK(has_all(ctrl | shift, ctrl | shift));

    CHECK(!has_all(ctrl, shift));
    CHECK(!has_all(ctrl, ctrl | shift));
    CHECK(!has_all(none, ctrl));

    // Every set contains the empty set, so a shortcut with no modifiers matches whatever is held.
    CHECK(has_all(none, none));
    CHECK(has_all(ctrl, none));

    auto accumulated = none;
    accumulated |= ctrl;
    accumulated |= shift;
    CHECK(accumulated == (ctrl | shift));
}

TEST("sr - window system creation reports whether a backend exists")
{
    // The API is here either way; only the answer changes. A caller writes this once and it compiles in both
    // builds, which is the point of not gating the types on SR_HAS_WINDOW.
    auto const created = sr::window_system::try_create({.headless = true});

#if SR_HAS_WINDOW
    CHECK(created.has_value());
#else
    CHECK(created.has_error());
#endif
}

#if SR_HAS_WINDOW

TEST("sr - a fresh window system has no events")
{
    auto const wsys = sr::window_system::create({.headless = true});
    CHECK(wsys->events().empty());

    // Draining an empty queue produces nothing rather than leaving the previous frame's span in place.
    wsys->poll_events();
    CHECK(wsys->events().empty());
}

TEST("sr - text input is off until asked for")
{
    auto const wsys = sr::window_system::create({.headless = true});
    auto const win = wsys->create_window({.title = "text"});

    // Off by default: while it is on the OS may swallow keystrokes to compose a character.
    CHECK(!win->is_text_input_active());

    win->start_text_input();
    CHECK(win->is_text_input_active());

    win->stop_text_input();
    CHECK(!win->is_text_input_active());
}

TEST("sr - relative mouse mode round-trips")
{
    auto const wsys = sr::window_system::create({.headless = true});
    auto const win = wsys->create_window({.title = "capture"});

    CHECK(!win->is_relative_mouse_mode());

    win->set_relative_mouse_mode(true);
    CHECK(win->is_relative_mouse_mode());

    win->set_relative_mouse_mode(false);
    CHECK(!win->is_relative_mouse_mode());
}

#endif // SR_HAS_WINDOW
