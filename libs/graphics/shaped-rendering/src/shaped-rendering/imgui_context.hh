#pragma once

#include <imgui/imgui_fwd.hh>
#include <shaped-rendering/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

#include <memory>

namespace sr
{
/// How an imgui_context is created.
struct imgui_context_description
{
    /// Whether an imgui window dragged outside the main one becomes its own OS window.
    ///
    /// Off by default, because turning it on changes the contract in two ways a caller must opt into:
    /// every imgui coordinate — mouse positions, ImGui::SetNextWindowPos — becomes desktop-space rather than relative to the main window,
    /// and update_viewports must run every frame.
    /// Skipping that call does not merely leave viewports unmoved; imgui stops hit-testing the mouse at all, so nothing hovers.
    ///
    /// Needs a window_system, so a caller driving begin_frame(frame_info) alone must leave this off.
    bool enable_viewports = false;

    /// Whether create() paints the context in the Solidean theme (sr::apply_solidean_default_style).
    /// On by default, so an app gets the brand look without a call.
    /// Turn it off to keep imgui's stock dark theme, or to apply your own style after create().
    bool apply_default_style = true;
};

/// Owns the Dear ImGui context and brackets a frame — the platform half of an imgui backend, as far as it goes without a windowing system.
/// The renderer half is sr::imgui_routine.
///
///     auto imgui = sr::imgui_context::create();
///
///     wsys->poll_events();
///     imgui.process_events(*wsys);
///     imgui.begin_frame(*win, delta_time);
///     ImGui::ShowDemoWindow();
///     imgui.end_frame();
///     // ... sr::imgui_routine::execute(pass, ImGui::GetDrawData()) inside a rendering scope
///
/// Docking is always enabled.
/// Multi-viewport is opt-in via imgui_context_description::enable_viewports, and then installs itself on the first begin_frame(window&) — see update_viewports.
///
/// Move-only, and destroying it destroys the ImGui context.
/// ImGui keeps one process-global current context, so only one of these may exist at a time.
class imgui_context
{
    // construction
public:
    /// Creates the ImGui context, routes its allocations through clean-core, and enables docking.
    [[nodiscard]] static imgui_context create(imgui_context_description const& desc = {});

    imgui_context();
    ~imgui_context();

    imgui_context(imgui_context&& other) noexcept;
    imgui_context& operator=(imgui_context&& other) noexcept;
    imgui_context(imgui_context const&) = delete;
    imgui_context& operator=(imgui_context const&) = delete;

    // input
public:
    /// Feeds a frame of input to imgui.
    ///
    /// Call once per frame between window_system::poll_events and begin_frame, so imgui sees this frame's input before it decides what the UI looks like:
    ///
    ///     wsys->poll_events();
    ///     imgui.process_events(*wsys);
    ///     imgui.begin_frame(*win, dt);
    ///
    /// Events for other windows are fed too — imgui tracks one input state, and a single-viewport app has only one window that can be focused anyway.
    void process_events(window_system const& wsys);

    /// Feeds one event.
    /// process_events is the usual entry point; take this when the caller filters the stream itself, e.g. to route events to a viewport before imgui sees them.
    void process_event(input_event const& event);

    /// Whether imgui wants the keyboard or the mouse this frame — true while a text field has focus or the cursor is over a window.
    /// Check these before acting on the same input yourself, or a camera will spin while the user drags an imgui slider.
    /// Meaningful only between begin_frame and end_frame.
    [[nodiscard]] bool wants_keyboard() const;
    [[nodiscard]] bool wants_mouse() const;

    // frame
public:
    struct frame_info
    {
        /// Display extent in imgui's own units (logical pixels).
        tg::vec2i display_size;

        /// Seconds since the previous frame.
        /// Must be > 0 — imgui divides by it.
        float delta_time = 1.0f / 60.0f;

        /// Framebuffer pixels per display unit.
        /// 1 unless the caller is doing DPI scaling itself.
        tg::vec2f framebuffer_scale = tg::vec2f(1.0f, 1.0f);
    };

    /// Starts a frame: publishes the display size and timestep, then ImGui::NewFrame().
    void begin_frame(frame_info const& info);

    /// Starts a frame sized from `win`, and hands the window imgui's text-input intent so an on-screen keyboard and IME engage around a focused text field.
    ///
    /// Skips the frame's text-input handover while the window is minimized (it has no drawable area then);
    /// the caller should skip rendering too — see window::is_minimized.
    void begin_frame(window& win, float delta_time);

    /// Ends the frame and builds the draw data, which ImGui::GetDrawData() then returns.
    void end_frame();

    // Multi-viewport.

    /// Brings the OS windows behind imgui's viewports in line with what this frame decided:
    /// opens one for a window the user dragged out, moves and resizes the rest, closes what is gone.
    ///
    /// Call it after end_frame, then sr::imgui_routine::render_viewports to draw and present them.
    /// Both are no-ops while viewports are off, which they are until a begin_frame(window&) supplies a window_system.
    ///
    /// The window_system must outlive this context: closing a viewport destroys an sr::window, which unregisters itself from the system it came from.
    void update_viewports();

    // queries
public:
    [[nodiscard]] bool is_valid() const { return _ctx != nullptr; }

private:
    /// Wraps a freshly created ImGuiContext, installing the deleter that tears its backend down.
    /// Out-of-line because the deleter lambda touches ImGui, which this header keeps out.
    explicit imgui_context(ImGuiContext* ctx);

    /// Points imgui's clipboard hooks at `wsys`.
    /// Idempotent, and called from begin_frame(window&) rather than from create() because that is the first point a window_system is in reach.
    static void install_clipboard(window_system& wsys);

    /// Installs the platform viewport callbacks against `wsys` and enables multi-viewport — same reason as install_clipboard for living here rather than in create().
    /// Restates the main viewport's handles from `main_window` on every call, since the caller may drive a different window than it did last frame.
    /// Does nothing unless the description asked for viewports.
    void install_viewports(window_system& wsys, window& main_window);

    std::unique_ptr<ImGuiContext, void (*)(ImGuiContext*)> _ctx;

    /// What the description asked for.
    /// The ImGui config flag only goes on once a window_system is in reach.
    bool _viewports_requested = false;

    /// Set by end_frame while viewports are on, cleared by update_viewports.
    /// begin_frame asserts on it, so a caller who never wires update_viewports gets told rather than losing all mouse hit-testing silently.
    bool _viewport_update_pending = false;
};
} // namespace sr
