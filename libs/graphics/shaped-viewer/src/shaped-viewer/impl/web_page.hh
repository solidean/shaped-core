#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/async.hh>
#include <shaped-rendering/input.hh>
#include <typed-geometry/linalg/vec.hh>

// The browser page an `sv::interactive_async` loop runs in: its `#canvas`, the canvas's input, and the shell's
// loading overlay.
// Everything here answers "no page" natively and under node, so a caller asks `page_present` once and needs no `#if`.
// Main thread only: the page's listeners and the frame loop share it.
namespace sv::impl
{
/// Whether this runs in a browser page, which has a document; false natively and under node.
[[nodiscard]] bool page_present();

/// The CSS selector of the canvas the viewer presents into.
inline constexpr char const* page_canvas_selector = "#canvas";

/// Starts translating the canvas's pointer, wheel and key events into `sr::input_event`s, and sizes its drawing buffer
/// to its layout at the screen's density from then on.
/// Once per process; a second call does nothing.
void install_page_input();

/// The canvas's drawing-buffer size; (0, 0) without a page.
[[nodiscard]] tg::vec2i page_canvas_size();

/// The canvas's events since the last call, oldest first.
[[nodiscard]] cc::vector<sr::input_event> take_page_input();

/// The size the canvas's drawing buffer changed to since the last call, if it did.
[[nodiscard]] cc::optional<tg::vec2i> take_page_resize();

/// Settles at the browser's next frame callback, so a page presents once per display refresh rather than as often as
/// the GPU allows; settled at once without a page.
[[nodiscard]] cc::shared_async<cc::unit> next_animation_frame();

/// What the page's loading overlay says while there is nothing to see yet.
void set_page_status(cc::string_view text);

/// Takes the loading overlay away, once the image is.
void set_page_loaded();
} // namespace sv::impl
