#pragma once

#include <shaped-rendering/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

struct ImGuiContext;

namespace sr
{
/// Owns the Dear ImGui context and brackets a frame — the platform half of an imgui backend, as far as it
/// goes without a windowing system. The renderer half is sr::imgui_renderer.
///
///     auto imgui = sr::imgui_context::create();
///     auto renderer = sr::imgui_renderer::create(ctx);
///
///     imgui.begin_frame({.display_size = tg::vec2i(w, h), .delta_time = dt});
///     ImGui::ShowDemoWindow();
///     imgui.end_frame();
///     // ... renderer.prepare() / renderer.render() with ImGui::GetDrawData()
///
/// Docking is enabled. Input is *not* wired yet — until the windowing system lands, the UI renders but
/// does not respond to mouse or keyboard; see the TODO(windowing) notes in the .cc for exactly what the
/// platform layer must fill in.
///
/// Move-only, and destroying it destroys the ImGui context. ImGui keeps one process-global current
/// context, so only one of these may exist at a time.
class imgui_context
{
    // construction
public:
    /// Creates the ImGui context, routes its allocations through clean-core, and enables docking.
    [[nodiscard]] static imgui_context create();

    imgui_context() = default;
    ~imgui_context();

    imgui_context(imgui_context&& other) noexcept;
    imgui_context& operator=(imgui_context&& other) noexcept;
    imgui_context(imgui_context const&) = delete;
    imgui_context& operator=(imgui_context const&) = delete;

    // frame
public:
    struct frame_info
    {
        /// Display extent in imgui's own units (logical pixels).
        tg::vec2i display_size;

        /// Seconds since the previous frame. Must be > 0 — imgui divides by it.
        float delta_time = 1.0f / 60.0f;

        /// Framebuffer pixels per display unit. 1 unless the caller is doing DPI scaling itself.
        tg::vec2f framebuffer_scale = tg::vec2f(1.0f, 1.0f);
    };

    /// Starts a frame: publishes the display size and timestep, then ImGui::NewFrame().
    void begin_frame(frame_info const& info);

    /// Ends the frame and builds the draw data, which ImGui::GetDrawData() then returns.
    void end_frame();

    // queries
public:
    [[nodiscard]] bool is_valid() const { return _ctx != nullptr; }

private:
    explicit imgui_context(ImGuiContext* ctx) : _ctx(ctx) {}

    ImGuiContext* _ctx = nullptr;
};
} // namespace sr
