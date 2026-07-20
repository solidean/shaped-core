#include <SDL3/SDL.h>
#include <nexus/test.hh>
#include <shaped-rendering/impl/window_internals.hh>
#include <shaped-rendering/window.hh>

// Event dispatch, driven by pushing synthetic SDL events through a live (headless) window system.
//
// Reaches into impl/ and sees SDL, like input-translation-test.cc — see ../docs/coding-guidelines.md.
// Everything here is about routing and state: which window an event lands on, what it changes, and how long the
// resulting span stays valid.
// The mapping itself is covered in input-translation-test.cc; this is the layer above it.
//
// Needs no display: the dummy video driver has a real event queue.

using namespace sr;

namespace
{
/// Pushes one event and drains it, so each check reads the state produced by exactly that event.
void push(SDL_Event& event)
{
    REQUIRE(SDL_PushEvent(&event));
}

SDL_Event key_down(u32 window_id, SDL_Scancode scancode, SDL_Keymod mod = SDL_KMOD_NONE)
{
    SDL_Event e = {};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = SDL_WindowID(window_id);
    e.key.scancode = scancode;
    e.key.key = SDL_GetKeyFromScancode(scancode, mod, false);
    e.key.mod = mod;
    e.key.down = true;
    return e;
}

SDL_Event window_event(u32 window_id, SDL_EventType type, i32 data1 = 0, i32 data2 = 0)
{
    SDL_Event e = {};
    e.type = type;
    e.window.windowID = SDL_WindowID(window_id);
    e.window.data1 = data1;
    e.window.data2 = data2;
    return e;
}
} // namespace

TEST("sr - an event is routed to the window it names")
{
    auto const wsys = sr::window_system::create({.headless = true});
    auto const a = wsys->create_window({.title = "a"});
    auto const b = wsys->create_window({.title = "b"});

    auto const id_a = impl::backend_window_id(*a);
    auto const id_b = impl::backend_window_id(*b);
    REQUIRE(id_a != 0);
    REQUIRE(id_b != 0);
    REQUIRE(id_a != id_b);

    auto e = key_down(id_b, SDL_SCANCODE_K);
    push(e);
    wsys->poll_events();

    REQUIRE(wsys->events().size() == 1);
    CHECK(wsys->events()[0].window == b.get());
    CHECK(wsys->events()[0].window != a.get());

    auto const* const k = std::get_if<sr::key_event>(&wsys->events()[0].payload);
    REQUIRE(k != nullptr);
    CHECK(k->scancode == scancode::k);
    CHECK(k->is_down);
}

TEST("sr - events keep their order across windows")
{
    auto const wsys = sr::window_system::create({.headless = true});
    auto const a = wsys->create_window({.title = "a"});
    auto const b = wsys->create_window({.title = "b"});

    auto const id_a = impl::backend_window_id(*a);
    auto const id_b = impl::backend_window_id(*b);

    // Interleaved across two windows: one stream, in the order they happened.
    auto e0 = key_down(id_a, SDL_SCANCODE_1);
    auto e1 = key_down(id_b, SDL_SCANCODE_2);
    auto e2 = key_down(id_a, SDL_SCANCODE_3);
    push(e0);
    push(e1);
    push(e2);
    wsys->poll_events();

    REQUIRE(wsys->events().size() == 3);
    CHECK(wsys->events()[0].window == a.get());
    CHECK(wsys->events()[1].window == b.get());
    CHECK(wsys->events()[2].window == a.get());

    CHECK(std::get<sr::key_event>(wsys->events()[0].payload).scancode == scancode::num_1);
    CHECK(std::get<sr::key_event>(wsys->events()[1].payload).scancode == scancode::num_2);
    CHECK(std::get<sr::key_event>(wsys->events()[2].payload).scancode == scancode::num_3);
}

TEST("sr - each poll replaces the previous frame's events")
{
    auto const wsys = sr::window_system::create({.headless = true});
    auto const win = wsys->create_window({.title = "w"});
    auto const id = impl::backend_window_id(*win);

    auto e = key_down(id, SDL_SCANCODE_A);
    push(e);
    wsys->poll_events();
    CHECK(wsys->events().size() == 1);

    // A poll with nothing queued clears rather than leaving the last frame's span in place.
    wsys->poll_events();
    CHECK(wsys->events().empty());
}

TEST("sr - modifiers carry forward from key events onto mouse events")
{
    auto const wsys = sr::window_system::create({.headless = true});
    auto const win = wsys->create_window({.title = "w"});
    auto const id = impl::backend_window_id(*win);

    // The platform does not stamp modifiers onto a mouse event, so the shift press earlier in this same queue is
    // what has to supply them.
    auto shift = key_down(id, SDL_SCANCODE_LSHIFT, SDL_KMOD_LSHIFT);
    SDL_Event click = {};
    click.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    click.button.windowID = SDL_WindowID(id);
    click.button.button = SDL_BUTTON_LEFT;
    click.button.down = true;
    click.button.x = 12.0f;
    click.button.y = 34.0f;

    push(shift);
    push(click);
    wsys->poll_events();

    REQUIRE(wsys->events().size() == 2);
    auto const* const b = std::get_if<sr::mouse_button_event>(&wsys->events()[1].payload);
    REQUIRE(b != nullptr);
    CHECK(has_all(b->modifiers, key_modifiers::shift));
    CHECK(b->button == mouse_button::left);
    CHECK(b->is_down);
    CHECK(b->x == 12.0f);
    CHECK(b->y == 34.0f);
}

TEST("sr - committed text arrives as its own event, copied")
{
    auto const wsys = sr::window_system::create({.headless = true});
    auto const win = wsys->create_window({.title = "w"});

    // Multi-byte on purpose: text is UTF-8 and one event is a whole commit, not one character.
    char text[] = "héllo";

    SDL_Event e = {};
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.windowID = SDL_WindowID(impl::backend_window_id(*win));
    e.text.text = text;
    push(e);
    wsys->poll_events();

    REQUIRE(wsys->events().size() == 1);
    auto const* const t = std::get_if<sr::text_event>(&wsys->events()[0].payload);
    REQUIRE(t != nullptr);
    CHECK(t->text == "héllo");

    // Mutated after the drain: the event owns its text, rather than aliasing the buffer SDL pointed at.
    // SDL's own text events live only until the next pump, so aliasing would dangle within one frame.
    text[0] = 'X';
    CHECK(t->text == "héllo");
}

TEST("sr - a close request lands only on the window that got it")
{
    auto const wsys = sr::window_system::create({.headless = true});
    auto const a = wsys->create_window({.title = "a"});
    auto const b = wsys->create_window({.title = "b"});

    auto e = window_event(impl::backend_window_id(*b), SDL_EVENT_WINDOW_CLOSE_REQUESTED);
    push(e);
    wsys->poll_events();

    CHECK(b->is_close_requested());
    CHECK(!a->is_close_requested());
}

TEST("sr - a quit closes every window")
{
    auto const wsys = sr::window_system::create({.headless = true});
    auto const a = wsys->create_window({.title = "a"});
    auto const b = wsys->create_window({.title = "b"});

    SDL_Event e = {};
    e.type = SDL_EVENT_QUIT;
    push(e);
    wsys->poll_events();

    CHECK(wsys->is_quit_requested());
    CHECK(a->is_close_requested());
    CHECK(b->is_close_requested());

    wsys->clear_quit_request();
    CHECK(!wsys->is_quit_requested());
}

TEST("sr - a pixel size change updates that window's size")
{
    auto const wsys = sr::window_system::create({.headless = true});
    auto const win = wsys->create_window({.title = "w", .width = 800, .height = 600});
    REQUIRE(win->width() == 800);

    auto e = window_event(impl::backend_window_id(*win), SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, 1024, 768);
    push(e);
    wsys->poll_events();

    CHECK(win->width() == 1024);
    CHECK(win->height() == 768);
}

TEST("sr - minimizing reports no drawable area")
{
    auto const wsys = sr::window_system::create({.headless = true});
    auto const win = wsys->create_window({.title = "w", .width = 800, .height = 600});

    auto minimized = window_event(impl::backend_window_id(*win), SDL_EVENT_WINDOW_MINIMIZED);
    push(minimized);
    wsys->poll_events();

    // Both zero, so a renderer that skips on is_minimized never sizes a swapchain against a stale extent.
    CHECK(win->is_minimized());
    CHECK(win->width() == 0);
    CHECK(win->height() == 0);
}

TEST("sr - destroying a window drops its queued events")
{
    auto const wsys = sr::window_system::create({.headless = true});
    auto const a = wsys->create_window({.title = "a"});
    auto b = wsys->create_window({.title = "b"});

    auto e0 = key_down(impl::backend_window_id(*a), SDL_SCANCODE_A);
    auto e1 = key_down(impl::backend_window_id(*b), SDL_SCANCODE_B);
    push(e0);
    push(e1);
    wsys->poll_events();
    REQUIRE(wsys->events().size() == 2);

    // This is what lets input_event::window be a plain pointer a caller may dereference: b's event goes with b,
    // rather than being left behind pointing at freed memory.
    b = nullptr;

    REQUIRE(wsys->events().size() == 1);
    CHECK(wsys->events()[0].window == a.get());
}
