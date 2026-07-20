#pragma once

#include <shaped-rendering/fwd.hh>

/// Internals of the window backend that a test needs, and nothing else.
///
/// Internal: not part of the public header set, and nothing outside shaped-rendering's own sources and tests may
/// include it — see docs/coding-guidelines.md.
/// Names no SDL type, so including it costs a consumer nothing; it is under impl/ because it exposes a detail of
/// how the backend addresses its windows, not because of what it includes.

namespace sr::impl
{
/// The backend's window id for `w`, or 0 if it has none.
///
/// Exists so a test can push a synthetic event addressed at a particular window, which is the only way to reach
/// the event-dispatch path — routing an event to the right window cannot be provoked through the public API.
[[nodiscard]] u32 backend_window_id(window const& w);
} // namespace sr::impl
