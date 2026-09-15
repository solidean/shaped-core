#pragma once

#include <clean-core/error/result.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-viewer/fwd.hh>

namespace sv
{
/// How a viewer gets hold of a rendering context.
///
/// A provider only has to *create* a context: it is called at most once per process, and the handle it returns is what
/// every viewer from then on receives.
/// So it needs no static of its own and no caching.
/// The handle is refcounted, so every viewer holding one keeps the context alive whatever else lets go of it.
using context_provider = cc::unique_function<cc::result<sg::context_handle>()>;

/// Sets the hook that decides which context viewers run on.
///
///     sv::set_acquire_context([] { return sg::create_dx12_context({.adapter = sg::backend::dx12::dx12_adapter::warp}); });
///
/// Unset by default, and then `impl::acquire_default_context` answers instead — so a caller who never touches this
/// gets a working viewer, and one who does is in full control.
/// Passing `{}` clears the provider again — the paragraph below is what that does and does not undo.
///
/// Set it before the first viewer is created.
/// Once a provider has answered successfully its context is kept for the process, so setting this afterwards has no
/// effect; a provider that *failed* is retried, which is the case where setting it late is exactly the fix.
void set_acquire_context(context_provider provider);

namespace impl
{
/// Creates the context viewers use when no provider was set, preferring a hardware device and falling back to WARP.
///
/// Today that is always dx12; which backend it reaches for is a per-target choice that will grow with the platforms sv
/// supports, and is why this sits behind a hook rather than in the viewer.
///
/// It creates unconditionally — `acquire_viewer_context` is what makes that happen only once.
/// Fails, rather than asserting, when sv was built with no backend to fall back on.
[[nodiscard]] cc::result<sg::context_handle> acquire_default_context();
} // namespace impl

/// The context every viewer runs on: the caller's provider if they set one, otherwise the built-in default.
///
/// Created on the first call and shared by every caller after, so viewers run in succession — or side by side — on one
/// device rather than one each.
/// The ACQUISITION is thread-safe — two threads asking at once get one device, not two.
/// What it hands back keeps sg's own rules, and the window system it feeds is main-thread bound anyway.
[[nodiscard]] cc::result<sg::context_handle> acquire_viewer_context();

/// Completes once the background work started so far against `ctx` has settled, whether it succeeded or failed.
///
/// It also waits on process-wide work such as shader compiles and cache writes, so it may wait for work another context started.
/// What it covers beyond that is internal.
/// Such work outlives the frame that started it, so a loop or a test that must leave nothing running awaits this before it ends.
/// **Lazy**: start it after the frame loop, never inside a frame, where under `SC_THREADS=OFF` a scheduled node crashed the frame's submit.
[[nodiscard]] cc::shared_async<cc::unit> background_work(sg::context& ctx);
} // namespace sv
