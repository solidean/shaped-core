#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-rendering/input.hh>

namespace sr
{
/// How a window is created.
/// Defaults describe a resizable, visible 1280x720 window.
struct window_description
{
    /// Caption shown in the title bar and the task switcher.
    /// Copied at creation; set_title changes it later.
    cc::string title = "shaped window";

    /// Client-area size in pixels; both must be > 0.
    /// This is the drawable size a swapchain matches, not the outer frame size.
    int width = 1280;
    int height = 720;

    /// Whether the user can resize the window.
    /// A resizable window changes size under you — read width()/height() every frame rather than caching them.
    bool is_resizable = true;

    /// Whether the window is mapped on creation.
    /// Create hidden to finish GPU setup before the first frame is visible, then call show().
    bool is_visible = true;
};

/// A single OS window.
///
/// Created from a window_system, which must outlive it.
/// Destroying the window closes it immediately.
/// Every method must run on the thread that created that system — see window_system.
///
/// A window does not pump its own events.
/// window_system::poll_events refreshes every live window at once, so size, minimized state and the close
/// request all read as of the last poll.
///
///     auto const win = wsys->create_window({.title = "viewer", .width = 1600, .height = 900});
///     auto const sc = ctx->create_swapchain({.native_window_handle = win->native_window_handle()});
class window
{
public:
    ~window();

    window(window const&) = delete;
    window(window&&) = delete;
    window& operator=(window const&) = delete;
    window& operator=(window&&) = delete;

    /// The OS window object, for sg::swapchain_description::native_window_handle.
    /// An HWND on Windows.
    /// Null on every other platform, and under a headless window_system — nothing can present against those.
    /// X11 and wayland each need a display plus a handle, which does not fit one pointer.
    /// sg grows a platform-tagged handle when its vulkan swapchain lands and gives that struct a second caller.
    [[nodiscard]] void* native_window_handle() const;

    /// Client-area size in pixels, as of the last poll_events.
    /// Both are 0 while minimized — skip the frame rather than sizing a swapchain against that.
    [[nodiscard]] int width() const { return _width; }
    [[nodiscard]] int height() const { return _height; }

    /// Whether the window is minimized and so has no drawable area.
    [[nodiscard]] bool is_minimized() const { return _is_minimized; }

    [[nodiscard]] cc::string_view title() const { return _title; }

    /// Whether anything has asked this window to close — the title bar's X, Alt+F4, a system quit, request_close.
    /// Latched: it stays true until clear_close_request, so a frame loop cannot miss it between polls.
    /// Closing is the caller's decision.
    /// The window stays alive and usable until it is destroyed.
    [[nodiscard]] bool is_close_requested() const { return _is_close_requested; }

    void set_title(cc::string_view title);

    void show();
    void hide();

    /// Raise the close request from code, exactly as the title bar's X would.
    /// Keeps an app-driven quit and a user-driven one on one code path.
    void request_close() { _is_close_requested = true; }

    /// Drop the close request and keep the window open, e.g. once a "discard unsaved changes?" prompt is declined.
    void clear_close_request() { _is_close_requested = false; }

    /// Capture the cursor: it is hidden, confined to this window, and stops having a position.
    /// mouse_move_event::dx/dy keep reporting motion and become unbounded, which is what an FPS-style camera wants;
    /// x/y stop being meaningful.
    /// Release it before showing any UI the user has to click.
    void set_relative_mouse_mode(bool enabled);

    [[nodiscard]] bool is_relative_mouse_mode() const { return _is_relative_mouse_mode; }

    /// Begin delivering text_events for this window, and let the platform's IME and on-screen keyboard engage.
    /// Off until asked for, because while it is on the OS may swallow keystrokes to compose a character.
    /// Turn it on around a focused text field, not for the whole app.
    void start_text_input();
    void stop_text_input();

    [[nodiscard]] bool is_text_input_active() const { return _is_text_input_active; }

private:
    /// Windows come from window_system::try_create_window, which fills in everything below.
    /// Private only keeps one off the stack — cc::make_unique still reaches it through the friend below.
    /// So that origin is a convention, not something the type enforces.
    /// A window built any other way has no system and null-derefs on first use.
    window() = default;

    friend class window_system;
    friend struct cc::node_allocation<window>; // cc::make_unique constructs through it

    window_system* _system = nullptr;

    /// The backend's window object, opaque here because its type is not part of this API.
    void* _native_window = nullptr;

    cc::string _title;
    int _width = 0;
    int _height = 0;
    bool _is_minimized = false;
    bool _is_close_requested = false;
    bool _is_relative_mouse_mode = false;
    bool _is_text_input_active = false;
};

/// How a window_system is created.
struct window_system_description
{
    /// Run without a display: windows are created and tracked as usual but never appear.
    /// Their native handles stay null, so nothing can present.
    /// This is what lets a window test run unattended.
    bool headless = false;
};

/// The window subsystem: owns the platform-level init and shutdown and the event pump, and creates windows.
///
/// Main-thread bound.
/// Create it, create windows from it, poll it and destroy it all on one thread.
/// On macOS that thread must be the process main thread.
/// Violations assert.
///
/// At most one may be alive per process — the OS event queue is process-global, so two would steal each other's
/// events.
///
/// It must outlive every window created from it.
/// Windows are owned by the caller and may be created and destroyed freely between polls.
/// The system holds only non-owning references, enough to route events.
///
///     auto const wsys = sr::window_system::create();
///     auto const win = wsys->create_window({.title = "viewer"});
///     while (!win->is_close_requested())
///     {
///         wsys->poll_events();
///         // ... render and present ...
///     }
class window_system
{
public:
    /// Brings the window subsystem up, or fails with the reason.
    /// Fails when no display is available and headless was not requested.
    ///
    /// Also fails when shaped-rendering was built without a window backend, which is the only thing
    /// SR_HAS_WINDOW changes — the API is always here, so code compiles either way and finds out now.
    /// Check this result rather than the macro unless you need the answer at compile time.
    [[nodiscard]] static cc::result<cc::unique_ptr<window_system>> try_create(window_system_description const& desc = {});

    /// Throwing counterpart of try_create.
    [[nodiscard]] static cc::unique_ptr<window_system> create(window_system_description const& desc = {});

    /// Shuts the subsystem down.
    /// Every window created from it must already be destroyed.
    ~window_system();

    window_system(window_system const&) = delete;
    window_system(window_system&&) = delete;
    window_system& operator=(window_system const&) = delete;
    window_system& operator=(window_system&&) = delete;

    /// Opens a window, or fails with the platform's reason.
    /// The returned window is owned by the caller and must not outlive this system.
    [[nodiscard]] cc::result<cc::unique_ptr<window>> try_create_window(window_description const& desc = {});

    /// Throwing counterpart of try_create_window.
    [[nodiscard]] cc::unique_ptr<window> create_window(window_description const& desc = {});

    /// Every window currently alive, in creation order.
    /// Non-owning — an element dangles as soon as its owner destroys it.
    [[nodiscard]] cc::span<window* const> windows() const { return _windows; }

    /// Drains the OS event queue once and refreshes every live window's size, minimized state and close request.
    /// Call it once per frame before rendering — nothing else advances a window's state.
    /// An unpumped window is one the OS considers hung.
    /// Events naming a window that no longer exists are discarded.
    void poll_events();

    /// What the user did during the last poll_events, oldest first (see sr::input_event).
    /// One stream across every window, in the order the OS reported them, each event naming the window it went to.
    /// Invalidated by the next poll_events, text included — copy anything you need to keep.
    [[nodiscard]] cc::span<input_event const> events() const { return _events; }

    /// Whether the OS asked the application as a whole to quit — a session logout, the platform's quit gesture.
    /// Latched, like a window's close request.
    /// A quit also raises the close request on every live window, so a single-window app can ignore this.
    [[nodiscard]] bool is_quit_requested() const { return _is_quit_requested; }

    void clear_quit_request() { _is_quit_requested = false; }

    /// Whether this system was created headless (see window_system_description).
    [[nodiscard]] bool is_headless() const { return _is_headless; }

private:
    /// Systems come from try_create, which brings the platform up first.
    /// Private the same way window is, and with the same caveat.
    window_system() = default;

    friend class window;
    friend struct cc::node_allocation<window_system>; // cc::make_unique constructs through it

    /// Drops a window from the dispatch table. Called from ~window.
    void unregister_window(window* w);

    /// Asserts the calling thread is the one this system was created on.
    void assert_owning_thread() const;

    cc::vector<window*> _windows;

    /// This frame's events, cleared at the top of every poll_events and handed out by events().
    cc::vector<input_event> _events;

    /// Modifiers as of the last key event seen, carried across polls.
    /// Mouse events are stamped from this rather than from a live query, so a click reads the state as of its own
    /// position in the event stream. See mouse_button_event::modifiers.
    key_modifiers _modifiers = key_modifiers::none;

    u64 _owning_thread_id = 0;
    bool _is_headless = false;
    bool _is_quit_requested = false;
};
} // namespace sr
