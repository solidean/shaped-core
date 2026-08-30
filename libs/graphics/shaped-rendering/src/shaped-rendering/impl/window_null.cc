#include <clean-core/common/assert.hh>
#include <shaped-rendering/impl/window_internals.hh>
#include <shaped-rendering/window.hh>

// The window backend used when shaped-rendering was built without one.
//
// try_create fails with a reason a caller can print, and that is the whole backend:
// no window_system can exist, so no window can either, and every other entry point here is unreachable rather than empty.
// Asserting that is better than a silent no-op — a window method running without a backend means the failure from try_create went unchecked, and that is worth finding.

namespace sr
{
namespace
{
// Two independent causes, and the message names both: SDL3 was never fetched, or it was fetched and then skipped at
// configure for missing X11 / wayland development headers.
// Naming only the first sends a reader on this second machine to a fetch script that already ran.
constexpr char const* no_backend_reason
    = "shaped-rendering was built without a window backend: SDL3 was not fetched, or was skipped at configure for "
      "missing X11 / wayland development headers. Run `uv run dev.py doctor` — its `sr::window (SDL3)` line says "
      "which.";

[[noreturn]] void unreachable_without_backend()
{
    CC_UNREACHABLE("no sr::window_system can exist without a window backend, so this cannot be reached");
}
} // namespace

cc::result<cc::unique_ptr<window_system>> window_system::try_create(window_system_description const&)
{
    return cc::error(cc::string(no_backend_reason));
}

// Nothing to tear down, and nothing that could have been constructed to tear down: try_create never returns one.
window_system::~window_system() = default;

void window_system::assert_owning_thread() const
{
    unreachable_without_backend();
}

cc::result<cc::unique_ptr<window>> window_system::try_create_window(window_description const&)
{
    unreachable_without_backend();
}

void window_system::unregister_window(window*)
{
    unreachable_without_backend();
}

void window_system::poll_events()
{
    unreachable_without_backend();
}

window::~window() = default;

sg::native_window window::native_window() const
{
    unreachable_without_backend();
}

void window::set_title(cc::string_view)
{
    unreachable_without_backend();
}

void window::show()
{
    unreachable_without_backend();
}

void window::hide()
{
    unreachable_without_backend();
}

void window::focus()
{
    unreachable_without_backend();
}

void window::set_position(tg::pos2i)
{
    unreachable_without_backend();
}

void window::set_size(tg::vec2i)
{
    unreachable_without_backend();
}

void window::set_relative_mouse_mode(bool)
{
    unreachable_without_backend();
}

void window::start_text_input()
{
    unreachable_without_backend();
}

void window::stop_text_input()
{
    unreachable_without_backend();
}

window_system& window::system() const
{
    unreachable_without_backend();
}

void window_system::set_cursor(cursor_shape)
{
    unreachable_without_backend();
}

void window_system::set_cursor_visible(bool)
{
    unreachable_without_backend();
}

cc::string window_system::clipboard_text() const
{
    unreachable_without_backend();
}

void window_system::set_clipboard_text(cc::string_view)
{
    unreachable_without_backend();
}

bool window_system::has_clipboard_text() const
{
    unreachable_without_backend();
}

cc::vector<display_info> window_system::displays() const
{
    unreachable_without_backend();
}

u32 impl::backend_window_id(window const&)
{
    unreachable_without_backend();
}

} // namespace sr
