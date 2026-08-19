#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-viewer/frame.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/impl/view_state.hh>
#include <shaped-viewer/view/view_id.hh>

/// How a viewer is created.
/// All defaults are sensible for a windowed, path-traced viewer.
struct sv::viewer_config
{
    /// The window title.
    /// Unset takes the viewer's own id, up to its `##` — so naming a viewer usually titles it too.
    /// Optional rather than an empty default, so a caller can still ask for a genuinely blank title.
    cc::optional<cc::string> title;

    int width = 1280;
    int height = 720;

    /// Swapchain back buffers, and also the pipelining depth passed to advance_epoch.
    /// Must be >= 2.
    int buffer_count = 3;
};

/// The viewer: owns the window, swapchain, shader library and scene resources, and drives the per-frame loop.
/// Each frame is flattened into a render plan and recorded through viewer_renderer.
///
/// It acquires its rendering context through the provider `sv::set_acquire_context` installed, falling back to a built-in default, and holds the
/// handle for its whole life — so a caller needs no context of their own, and one who wants a particular device sets
/// that hook once rather than threading a context through every call.
/// Everything else the viewer sets up itself.
/// Single viewer per process for now, because the window system is one-per-process.
/// Move-only.
///
/// The window itself stays inside — nothing hands out a handle to it yet.
///
///     auto viewer = sv::viewer::create("main");
///     for (auto frame : viewer.frames())
///         frame.add_scene().add_mesh(mesh);
///
/// `sv::interactive` is the shorthand that owns the viewer for the loop's lifetime.
///
/// `begin_frame` / `end_frame` is the same loop written out, for an application whose own loop must stay in charge:
///
///     while (viewer.is_running())
///     {
///         auto& frame = viewer.begin_frame();
///         if (!frame.is_open())
///             continue; // the window cannot draw right now
///         frame.add_scene().add_mesh(mesh);
///         viewer.end_frame();
///     }
///
/// The two are the same `sv::frame` authored the same way, and differ only in what ends it: `frames()` yields a
/// `frame_scope`, whose destructor presents, while `begin_frame` hands out the bare frame for `end_frame` to present.
class sv::viewer
{
public:
    /// Brings up a viewer on whatever the provider hands back, or the built-in default when none was set.
    /// `id` names this viewer's persistent state.
    ///
    /// The viewer holds the context handle for its whole life, so the context outlives it whatever else lets go.
    [[nodiscard]] static cc::result<viewer> try_create(cc::string_view id, viewer_config config = {});

    /// Brings up a viewer on a context the caller owns, bypassing the provider entirely.
    ///
    /// `ctx` must outlive the viewer: nothing here can hold a reference count on a context it was handed by reference.
    /// Prefer the overload above, or call `sv::set_acquire_context`, unless the lifetime is genuinely the caller's to manage.
    [[nodiscard]] static cc::result<viewer> try_create(sg::context& ctx, cc::string_view id, viewer_config config = {});

    /// Throwing counterparts of try_create.
    [[nodiscard]] static viewer create(cc::string_view id, viewer_config config = {});
    [[nodiscard]] static viewer create(sg::context& ctx, cc::string_view id, viewer_config config = {});

    viewer(viewer&&) noexcept;
    viewer& operator=(viewer&&) noexcept;
    viewer(viewer const&) = delete;
    viewer& operator=(viewer const&) = delete;
    ~viewer();

    /// The frame loop: `for (auto frame : viewer.frames()) { ... }`.
    /// It polls the window and skips frames that cannot be drawn; what it yields is a `frame_scope`, so each frame
    /// presents when the loop body ends.
    ///
    /// Leaving the loop — through the window closing, or a `break` — closes the window, since nothing polls it after
    /// that.
    /// A viewer that must outlive its loop is driven by `begin_frame` / `end_frame` instead.
    [[nodiscard]] frame_range frames();

    /// Whether the window is still open — not close-requested, not quit, not lost.
    /// The condition of a hand-driven loop, and what `frames()` stops on.
    [[nodiscard]] bool is_running() const;

    /// Opens the next frame, for a loop the caller drives.
    /// The viewer owns it, so the reference is good until `end_frame` — which every open frame needs.
    ///
    /// A closed frame (`is_open()` false) is one the window cannot draw right now: it needs no `end_frame`, so
    /// `continue` and loop again.
    [[nodiscard]] frame& begin_frame();

    /// Presents the frame `begin_frame` opened — here nothing else will, since the viewer holds a bare `frame` rather
    /// than the `frame_scope` that ends the loop's frames for it.
    void end_frame();

    /// Ends the frame loop: the current frame still finishes and presents, then `frames()` stops.
    /// Exactly what the window's close button does, so a caller needs no handle on the window to quit.
    void request_close();

private:
    struct impl;
    cc::unique_ptr<impl> _impl;

    explicit viewer(cc::unique_ptr<impl> im);

    /// The GPU resources every view in this viewer draws from.
    /// Reached through `frame::resources`, which is the only sanctioned way in — a caller has no viewer to ask.
    [[nodiscard]] sv::scene_resources& scene_resources_of();

    /// The persistent state of the view named `id`, created on first use.
    /// The frame reaches this on a caller's behalf; nothing outside authoring should hold the reference across frames,
    /// since the cache reclaims views that go unseen.
    [[nodiscard]] sv::impl::view_state& state_of(view_id id);

    /// Runs one frame's rendering: resolve layout, translate views to a viewer_definition, drive viewer_renderer, present, advance the epoch.
    /// Called by frame::present.
    /// Never throws — device loss stops the viewer instead.
    void finish_frame(frame& f);

    /// Polls the window, routes input to the built-in camera controllers and acquires the next frame — the one step
    /// both loops take.
    /// What each does with the frame differs, and that is the whole difference between them.
    [[nodiscard]] frame acquire_frame();

    /// Opens the loop: the next frame is frame one and restamps the clock.
    /// `frames()` and `interactive()` call this, and a manual loop needs it only to number a second run of frames from
    /// one again.
    void begin_frames();

    /// Feeds the last poll's events to the built-in camera controllers, one view at a time.
    ///
    /// A view is picked by the drag it holds, else by the frontmost rect under the cursor — last frame's rect, since this runs before the frame is authored.
    /// A view whose camera the caller set last frame is skipped entirely.
    void route_input();

    /// Starts dragging the view under `window_point`, if the caller offered it this frame.
    /// Records where inside the pane the cursor grabbed, so the first motion does not snap its corner to the cursor.
    void begin_move(tg::pos2f window_point);

    /// Moves the view being dragged so the grabbed point tracks `window_point`.
    void move_to(tg::pos2f window_point);

    /// Magnifies the view under `window_point` by `ticks` wheel steps, keeping the point under the cursor fixed.
    /// Writes only the view's persistent zoom — never a camera, and never anything a trace reads.
    void zoom_at(tg::pos2f window_point, float ticks);

    friend class frame;
    friend class frame_iterator;

    /// interactive() owns the viewer it opens, so it starts the loop the same way frames() does.
    friend frame_range interactive(cc::string_view id, viewer_config config);
    friend frame_range interactive(sg::context& ctx, cc::string_view id, viewer_config config);
};
