#include <SDL3/SDL.h>
#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <shaped-rendering/impl/input_translation.hh>
#include <shaped-rendering/impl/window_internals.hh>
#include <shaped-rendering/window.hh>

// The SDL-backed implementation of sr::window and sr::window_system.
// The pure SDL-to-sr mapping lives in impl/input_translation.hh so a test can reach it; everything that needs a live window or the event queue is here.
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

/// Property under which a window stashes itself on its SDL window, so an event's window id resolves back to the sr::window in one step.
/// Keeping the back-pointer on the SDL window rather than in a side table means there is one source of truth, which cannot fall out of sync with window lifetime.
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

SDL_SystemCursor as_sdl(cursor_shape shape)
{
    switch (shape)
    {
    case cursor_shape::arrow:
        return SDL_SYSTEM_CURSOR_DEFAULT;
    case cursor_shape::text:
        return SDL_SYSTEM_CURSOR_TEXT;
    case cursor_shape::wait:
        return SDL_SYSTEM_CURSOR_WAIT;
    case cursor_shape::progress:
        return SDL_SYSTEM_CURSOR_PROGRESS;
    case cursor_shape::crosshair:
        return SDL_SYSTEM_CURSOR_CROSSHAIR;
    case cursor_shape::pointer:
        return SDL_SYSTEM_CURSOR_POINTER;
    case cursor_shape::move:
        return SDL_SYSTEM_CURSOR_MOVE;
    case cursor_shape::not_allowed:
        return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
    case cursor_shape::resize_ns:
        return SDL_SYSTEM_CURSOR_NS_RESIZE;
    case cursor_shape::resize_ew:
        return SDL_SYSTEM_CURSOR_EW_RESIZE;
    case cursor_shape::resize_nesw:
        return SDL_SYSTEM_CURSOR_NESW_RESIZE;
    case cursor_shape::resize_nwse:
        return SDL_SYSTEM_CURSOR_NWSE_RESIZE;
    }
    return SDL_SYSTEM_CURSOR_DEFAULT;
}

constexpr int cursor_shape_count = int(cursor_shape::resize_nwse) + 1;

/// The SDL cursor per shape, created on first use and destroyed with the system.
///
/// File-scope rather than a window_system member because window.hh must name no SDL type,
/// and because at most one system is alive at a time (s_live_system_count) — the same reasoning that lets that counter be a global.
/// Creating each shape once matters: a UI library sets the cursor every frame from its hover state.
SDL_Cursor* s_cursors[cursor_shape_count] = {};

void destroy_cursors()
{
    for (auto& cursor : s_cursors)
    {
        if (cursor != nullptr)
            SDL_DestroyCursor(cursor);
        cursor = nullptr;
    }
}
} // namespace

u32 impl::backend_window_id(window const& w)
{
    // Walks SDL's own window list and matches on the back-pointer, so this shares the one source of truth with window_from_id rather than reaching into window's members.
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

// The setters below return void, so a platform refusal cannot be reported — it is a bug or a broken driver, not something a caller can act on.
// Assert on it and let the state we already updated stand.
// The result goes through a local: CC_ASSERT does not evaluate its condition once assertions are off,
// so calling SDL inside the macro would drop the call entirely in a release build.

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

// Both of these write the cached value through before asking SDL, so a get right after a set reads the request rather than a value only the next poll_events would refresh.
// imgui's viewport backend does exactly that within one frame.

void window::set_position(tg::pos2i position)
{
    _system->assert_owning_thread();

    _position = position;
    auto const ok = SDL_SetWindowPosition(as_sdl(_native_window), position[0], position[1]);
    CC_ASSERT(ok, "SDL_SetWindowPosition failed");
}

void window::set_size(tg::vec2i size)
{
    _system->assert_owning_thread();
    CC_ASSERT(size[0] > 0 && size[1] > 0, "window size must be positive");

    _width = size[0];
    _height = size[1];
    auto const ok = SDL_SetWindowSize(as_sdl(_native_window), size[0], size[1]);
    CC_ASSERT(ok, "SDL_SetWindowSize failed");
}

void window::focus()
{
    _system->assert_owning_thread();
    auto const ok = SDL_RaiseWindow(as_sdl(_native_window));
    CC_ASSERT(ok, "SDL_RaiseWindow failed");
}

void window::set_relative_mouse_mode(bool enabled)
{
    _system->assert_owning_thread();

    auto const ok = SDL_SetWindowRelativeMouseMode(as_sdl(_native_window), enabled);
    CC_ASSERT(ok, "SDL_SetWindowRelativeMouseMode failed");

    // Tracked from the request rather than queried back, so the getter still describes what was asked for on a platform that silently declines to capture.
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

window_system& window::system() const
{
    CC_ASSERT(_system != nullptr, "window has no system — it was not created through window_system");
    return *_system;
}

void window_system::set_cursor(cursor_shape shape)
{
    assert_owning_thread();

    if (shape == _cursor)
        return; // called every frame from a hover state, so the no-change path must not reach the platform

    _cursor = shape;

    auto& cached = s_cursors[int(shape)];
    if (cached == nullptr)
        cached = SDL_CreateSystemCursor(as_sdl(shape));

    // A shape the platform cannot provide is not an error: keep whatever is showing rather than falling back to an arrow, which would be a visible glitch on the one platform that lacks it.
    if (cached != nullptr)
        SDL_SetCursor(cached);
}

void window_system::set_cursor_visible(bool visible)
{
    assert_owning_thread();

    if (visible == _is_cursor_visible)
        return;

    _is_cursor_visible = visible;
    auto const ok = visible ? SDL_ShowCursor() : SDL_HideCursor();
    CC_ASSERT(ok, "could not change cursor visibility");
}

cc::string window_system::clipboard_text() const
{
    assert_owning_thread();

    // SDL hands back an owned copy that must be freed, and null when the clipboard holds no text.
    auto* const text = SDL_GetClipboardText();
    if (text == nullptr)
        return {};

    auto result = cc::string(text);
    SDL_free(text);
    return result;
}

void window_system::set_clipboard_text(cc::string_view text)
{
    assert_owning_thread();

    // cc::string_view is not null-terminated and SDL takes a C string.
    auto const terminated = cc::string::create_copy_c_str_materialized(text);
    auto const ok = SDL_SetClipboardText(terminated.c_str_if_terminated());
    CC_ASSERT(ok, "SDL_SetClipboardText failed");
}

bool window_system::has_clipboard_text() const
{
    assert_owning_thread();
    return SDL_HasClipboardText();
}

cc::vector<display_info> window_system::displays() const
{
    assert_owning_thread();

    auto result = cc::vector<display_info>();

    auto count = 0;
    auto* const ids = SDL_GetDisplays(&count);
    if (ids != nullptr)
    {
        for (auto i = 0; i < count; ++i)
        {
            SDL_Rect bounds = {};
            SDL_Rect usable = {};
            if (!SDL_GetDisplayBounds(ids[i], &bounds))
                continue;

            // Falls back to the full bounds rather than dropping the display: a monitor whose usable area the platform will not report is still a monitor,
            // and a zero-sized work area is worse than an approximate one.
            if (!SDL_GetDisplayUsableBounds(ids[i], &usable))
                usable = bounds;

            auto const scale = SDL_GetDisplayContentScale(ids[i]);

            result.push_back({.position = tg::pos2i(bounds.x, bounds.y),
                              .size = tg::vec2i(bounds.w, bounds.h),
                              .work_position = tg::pos2i(usable.x, usable.y),
                              .work_size = tg::vec2i(usable.w, usable.h),
                              .content_scale = scale > 0.0f ? scale : 1.0f});
        }
        SDL_free(ids);
    }

    // The dummy video driver reports no displays at all.
    // One synthetic entry keeps the postcondition — and imgui's multi-viewport sanity check, which refuses a frame with an empty monitor list.
    if (result.empty())
        result.push_back({.position = tg::pos2i(0, 0),
                          .size = tg::vec2i(1920, 1080),
                          .work_position = tg::pos2i(0, 0),
                          .work_size = tg::vec2i(1920, 1080)});

    return result;
}

cc::result<cc::unique_ptr<window_system>> window_system::try_create(window_system_description const& desc)
{
    CC_ASSERT(SDL_IsMainThread(), "sr::window_system must be created on the process main thread");
    CC_ASSERT(s_live_system_count == 0, "at most one sr::window_system may be alive at a time");

    // Set before init: the driver is chosen while the video subsystem comes up.
    // Reset rather than left alone in the normal case — the hint is process-global and outlives SDL_Quit.
    // Without the reset, one headless system would leave every later system in the process headless too.
    if (desc.headless)
    {
        auto const hint_ok = SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        CC_ASSERT(hint_ok, "could not set the SDL video-driver hint");
    }
    else
    {
        // Deliberately unchecked: SDL_ResetHint reports false when the hint was never set at all, which is the normal case in a process that has not created a headless system yet.
        // Either way the hint is now unset, which is all this path asks for — asserting here would fire on the first real window.
        (void)SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
    }

    if (!SDL_Init(SDL_INIT_VIDEO))
        return cc::error(last_sdl_error());

    // The hint is a request, so confirm it took rather than trusting it.
    // A headless system that quietly got a real driver would open real windows on a CI machine, which the assert above cannot catch in a build with assertions off.
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

window_system::~window_system()
{
    assert_owning_thread();
    CC_ASSERT(_windows.empty(), "every sr::window must be destroyed before its window_system");

    // Before SDL_Quit: the cursors are SDL objects, and freeing them after the subsystem is down is a use-after-free that only shows up under a leak checker.
    destroy_cursors();

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
    if (!desc.has_decoration)
        flags |= SDL_WINDOW_BORDERLESS;
    if (desc.is_always_on_top)
        flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    if (!desc.has_taskbar_icon)
        flags |= SDL_WINDOW_UTILITY;
    if (!desc.is_focusable)
        flags |= SDL_WINDOW_NOT_FOCUSABLE;

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

    // The size and position SDL actually gave us, which a window manager may have clamped or placed itself.
    // Queried now so the getters are meaningful before the first poll_events.
    if (!SDL_GetWindowSizeInPixels(sdl_window, &win->_width, &win->_height))
        return cc::error(last_sdl_error());

    auto position_x = 0;
    auto position_y = 0;
    if (!SDL_GetWindowPosition(sdl_window, &position_x, &position_y))
        return cc::error(last_sdl_error());
    win->_position = tg::pos2i(position_x, position_y);

    // A window shown on creation usually arrives focused, and SDL has already delivered that event by now —
    // seeding from the live flags rather than waiting for the next poll keeps the first frame honest.
    win->_is_focused = (SDL_GetWindowFlags(sdl_window) & SDL_WINDOW_INPUT_FOCUS) != 0;

    // A hard failure rather than best-effort: without the back-pointer, window_from_id never resolves this window, and it would silently never see a close or resize event again.
    if (!SDL_SetPointerProperty(props, back_pointer_property, win.get()))
        return cc::error(last_sdl_error());

    // Registered last, so every failure path above leaves nothing to unregister.
    _windows.push_back(win.get());

    return cc::move(win);
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
            // A key event carries the platform's own modifier state for that moment,
            // so remembering it here is what lets a mouse event later in this same queue be stamped with the state as of its position in the stream
            // rather than as of whenever the queue happened to be drained.
            _modifiers = impl::modifiers_from_sdl(event.key.mod);

            _events.push_back({.window = window_from_id(event.key.windowID),
                               .payload = key_event{.scancode = impl::scancode_from_sdl(event.key.scancode),
                                                    .character = impl::character_from_keycode(event.key.key),
                                                    .modifiers = _modifiers,
                                                    .is_down = event.key.down,
                                                    .is_repeat = event.key.repeat}});
            break;

        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            if (auto* const w = window_from_id(event.window.windowID))
                w->_is_focused = true;
            // Modifiers can be pressed or released while another application had focus, and those changes produce no key event here.
            // Re-sync so the first click after a window switch is not stamped with stale state.
            _modifiers = impl::modifiers_from_sdl(SDL_GetModState());
            break;

        case SDL_EVENT_WINDOW_FOCUS_LOST:
            if (auto* const w = window_from_id(event.window.windowID))
                w->_is_focused = false;
            break;

        case SDL_EVENT_TEXT_INPUT:
            // SDL owns the text only until the next pump, so it is copied rather than referenced.
            if (event.text.text != nullptr)
                _events.push_back({.window = window_from_id(event.text.windowID),
                                   .payload = text_event{.text = cc::string(event.text.text)}});
            break;

        case SDL_EVENT_MOUSE_MOTION:
            _events.push_back({.window = window_from_id(event.motion.windowID),
                               .payload = mouse_move_event{.cursor_pos = tg::pos2f(event.motion.x, event.motion.y),
                                                           .delta = tg::vec2f(event.motion.xrel, event.motion.yrel)}});
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            _events.push_back({.window = window_from_id(event.button.windowID),
                               .payload = mouse_button_event{.button = impl::mouse_button_from_sdl(event.button.button),
                                                             .modifiers = _modifiers,
                                                             .is_down = event.button.down,
                                                             .cursor_pos = tg::pos2f(event.button.x, event.button.y)}});
            break;

        case SDL_EVENT_MOUSE_WHEEL:
        {
            _events.push_back({.window = window_from_id(event.wheel.windowID),
                               .payload = mouse_wheel_event{
                                   .delta = tg::vec2f(impl::wheel_amount(event.wheel.x, event.wheel.direction),
                                                      impl::wheel_amount(event.wheel.y, event.wheel.direction)),
                                   .cursor_pos = tg::pos2f(event.wheel.mouse_x, event.wheel.mouse_y)}});
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

        case SDL_EVENT_WINDOW_MOVED:
            if (auto* const w = window_from_id(event.window.windowID))
                w->_position = tg::pos2i(event.window.data1, event.window.data2);
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
                // On failure the previous size stands, which is the best available answer here — poll_events returns void, and the next size event will correct it.
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
