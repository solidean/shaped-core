#include <clean-core/common/assert.hh>
#include <shaped-rendering/impl/window_internals.hh>
#include <shaped-rendering/window.hh>

// The window backend used when shaped-rendering was built without one, because SDL3 was not fetched.
//
// try_create fails with a reason a caller can print, and that is the whole backend: no window_system can
// exist, so no window can either, and every other entry point here is unreachable rather than empty.
// Asserting that is better than a silent no-op — a window method running without a backend means the
// failure from try_create went unchecked, and that is worth finding.

namespace sr
{
namespace
{
constexpr char const* no_backend_reason = "shaped-rendering was built without a window backend: SDL3 was not fetched. "
                                          "Run `uv run extern/sdl3/fetch-sdl3.py` and reconfigure.";

[[noreturn]] void unreachable_without_backend()
{
    CC_UNREACHABLE("no sr::window_system can exist without a window backend, so this cannot be reached");
}
} // namespace

cc::result<cc::unique_ptr<window_system>> window_system::try_create(window_system_description const&)
{
    return cc::error(cc::string(no_backend_reason));
}

window_system::~window_system()
{
    // Reached only if a window_system was somehow constructed, which try_create never does.
}

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

void* window::native_window_handle() const
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

u32 impl::backend_window_id(window const&)
{
    unreachable_without_backend();
}
} // namespace sr
