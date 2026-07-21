#include <clean-core/common/macros.hh> // CC_OS_WINDOWS
#include <clean-core/string/print.hh>
#include <nexus/test.hh>
#include <shaped-rendering/window.hh>

// The counterpart to window-test.cc: what a dummy video driver cannot reach.
// A real native handle, and a real window manager delivering close and resize events.
// Both need a display, so both are manual.
//
//     uv run dev.py test "sr - window native handle (manual)" --manual
//     uv run dev.py test "sr - window (manual)" --manual --mirror-test-output
//
// The first needs only a display and finishes on its own.
// The second opens a visible window and runs until someone closes it.

#if SR_HAS_WINDOW

TEST("sr - window native handle (manual)", nx::config::manual)
{
    // The claim the whole abstraction rests on: a real window yields the handle sg::swapchain_description wants.
    // Created hidden, so it needs a display but no person.
    auto const wsys = sr::window_system::create();
    auto const win = wsys->create_window({.title = "handle probe", .width = 320, .height = 240, .is_visible = false});

    CHECK(!wsys->is_headless());
    CHECK(win->width() == 320);
    CHECK(win->height() == 240);

#ifdef CC_OS_WINDOWS
    CHECK(win->native_window_handle() != nullptr);
#endif
}

TEST("sr - window (manual)", nx::config::manual)
{
    auto const wsys = sr::window_system::create();
    auto const win = wsys->create_window({.title = "shaped-rendering — close this window to end the test"});

    cc::println("opened a {}x{} window; native handle {}", win->width(), win->height(),
                win->native_window_handle() != nullptr ? "present" : "null");

    auto last_width = win->width();
    auto last_height = win->height();

    while (!win->is_close_requested())
    {
        wsys->poll_events();

        if (win->width() != last_width || win->height() != last_height)
        {
            last_width = win->width();
            last_height = win->height();
            cc::println("resized to {}x{}{}", last_width, last_height, win->is_minimized() ? " (minimized)" : "");
        }
    }

    cc::println("close requested — shutting down");
}

#endif // SR_HAS_WINDOW
