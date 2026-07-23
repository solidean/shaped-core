#include <nexus/test.hh>
#include <shaped-rendering/window.hh>

// Window tests that run unattended, on SDL's dummy video driver (window_system_description::headless).
//
// These cover the object graph and the bookkeeping: creation, ownership, the registration table, the per-window close latch.
// That is most of what can break in this layer.
//
// They cannot cover anything that needs a display.
// Close and resize events from a real window manager, pixel sizes on a high-density display, and the native-handle-to-swapchain path are all out of reach — a headless window has no native handle.
// Those live in window-manual-test.cc and, for the swapchain, above sr.

#if SR_HAS_WINDOW

TEST("sr - window system creates and shuts down")
{
    auto const wsys = sr::window_system::create({.headless = true});
    CHECK(wsys->is_headless());
    CHECK(wsys->windows().empty());
    CHECK(!wsys->is_quit_requested());
}

TEST("sr - window reports its requested size before any poll")
{
    auto const wsys = sr::window_system::create({.headless = true});
    auto const win = wsys->create_window({.title = "sized", .width = 640, .height = 480});

    CHECK(win->width() == 640);
    CHECK(win->height() == 480);
    CHECK(!win->is_minimized());
    CHECK(!win->is_close_requested());
}

TEST("sr - window position and size read back without an intervening poll")
{
    // The write-through is the point: imgui's viewport backend sets a position and reads it again inside one frame, long before the next poll_events would refresh it.
    auto const wsys = sr::window_system::create({.headless = true});
    auto const win = wsys->create_window({.title = "placed", .width = 640, .height = 480});

    win->set_position(tg::pos2i(120, -40));
    CHECK(win->position() == tg::pos2i(120, -40)); // a negative coordinate is a legal desktop position

    win->set_size(tg::vec2i(800, 600));
    CHECK(win->width() == 800);
    CHECK(win->height() == 600);
}

TEST("sr - the display list is never empty and its work area fits inside its bounds")
{
    // imgui's multi-viewport path refuses a frame outright while the monitor list is empty, and the dummy
    // video driver reports no displays at all — so a headless system substitutes one.
    auto const wsys = sr::window_system::create({.headless = true});

    auto const displays = wsys->displays();
    REQUIRE(!displays.empty());

    for (auto const& d : displays)
    {
        CHECK(d.size[0] > 0);
        CHECK(d.size[1] > 0);
        CHECK(d.content_scale > 0.0f);

        // The usable area is what a window should open into, so it can never be larger than the monitor.
        CHECK(d.work_position[0] >= d.position[0]);
        CHECK(d.work_position[1] >= d.position[1]);
        CHECK(d.work_position[0] + d.work_size[0] <= d.position[0] + d.size[0]);
        CHECK(d.work_position[1] + d.work_size[1] <= d.position[1] + d.size[1]);
    }
}

TEST("sr - windows register in creation order and unregister on destruction")
{
    auto const wsys = sr::window_system::create({.headless = true});

    auto a = wsys->create_window({.title = "a"});
    auto b = wsys->create_window({.title = "b"});
    auto c = wsys->create_window({.title = "c"});

    REQUIRE(wsys->windows().size() == 3);
    CHECK(wsys->windows()[0] == a.get());
    CHECK(wsys->windows()[1] == b.get());
    CHECK(wsys->windows()[2] == c.get());

    // Destroying out of creation order must leave the survivors intact and in order.
    b = nullptr;

    REQUIRE(wsys->windows().size() == 2);
    CHECK(wsys->windows()[0] == a.get());
    CHECK(wsys->windows()[1] == c.get());
}

TEST("sr - a close request is per window")
{
    auto const wsys = sr::window_system::create({.headless = true});
    auto const a = wsys->create_window({.title = "a"});
    auto const b = wsys->create_window({.title = "b"});

    b->request_close();

    CHECK(b->is_close_requested());
    CHECK(!a->is_close_requested());

    // Latched: draining an empty queue must not drop a request the loop has not seen yet.
    wsys->poll_events();
    CHECK(b->is_close_requested());
    CHECK(!a->is_close_requested());

    b->clear_close_request();
    CHECK(!b->is_close_requested());
}

TEST("sr - window title round-trips")
{
    auto const wsys = sr::window_system::create({.headless = true});
    auto const win = wsys->create_window({.title = "before"});
    CHECK(win->title() == "before");

    win->set_title("after");
    CHECK(win->title() == "after");
}

TEST("sr - a headless window has no native handle")
{
    // Pins the documented contract rather than skipping it.
    // Nothing can present against a headless window, and a caller must see that from the handle alone.
    auto const wsys = sr::window_system::create({.headless = true});
    auto const win = wsys->create_window({.title = "handleless"});

    CHECK(win->native_window_handle() == nullptr);
}

TEST("sr - a second window system asserts")
{
    auto const wsys = sr::window_system::create({.headless = true});
    CHECK_ASSERTS(sr::window_system::create({.headless = true}));
}

TEST("sr - a window knows the system it came from")
{
    auto const wsys = sr::window_system::create({.headless = true});
    auto const win = wsys->create_window({.title = "owned"});

    CHECK(&win->system() == wsys.get());
}

TEST("sr - the cursor shape is tracked and starts as an arrow")
{
    // The dummy video driver has no real pointer, so what is checkable here is the bookkeeping: the shape a
    // caller set is the shape it reads back, and setting the same one twice is not an error.
    // Whether the OS actually draws it needs a display, which is the manual bucket's job.
    auto const wsys = sr::window_system::create({.headless = true});

    CHECK(wsys->cursor() == sr::cursor_shape::arrow);
    CHECK(wsys->is_cursor_visible());

    wsys->set_cursor(sr::cursor_shape::text);
    CHECK(wsys->cursor() == sr::cursor_shape::text);

    wsys->set_cursor(sr::cursor_shape::text); // the every-frame case: a no-op, not a re-set
    CHECK(wsys->cursor() == sr::cursor_shape::text);

    wsys->set_cursor(sr::cursor_shape::resize_nwse);
    CHECK(wsys->cursor() == sr::cursor_shape::resize_nwse);
}

TEST("sr - hiding the cursor leaves its shape alone")
{
    // Visibility and shape are independent, so showing the pointer again must restore what was set rather than
    // resetting it to an arrow.
    auto const wsys = sr::window_system::create({.headless = true});

    wsys->set_cursor(sr::cursor_shape::pointer);
    wsys->set_cursor_visible(false);

    CHECK(!wsys->is_cursor_visible());
    CHECK(wsys->cursor() == sr::cursor_shape::pointer);

    wsys->set_cursor_visible(true);
    CHECK(wsys->is_cursor_visible());
    CHECK(wsys->cursor() == sr::cursor_shape::pointer);
}

TEST("sr - clipboard text round-trips")
{
    // The clipboard is real even under the dummy driver: SDL keeps its own when the platform has none.
    auto const wsys = sr::window_system::create({.headless = true});

    wsys->set_clipboard_text("shaped");
    CHECK(wsys->has_clipboard_text());
    CHECK(wsys->clipboard_text() == "shaped");

    // A cc::string_view is not null-terminated, so a substring is the case that catches a missing
    // materialization: the wrong length would carry the rest of the source string with it.
    auto const source = cc::string("shaped-core");
    wsys->set_clipboard_text(source.subview({.offset = 0, .size = 6}));
    CHECK(wsys->clipboard_text() == "shaped");

    // Empty is a legitimate value, not an error.
    wsys->set_clipboard_text("");
    CHECK(wsys->clipboard_text() == "");
}

#endif // SR_HAS_WINDOW
