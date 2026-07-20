#include <SDL3/SDL.h>
#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <shaped-rendering/impl/input_translation.hh>
#include <shaped-rendering/impl/window_internals.hh>
#include <shaped-rendering/window.hh>

// The SDL-backed implementation of sr::window and sr::window_system.
// The pure SDL-to-sr mapping lives in impl/input_translation.hh so a test can reach it; everything that needs a
// live window or the event queue is here.
//
// Both types live here because they are two halves of one mechanism.
// The system's init, the window registration and the event dispatch all touch the same private state.
// Splitting them would buy nothing but an internal header to share it through.

namespace sr
{
namespace
{
/// How many window_systems are alive.
/// SDL's event queue is process-global, so a second live system would steal the first one's events.
int s_live_system_count = 0;

/// Property under which a window stashes itself on its SDL window, so an event's window id resolves back to the
/// sr::window in one step.
/// Keeping the back-pointer on the SDL window rather than in a side table means there is one source of truth,
/// which cannot fall out of sync with window lifetime.
constexpr char const* back_pointer_property = "sr.window";

SDL_Window* as_sdl(void* native_window)
{
    return static_cast<SDL_Window*>(native_window);
}

/// SDL's last error message, copied.
/// SDL reuses its message buffer, so a pointer into it cannot be stored.
cc::string last_sdl_error()
{
    return {SDL_GetError()};
}

/// The sr::window an event's window id names, or null once that window is gone.
window* window_from_id(SDL_WindowID id)
{
    auto* const sdl_window = SDL_GetWindowFromID(id);
    if (sdl_window == nullptr)
        return nullptr;

    return static_cast<window*>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(sdl_window), back_pointer_property, nullptr));
}
} // namespace

u32 impl::backend_window_id(window const& w)
{
    // Walks SDL's own window list and matches on the back-pointer, so this shares the one source of truth with
    // window_from_id rather than reaching into window's members.
    int count = 0;
    auto** const windows = SDL_GetWindows(&count);
    if (windows == nullptr)
        return 0;

    for (int i = 0; i < count; ++i)
    {
        auto* const back_pointer
            = SDL_GetPointerProperty(SDL_GetWindowProperties(windows[i]), back_pointer_property, nullptr);
        if (back_pointer == &w)
            return u32(SDL_GetWindowID(windows[i]));
    }
    return 0;
}

window::~window()
{
    if (_native_window == nullptr)
        return;

    _system->unregister_window(this);
    SDL_DestroyWindow(as_sdl(_native_window));
}

void* window::native_window_handle() const
{
#if defined(SDL_PLATFORM_WIN32)
    return SDL_GetPointerProperty(SDL_GetWindowProperties(as_sdl(_native_window)), SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                  nullptr);
#else
    // X11 wants a display plus an XID and wayland a display plus a surface — neither fits a single pointer.
    return nullptr;
#endif
}

// These three return void, so a platform refusal cannot be reported — it is a bug or a broken driver, not
// something a caller can act on. Assert on it and let the state we already updated stand.
// The result goes through a local: CC_ASSERT does not evaluate its condition once assertions are off, so
// calling SDL inside the macro would drop the call entirely in a release build.

void window::set_title(cc::string_view title)
{
    _system->assert_owning_thread();

    _title = title;
    auto const ok = SDL_SetWindowTitle(as_sdl(_native_window), _title.c_str_materialize());
    CC_ASSERT(ok, "SDL_SetWindowTitle failed");
}

void window::show()
{
    _system->assert_owning_thread();
    auto const ok = SDL_ShowWindow(as_sdl(_native_window));
    CC_ASSERT(ok, "SDL_ShowWindow failed");
}

void window::hide()
{
    _system->assert_owning_thread();
    auto const ok = SDL_HideWindow(as_sdl(_native_window));
    CC_ASSERT(ok, "SDL_HideWindow failed");
}

void window::set_relative_mouse_mode(bool enabled)
{
    _system->assert_owning_thread();

    auto const ok = SDL_SetWindowRelativeMouseMode(as_sdl(_native_window), enabled);
    CC_ASSERT(ok, "SDL_SetWindowRelativeMouseMode failed");

    // Tracked from the request rather than queried back, so the getter still describes what was asked for on a
    // platform that silently declines to capture.
    _is_relative_mouse_mode = enabled;
}

void window::start_text_input()
{
    _system->assert_owning_thread();

    auto const ok = SDL_StartTextInput(as_sdl(_native_window));
    CC_ASSERT(ok, "SDL_StartTextInput failed");
    _is_text_input_active = true;
}

void window::stop_text_input()
{
    _system->assert_owning_thread();

    auto const ok = SDL_StopTextInput(as_sdl(_native_window));
    CC_ASSERT(ok, "SDL_StopTextInput failed");
    _is_text_input_active = false;
}

cc::result<cc::unique_ptr<window_system>> window_system::try_create(window_system_description const& desc)
{
    CC_ASSERT(SDL_IsMainThread(), "sr::window_system must be created on the process main thread");
    CC_ASSERT(s_live_system_count == 0, "at most one sr::window_system may be alive at a time");

    // Set before init: the driver is chosen while the video subsystem comes up.
    // Reset rather than left alone in the normal case — the hint is process-global and outlives SDL_Quit.
    // Without the reset, one headless system would leave every later system in the process headless too.
    auto const hint_ok = desc.headless ? SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy") //
                                       : SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
    CC_ASSERT(hint_ok, "could not set the SDL video-driver hint");

    if (!SDL_Init(SDL_INIT_VIDEO))
        return cc::error(last_sdl_error());

    // The hint is a request, so confirm it took rather than trusting it.
    // A headless system that quietly got a real driver would open real windows on a CI machine, which the
    // assert above cannot catch in a build with assertions off.
    if (desc.headless)
    {
        auto const* const driver = SDL_GetCurrentVideoDriver();
        if (driver == nullptr || cc::string_view(driver) != "dummy")
        {
            SDL_Quit();
            return cc::error(cc::string("headless was requested but SDL selected the video driver ")
                             + (driver != nullptr ? driver : "<none>"));
        }
    }

    auto system = cc::make_unique<window_system>();
    system->_is_headless = desc.headless;
    system->_owning_thread_id = SDL_GetCurrentThreadID();
    system->_modifiers = impl::modifiers_from_sdl(SDL_GetModState()); // seeded once; key events maintain it after
    ++s_live_system_count;

    return cc::move(system);
}

cc::unique_ptr<window_system> window_system::create(window_system_description const& desc)
{
    return try_create(desc).or_throw();
}

window_system::~window_system()
{
    assert_owning_thread();
    CC_ASSERT(_windows.empty(), "every sr::window must be destroyed before its window_system");

    SDL_Quit();
    --s_live_system_count;
}

void window_system::assert_owning_thread() const
{
    CC_ASSERT(SDL_GetCurrentThreadID() == _owning_thread_id, "sr::window_system must be used on one thread");
}

cc::result<cc::unique_ptr<window>> window_system::try_create_window(window_description const& desc)
{
    assert_owning_thread();
    CC_ASSERT(desc.width > 0 && desc.height > 0, "window size must be positive");

    auto flags = SDL_WindowFlags(0);
    if (desc.is_resizable)
        flags |= SDL_WINDOW_RESIZABLE;
    if (!desc.is_visible)
        flags |= SDL_WINDOW_HIDDEN;

    auto title = cc::string::create_copy_c_str_materialized(desc.title);
    auto* const sdl_window = SDL_CreateWindow(title.c_str_materialize(), desc.width, desc.height, flags);
    if (sdl_window == nullptr)
        return cc::error(last_sdl_error());

    // Adopt the SDL window immediately, so every failure below unwinds through ~window and closes it.
    auto win = cc::make_unique<window>();
    win->_system = this;
    win->_native_window = sdl_window;
    win->_title = cc::move(title);

    auto const props = SDL_GetWindowProperties(sdl_window);
    if (props == 0)
        return cc::error(last_sdl_error());

    // The size SDL actually gave us, which a window manager may have clamped.
    // Queried now so the getters are meaningful before the first poll_events.
    if (!SDL_GetWindowSizeInPixels(sdl_window, &win->_width, &win->_height))
        return cc::error(last_sdl_error());

    // A hard failure rather than best-effort: without the back-pointer, window_from_id never resolves this
    // window, and it would silently never see a close or resize event again.
    if (!SDL_SetPointerProperty(props, back_pointer_property, win.get()))
        return cc::error(last_sdl_error());

    // Registered last, so every failure path above leaves nothing to unregister.
    _windows.push_back(win.get());

    return cc::move(win);
}

cc::unique_ptr<window> window_system::create_window(window_description const& desc)
{
    return try_create_window(desc).or_throw();
}

void window_system::unregister_window(window* w)
{
    assert_owning_thread();
    _windows.remove_first_value(w);

    // Anything already queued for this window would dangle for the rest of the frame, so it goes too.
    // That is what lets input_event::window be a plain pointer the caller can dereference.
    _events.remove_all_where([w](input_event const& e) { return e.window == w; });
}

void window_system::poll_events()
{
    assert_owning_thread();

    // Last frame's events die here, which is the lifetime events() documents.
    _events.clear();

    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            // A key event carries the platform's own modifier state for that moment, so remembering it here is what
            // lets a mouse event later in this same queue be stamped with the state as of its position in the
            // stream rather than as of whenever the queue happened to be drained.
            _modifiers = impl::modifiers_from_sdl(event.key.mod);

            _events.push_back({.window = window_from_id(event.key.windowID),
                               .payload = key_event{.scancode = impl::scancode_from_sdl(event.key.scancode),
                                                    .character = impl::character_from_keycode(event.key.key),
                                                    .modifiers = _modifiers,
                                                    .is_down = event.key.down,
                                                    .is_repeat = event.key.repeat}});
            break;

        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            // Modifiers can be pressed or released while another application had focus, and those changes produce
            // no key event here. Re-sync so the first click after a window switch is not stamped with stale state.
            _modifiers = impl::modifiers_from_sdl(SDL_GetModState());
            break;

        case SDL_EVENT_TEXT_INPUT:
            // SDL owns the text only until the next pump, so it is copied rather than referenced.
            if (event.text.text != nullptr)
                _events.push_back({.window = window_from_id(event.text.windowID),
                                   .payload = text_event{.text = cc::string(event.text.text)}});
            break;

        case SDL_EVENT_MOUSE_MOTION:
            _events.push_back({.window = window_from_id(event.motion.windowID),
                               .payload = mouse_move_event{.x = event.motion.x,
                                                           .y = event.motion.y,
                                                           .dx = event.motion.xrel,
                                                           .dy = event.motion.yrel}});
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            _events.push_back({.window = window_from_id(event.button.windowID),
                               .payload = mouse_button_event{.button = impl::mouse_button_from_sdl(event.button.button),
                                                             .modifiers = _modifiers,
                                                             .is_down = event.button.down,
                                                             .x = event.button.x,
                                                             .y = event.button.y}});
            break;

        case SDL_EVENT_MOUSE_WHEEL:
        {
            _events.push_back(
                {.window = window_from_id(event.wheel.windowID),
                 .payload = mouse_wheel_event{.dx = impl::wheel_amount(event.wheel.x, event.wheel.direction),
                                              .dy = impl::wheel_amount(event.wheel.y, event.wheel.direction),
                                              .x = event.wheel.mouse_x,
                                              .y = event.wheel.mouse_y}});
            break;
        }

        case SDL_EVENT_QUIT:
            _is_quit_requested = true;
            // A system quit is a request to close everything.
            // So a single-window app never has to look at is_quit_requested separately.
            for (auto* const w : _windows)
                w->_is_close_requested = true;
            break;

        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (auto* const w = window_from_id(event.window.windowID))
                w->_is_close_requested = true;
            break;

        // The pixel size, not SDL_EVENT_WINDOW_RESIZED's logical size — the back buffer is sized in pixels.
        // The two diverge as soon as a window sits on a high-density display.
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            if (auto* const w = window_from_id(event.window.windowID))
            {
                w->_width = event.window.data1;
                w->_height = event.window.data2;
            }
            break;

        case SDL_EVENT_WINDOW_MINIMIZED:
            if (auto* const w = window_from_id(event.window.windowID))
            {
                w->_is_minimized = true;
                // A minimized window has no drawable area, and SDL does not always report that as a size change.
                // So state it here rather than leaving a stale size behind.
                w->_width = 0;
                w->_height = 0;
            }
            break;

        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
            if (auto* const w = window_from_id(event.window.windowID))
            {
                w->_is_minimized = false;
                // On failure the previous size stands, which is the best available answer here — poll_events
                // returns void, and the next size event will correct it.
                auto const ok = SDL_GetWindowSizeInPixels(as_sdl(w->_native_window), &w->_width, &w->_height);
                CC_ASSERT(ok, "SDL_GetWindowSizeInPixels failed");
            }
            break;

        default:
            break;
        }
    }
}
} // namespace sr
