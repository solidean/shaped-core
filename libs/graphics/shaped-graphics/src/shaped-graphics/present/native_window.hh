#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/types.hh>
#include <typed-geometry/linalg/vec.hh> // tg::vec2i (client_size)

/// Which windowing system a `native_window`'s fields belong to.
/// It decides which of them are meaningful, and which surface-creation call a backend makes.
enum class sg::window_platform : sg::u8
{
    win32,   ///< `handle` is an HWND
    xlib,    ///< `display` is a Display*, `window_id` an X11 Window
    xcb,     ///< `display` is an xcb_connection_t*, `window_id` an xcb_window_t
    wayland, ///< `display` is a wl_display*, `handle` a wl_surface*
};

/// An OS window, named in the terms its windowing system uses.
///
/// **Why this is not just a `void*`.** It was, and an HWND fits one — but X11 needs a `Display*` **and** a `Window`
/// XID, and wayland a `wl_display*` **and** a `wl_surface*`.
/// Neither fits a single pointer, so a `void*` handle silently limited windowed presentation to Windows.
///
/// Flat and trivially copyable on purpose: sg core must name this without including any platform header, which is
/// what a variant or an opaque pointer-to-implementation would cost.
/// The unused fields for a platform stay null; nothing reads them.
///
/// `sr::window::native_window()` is the supported producer.
/// A backend that cannot present to a given platform reports it rather than guessing — see
/// libs/graphics/shaped-graphics/docs/concepts/presentation.md.
struct sg::native_window
{
    window_platform platform = window_platform::win32;

    /// The connection: `Display*` (xlib), `xcb_connection_t*` (xcb), `wl_display*` (wayland).
    /// Null on win32, which has none.
    void* display = nullptr;

    /// The window itself where it is a pointer: `HWND` (win32), `wl_surface*` (wayland).
    /// Null on xlib and xcb, whose windows are integers rather than pointers — see `window_id`.
    void* handle = nullptr;

    /// The window itself where it is an integer: `Window` (xlib), `xcb_window_t` (xcb).
    /// Zero elsewhere.
    u64 window_id = 0;

    /// Client-area size in pixels.
    ///
    /// **Required on wayland**, where a surface has no size of its own: the application is the authority and the
    /// compositor takes whatever size the swapchain is built at.
    /// A wayland chain built without it comes up 1x1.
    /// Ignored on win32 and X11, whose surfaces report their own extent — a backend asks the surface there.
    ///
    /// This is the size at creation only; a resized window feeds `sg::swapchain::set_window_size`.
    tg::vec2i client_size = tg::vec2i(0, 0);

    /// Whether this names a window at all, by its own platform's rules.
    /// A default-constructed one does not, which is what makes "no window backend" representable.
    [[nodiscard]] bool is_valid() const
    {
        switch (platform)
        {
        case window_platform::win32:
            return handle != nullptr;
        case window_platform::xlib:
        case window_platform::xcb:
            return display != nullptr && window_id != 0;
        case window_platform::wayland:
            return display != nullptr && handle != nullptr;
        }
        return false;
    }

    /// A win32 window from its HWND — the one platform whose window is a single pointer.
    [[nodiscard]] static native_window from_win32(void* hwnd)
    {
        return {.platform = window_platform::win32, .handle = hwnd};
    }
};
