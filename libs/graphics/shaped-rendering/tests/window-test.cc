#include <nexus/test.hh>
#include <shaped-rendering/window.hh>

// Window tests that run unattended, on SDL's dummy video driver (window_system_description::headless).
//
// These cover the object graph and the bookkeeping: creation, ownership, the registration table, the per-window
// close latch.
// That is most of what can break in this layer.
//
// They cannot cover anything that needs a display.
// Close and resize events from a real window manager, pixel sizes on a high-density display, and the
// native-handle-to-swapchain path are all out of reach — a headless window has no native handle.
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

#endif // SR_HAS_WINDOW
