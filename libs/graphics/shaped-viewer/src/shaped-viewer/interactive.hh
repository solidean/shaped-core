#pragma once

#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/async.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/viewer.hh>

namespace sv
{
/// Opens an interactive viewer named `id` and returns its frame loop, which owns it.
///
///     for (auto f : sv::interactive("my viewer"))
///         f.add_scene().add_mesh(mesh);
///
/// The viewer lives as long as the returned range, so a caller needs no variable for it and nothing to tear down.
/// `id` names the viewer's persistent state, so the same string reopens the same cameras and accumulated images.
///
/// The context comes from `sv::set_acquire_context`, or from the built-in default when no provider was set — so the common case
/// needs no context at all, and a caller who wants their own sets that hook once instead of threading one through.
/// Everything else — the window, swapchain, shader library and scene resources — the viewer brings up itself.
///
/// This throws if the viewer cannot be created (no context, no window backend, no display).
/// A device without ray tracing still creates one: its raster layers draw, and a traced layer is declined with one logged error.
/// `sv::viewer::try_create` is the fallible form, for a caller who wants to degrade rather than fail.
[[nodiscard]] frame_range interactive(cc::string_view id, viewer_config config = {});

/// The same, on a context the caller owns and keeps alive — for embedding in an application that already has one.
[[nodiscard]] frame_range interactive(sg::context& ctx, cc::string_view id, viewer_config config = {});

/// Takes `capture` instead of reading one from the environment, which is left untouched.
///
/// `capture` must be active and name an output path; it overrides `config`'s headless flag and size exactly as the
/// environment would.
/// This is what reaches a request field the environment cannot spell, such as `sr::capture_request::clock_seconds`.
[[nodiscard]] frame_range interactive(cc::string_view id, viewer_config config, sr::capture_request capture);
[[nodiscard]] frame_range interactive(sg::context& ctx,
                                      cc::string_view id,
                                      viewer_config config,
                                      sr::capture_request capture);

/// `interactive`'s frame loop with each frame awaited rather than blocked on: **the loop that runs in a browser**.
///
///     auto frames = sv::interactive_async("my viewer");
///     while (auto* f = co_await frames.next())
///         f->add_scene().add_mesh(mesh);
///
/// A page's thread only presents, takes input and hears back from the GPU once the code running on it returns, and
/// awaiting `next()` is where it does; `interactive`'s loop never returns, so it cannot run there.
/// Natively it is the same loop, on whatever context `interactive` would use, so one body serves both.
/// The context is acquired on the first `next()`, as `sv::acquire_viewer_context_async` does: on the web, the WebGPU device.
///
/// In a page, a viewer whose config names neither a `surface` nor `headless` draws into the page's `#canvas`.
/// It sizes itself to the canvas, takes the canvas's input, presents once per display refresh, and clears the page's loading overlay once its image stands.
/// Under node, with no page, it draws headless for a few hundred frames and stops, as a smoke run.
/// A capture the environment asks for is taken as `interactive` takes it.
[[nodiscard]] async_frames interactive_async(cc::string_view id, viewer_config config = {});

/// The same, on a context the caller owns and keeps alive.
[[nodiscard]] async_frames interactive_async(sg::context& ctx, cc::string_view id, viewer_config config = {});
} // namespace sv

/// The frame loop `sv::interactive_async` returns, which owns its viewer.
///
/// `next()` ends the frame it handed out before, awaits what the frames in flight allow, and opens the next frame to author.
/// What it hands back is null once the viewer stopped running, and by then the viewer has finished, drained its frames
/// and its background work.
/// The frame is good until the next call to `next()`, which must not start before the one before it settled.
///
/// A loop left early, through a `break` or an exception, leaves the viewer to the destructor.
/// It finishes there on a context that may block; on one that never blocks, it finishes in the background, which `sv::background_work` covers.
class sv::async_frames
{
public:
    async_frames(async_frames&&) noexcept;
    async_frames& operator=(async_frames&&) noexcept;
    async_frames(async_frames const&) = delete;
    async_frames& operator=(async_frames const&) = delete;
    ~async_frames();

    /// The next frame to author, or null once the loop is over.
    /// The first call brings the viewer up, and fails with what kept it from coming up.
    [[nodiscard]] cc::shared_async<frame*> next();

private:
    struct state;
    cc::unique_ptr<state> _state;

    explicit async_frames(cc::unique_ptr<state> s);

    /// `next()`'s body, on the state rather than on `this`, so it holds nothing that moving the range invalidates.
    [[nodiscard]] static cc::shared_async<frame*> advance(state& s);

    /// Hands a viewer still running to the destructor's rule: drained here, or in the background where nothing may block.
    static void abandon(state& s);

    friend async_frames interactive_async(cc::string_view id, viewer_config config);
    friend async_frames interactive_async(sg::context& ctx, cc::string_view id, viewer_config config);
};
