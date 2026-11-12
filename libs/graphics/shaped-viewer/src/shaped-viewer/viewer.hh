#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/present/swapchain.hh>
#include <shaped-viewer/frame.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/impl/view_state.hh>
#include <shaped-viewer/resources/gpu_resource_manager.hh>
#include <shaped-viewer/stable_id.hh>

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

    /// Runs with no window, no swapchain and no present: the frame is composited into an offscreen texture and left
    /// there, `width` x `height`.
    ///
    /// It needs no display and no window backend, so a capture run uses it.
    /// The frame API is unchanged either way: `frame::viewport_size` reports the offscreen size and nothing hands out
    /// a swapchain, so an authoring body cannot tell the difference.
    /// A headless loop is ended by `request_close` alone, since there is no window to close.
    bool headless = false;

    /// A swapchain to present into, in place of a window the viewer opens: on the web, the page's canvas.
    ///
    /// Its window's `client_size`, or its `headless_extent`, is the viewer's first size, must be positive, and replaces `width` and `height`.
    /// After that the viewer's size is the back buffer's; `viewer::set_surface_size` asks for a new one.
    /// `buffer_count` and `format` are the viewer's own, whatever this says; every other field, `present_mode` and `enable_hdr` among them, is taken as given.
    /// With no window there is no display scale, so drawings lay out at 1.0 on a high-density canvas.
    /// No window backend comes up, so nothing polls input, and the loop is ended by `request_close` alone, as a headless one is.
    /// Ignored when `headless` is set.
    cc::optional<sg::swapchain_description> surface;

    /// How much GPU time a frame's still wavefront layers may take together, in milliseconds, split evenly among them; 0 runs each layer's `rounds_per_frame`.
    ///
    /// A still layer's rounds follow from what its last frames measured, so the image converges as fast per second as the GPU allows while the viewer stays responsive.
    /// A frame whose image changed is not held to it, since it drains.
    /// Needs `sg::feature::timestamp_query`; without it the rounds stay at `rounds_per_frame`.
    /// A headless viewer, which is what a capture runs in, treats it as 0, so its image depends on no machine's speed.
    f32 frame_budget_ms = 16.0f;

    /// Swapchain back buffers, and also the pipelining depth passed to advance_epoch.
    /// Must be >= 2.
    int buffer_count = 3;

    /// Budgets for the viewer's own `gpu_resource_manager` — per-manager LRU limits.
    /// The defaults suit a viewer-sized working set; a caller whose content says otherwise sets them here rather
    /// than reaching past the viewer.
    gpu_resource_manager_config resources = {};
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
    /// Blocks until the frames in flight fall back to the swapchain's depth, so it requires a context that may block.
    void end_frame();

    /// Presents the frame `begin_frame` opened, and hands back when the loop may go on instead of waiting for it.
    /// **The frame loop of a context that never blocks**, WebGPU's above all: awaiting it is where a page's thread
    /// returns to the browser, so the canvas is composited and input arrives.
    ///
    ///     while (viewer.is_running())
    ///     {
    ///         auto& frame = viewer.begin_frame();
    ///         if (frame.is_open())
    ///             frame.add_scene().add_mesh(mesh);
    ///         co_await viewer.end_frame_async();
    ///     }
    ///     co_await viewer.finish();
    ///
    /// A closed frame submits nothing, and what it hands back is already settled.
    [[nodiscard]] cc::shared_async<cc::unit> end_frame_async();

    /// Drains what the viewer still owes — an open frame, capture writes, the frames in flight, background work —
    /// without blocking; its destructor then owes nothing.
    /// **Required on a context that never blocks**, whose viewer cannot wait in its destructor.
    /// Elsewhere it is optional, and the destructor drains what it skipped.
    /// The viewer must outlive the returned async.
    [[nodiscard]] cc::shared_async<cc::unit> finish();

    /// Asks a `viewer_config::surface` to take `size` from the next frame on, as a page does when its canvas changes size.
    /// Only a surface with no size of its own reads it: a canvas, or wayland; win32 and X11 report theirs, and a headless chain never resizes.
    /// No effect on a viewer without a surface, whose window or offscreen texture decides.
    void set_surface_size(tg::vec2i size);

    /// Ends the frame loop: the current frame still finishes and presents, then `frames()` stops.
    /// Exactly what the window's close button does, so a caller needs no handle on the window to quit.
    void request_close();

    /// Queues an input event for the next frame, as a page does with its canvas's pointer, wheel and key events.
    ///
    /// For a viewer with no window -- a `viewer_config::surface` or a headless one -- whose input arrives from outside.
    /// The next frame routes the queued events to the camera controllers exactly as a window's are routed, and
    /// `frame::input_events` answers them; `window` must be null, and positions are pixels of the surface.
    /// From the thread driving the viewer; ignored on a viewer with a window, which polls its own.
    void feed_input(sr::input_event event);

private:
    struct impl;
    cc::unique_ptr<impl> _impl;

    explicit viewer(cc::unique_ptr<impl> im);

    /// The GPU resources every view in this viewer draws from.
    /// Reached through `frame::resources`, which is the only sanctioned way in — a caller has no viewer to ask.
    [[nodiscard]] sv::gpu_resource_manager& resources();

    /// `frame::gpu_frame_ms`'s answer.
    [[nodiscard]] cc::optional<double> gpu_frame_ms() const;

    /// The post-load work the resource managers still owe, as a const query — `frame::pending_resource_work`'s answer.
    /// Separate from `resources()` because that one hands out a mutable manager, and asking a count should not.
    [[nodiscard]] isize pending_resource_work() const;

    /// The payloads still streaming in, as a const query — `frame::streaming_resources`'s answer.
    [[nodiscard]] isize streaming_resources() const;

    /// The last poll's events, or the events fed since the frame before on a viewer without a window — `frame::input_events`'s answer.
    [[nodiscard]] cc::span<sr::input_event const> input_events() const;

    /// `frame::background_work`'s answer.
    [[nodiscard]] cc::shared_async<cc::unit> background_work();

    /// `frame::imgui`'s answer: brings the imgui context up on first use, and opens this frame's imgui frame if it is not open yet.
    [[nodiscard]] bool open_imgui();

    /// Whether the last imgui frame wanted the keyboard or the mouse; false while no imgui frame ever ran.
    [[nodiscard]] bool imgui_wants_keyboard() const;
    [[nodiscard]] bool imgui_wants_mouse() const;

    /// Opens this frame's imgui frame, sized to the window (or the offscreen target) and stepped by `delta`.
    void begin_imgui_frame(double delta);

    /// Ends the open imgui frame and records its draw data on the frame's command list, over `output`.
    /// Called after the views are composited, so the UI lands on top of them.
    void record_imgui(sg::render_target_view const& output);

    /// Installs the capture this run is taking; the viewer must have been created headless for it.
    ///
    /// **Only `sv::interactive` calls this.**
    /// A caller driving `begin_frame` / `end_frame` never gets a session, whatever the environment says, so an
    /// application embedding the viewer cannot have its loop ended out from under it.
    void install_capture(sr::capture_request req);

    /// Installs a capture whose finished image lands in `landing` instead of a file; the viewer must be headless.
    ///
    /// **Only `sv::render_settled` calls this**, and `landing` outlives the viewer.
    /// The request's `output_path` is unused, since nothing is written.
    void install_settled(sr::capture_request req, sv::impl::settled_landing* landing);

    /// Runs `body` when `name` is the capture being taken, and notes the name either way for the listing mode.
    ///
    /// Called from `frame::register_capture`, so the callback runs exactly where it was declared in the frame body —
    /// which is what lets it override what the body already wrote.
    void apply_capture(cc::string_view name, cc::function_ref<void(capture_context const&)> body, frame& f);

    /// Holds a capture for this frame, `reason` naming what it waits for; called from `frame::hold_capture`.
    void hold_capture(cc::string_view reason);

    /// The persistent state of the view named `id`, created on first use.
    /// The frame reaches this on a caller's behalf; nothing outside authoring should hold the reference across frames,
    /// since the cache reclaims views that go unseen.
    [[nodiscard]] sv::impl::view_state& state_of(view_id id);

    /// Decides what a capture run does next: settle, give up, or keep going — and writes the image when it is either
    /// of the first two.
    /// Runs after the frame has been submitted, since that is when its accumulators hold this frame's counts.
    void advance_capture(render_plan const& plan, bool traces_ran);

    /// Waits for every capture image still being read back or encoded, logging each that failed.
    void drain_capture_writes();

    /// Runs one frame's rendering: resolve layout, translate views to a viewer_definition, drive viewer_renderer, present, advance the epoch.
    /// Called by frame::submit; it waits for nothing, and leaves the frame's throttle for the caller.
    /// Never throws — device loss stops the viewer instead.
    void finish_frame(frame& f);

    /// The throttle the last submitted frame left, or a settled async where it left none.
    [[nodiscard]] cc::shared_async<cc::unit> take_throttle();

    /// Blocks on that throttle: what end_frame and a frame_scope do, on a context that may block.
    void wait_for_throttle();

    /// Polls the window, routes input to the built-in camera controllers and acquires the next frame — the one step
    /// both loops take.
    /// What each does with the frame differs, and that is the whole difference between them.
    [[nodiscard]] frame acquire_frame();

    /// `begin_frame` without its `sv.frame` record span, for `async_frames`: its frame is authored in the caller's
    /// coroutine step, and a span may not stay open across the `co_await` that hands the frame over.
    [[nodiscard]] frame& begin_frame_unrecorded();

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

    /// Asks for the surface point under `window_point` to become its view's orbit centre.
    /// Only the trace knows that point, so this records the request, and `read_back_pick` and `apply_pick` finish it frames later.
    void request_pick(tg::pos2f window_point);

    /// Reads the primary depth under a pending pick back, in this frame's command list, once its view traced in it.
    void read_back_pick(viewer_definition const& def, render_plan const& plan);

    /// Starts the picked view's glide to its new orbit centre once its depth has landed.
    /// A click on the background reads an escaped ray, and moves nothing.
    void apply_pick();

    friend class frame;
    friend class frame_iterator;
    friend class frame_range;
    friend class async_frames;

    /// interactive() owns the viewer it opens, so it starts the loop the same way frames() does.
    friend frame_range interactive(cc::string_view id, viewer_config config);
    friend frame_range interactive(sg::context& ctx, cc::string_view id, viewer_config config);
    friend frame_range interactive(cc::string_view id, viewer_config config, sr::capture_request capture);
    friend frame_range interactive(sg::context& ctx, cc::string_view id, viewer_config config, sr::capture_request capture);
    friend cc::result<settled_image> render_settled(sg::context& ctx,
                                                    cc::string_view id,
                                                    settled_options const& options,
                                                    cc::function_ref<void(frame&)> author);
};
