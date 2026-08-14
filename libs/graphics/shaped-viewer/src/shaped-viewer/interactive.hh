#pragma once

#include <clean-core/string/string_view.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/viewer.hh>

namespace sv
{
/// Opens an interactive viewer named `id` and returns its frame loop, which owns it.
///
///     for (auto f : sv::interactive("my viewer"))
///         f.scene().add_mesh(mesh, materials);
///
/// The viewer lives as long as the returned range, so a caller needs no variable for it and nothing to tear down.
/// `id` names the viewer's persistent state, so the same string reopens the same cameras and accumulated images.
///
/// The context comes from `sv::acquire_context`, or from the built-in default when that is unset — so the common case
/// needs no context at all, and a caller who wants their own sets that hook once instead of threading one through.
/// Everything else — the window, swapchain, shader library and scene resources — the viewer brings up itself.
///
/// This throws if the viewer cannot be created (no context, no window backend, no display, no ray tracing).
/// `sv::viewer::try_create` is the fallible form, for a caller who wants to degrade rather than fail.
[[nodiscard]] frame_range interactive(cc::string_view id, viewer_config config = {});

/// The same, on a context the caller owns and keeps alive — for embedding in an application that already has one.
[[nodiscard]] frame_range interactive(sg::context& ctx, cc::string_view id, viewer_config config = {});
} // namespace sv
