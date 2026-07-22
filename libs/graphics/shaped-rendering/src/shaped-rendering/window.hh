#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-rendering/input.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

namespace sr
{
/// The shape the mouse pointer is drawn as.
///
/// One cursor is showing at a time for the whole process, so this is set on the window_system rather than on a
/// window — see window_system::set_cursor.
///
/// Every shape here is one the platform provides. A shape the platform lacks falls back to the closest one it
/// has, which is the platform's business, not ours; nothing here fails.
enum class cursor_shape : u8
{
    arrow,       ///< the default pointer
    text,        ///< an I-beam, over editable text
    wait,        ///< busy and not interactive — an hourglass or spinner
    progress,    ///< busy but still interactive — usually the arrow with a spinner
    crosshair,   ///< precise selection
    pointer,     ///< a pointing hand, over something that acts like a link
    move,        ///< four-way arrow, for dragging a whole object
    not_allowed, ///< the action is refused here — usually a slashed circle
    resize_ns,   ///< over a horizontal edge
    resize_ew,   ///< over a vertical edge
    resize_nesw, ///< over the bottom-left or top-right corner
    resize_nwse, ///< over the bottom-right or top-left corner
};

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

    /// Whether the window has a title bar and a frame.
    /// A UI library that draws its own chrome — imgui's multi-viewport windows, a tooltip, a popup — wants
    /// this off, and then owns dragging and resizing itself.
    bool has_decoration = true;

    /// Whether the window stays above ordinary windows.
    /// For a popup or a tooltip that must not be buried by the window it belongs to.
    bool is_always_on_top = false;

    /// Whether the window appears in the task bar and the task switcher.
    /// Off for a window that is part of another window's UI rather than a place the user switches to.
    bool has_taskbar_icon = true;

    /// Whether the window may take the keyboard focus when it is shown.
    /// Off for a tooltip or a menu, which must appear without stealing focus from what the user is typing in.
    bool is_focusable = true;
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

    /// Top-left of the client area in desktop coordinates, as of the last poll_events or the last set_position.
    /// Multi-monitor desktops put origins wherever they like, so a coordinate may be negative.
    [[nodiscard]] tg::pos2i position() const { return _position; }

    /// Moves the window so its client area starts at `position`.
    /// Takes effect immediately, and position() reads back the new value without waiting for a poll.
    /// The window manager may refuse or adjust the move — the next poll_events reports where it actually landed.
    void set_position(tg::pos2i position);

    /// Resizes the client area; both components must be > 0.
    /// Like set_position, this is write-through: width()/height() read back the request at once, and the next
    /// poll_events reports what the window manager granted.
    void set_size(tg::vec2i size);

    /// Whether the window is minimized and so has no drawable area.
    [[nodiscard]] bool is_minimized() const { return _is_minimized; }

    /// Whether this window has the keyboard focus, as of the last poll_events.
    /// At most one window in the process has it, and none does while another application is in front.
    [[nodiscard]] bool is_focused() const { return _is_focused; }

    /// Asks the window manager to raise this window and give it the keyboard focus.
    /// A request, not a guarantee — a window manager may refuse to steal focus, so read is_focused after the
    /// next poll_events rather than assuming it took.
    void focus();

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

    /// The system this window came from, and which must outlive it.
    /// Saves threading a window_system& alongside a window& through code that already has one.
    [[nodiscard]] window_system& system() const;

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
    tg::pos2i _position = tg::pos2i(0, 0);
    bool _is_minimized = false;
    bool _is_focused = false;
    bool _is_close_requested = false;
    bool _is_relative_mouse_mode = false;
    bool _is_text_input_active = false;
};

/// One monitor attached to the desktop, as of the last poll_events.
///
/// All coordinates are in the same desktop space as window::position, so a window is on the display whose
/// bounds contain it. A multi-monitor desktop puts origins wherever the user arranged them, which is why a
/// coordinate here may be negative.
struct display_info
{
    /// The display's full rectangle.
    tg::pos2i position;
    tg::vec2i size;

    /// The part not covered by task bars, docks and other permanent desktop furniture.
    /// This is where a window should open; `size` is what a fullscreen window covers.
    tg::pos2i work_position;
    tg::vec2i work_size;

    /// Pixels per logical unit — 1 on a standard-density display, 2 on a typical HiDPI one.
    float content_scale = 1.0f;
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

    /// Every monitor attached to the desktop, primary first, queried fresh on each call.
    /// Never empty while a display is available; a headless system reports one synthetic display, so code
    /// that places windows against a monitor has something well-formed to work with either way.
    [[nodiscard]] cc::vector<display_info> displays() const;

    // Mouse cursor.
    //
    // Process-global, not per-window: one cursor is showing at a time, whichever window the pointer is over.
    // Cheap to set every frame — the platform cursor is only touched when the shape actually changes, which is
    // what a UI library driving this from its hover state needs.

    void set_cursor(cursor_shape shape);
    [[nodiscard]] cursor_shape cursor() const { return _cursor; }

    /// Whether the pointer is drawn at all. Independent of its shape, so hiding and showing it again restores
    /// the shape that was set. Note window::set_relative_mouse_mode hides it too, for as long as it is on.
    void set_cursor_visible(bool visible);
    [[nodiscard]] bool is_cursor_visible() const { return _is_cursor_visible; }

    // System clipboard.
    //
    // Process-global and shared with every other application, so treat a read as untrusted input of unbounded
    // size. Text only; images and files are not modelled.

    /// The clipboard's text, or empty when it holds none (including when it holds something that is not text).
    /// Returns a copy — the platform's buffer is not ours to hold.
    [[nodiscard]] cc::string clipboard_text() const;

    void set_clipboard_text(cc::string_view text);

    [[nodiscard]] bool has_clipboard_text() const;

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

    /// The cursor as last set. Tracked so set_cursor can skip the platform call when nothing changed, and so
    /// hiding and showing the pointer restores the shape rather than resetting it to an arrow.
    cursor_shape _cursor = cursor_shape::arrow;
    bool _is_cursor_visible = true;

    u64 _owning_thread_id = 0;
    bool _is_headless = false;
    bool _is_quit_requested = false;
};
} // namespace sr
