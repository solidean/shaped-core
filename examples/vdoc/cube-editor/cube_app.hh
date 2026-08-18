#pragma once

#include "camera.hh"
#include "cube_renderer.hh"

#include <clean-core/error/optional.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/imgui_context.hh>
#include <shaped-rendering/window.hh>
#include <shaped-shader-library/shader_library.hh>

/// The bring-up both examples share: a window, a dx12 context, the shader library, a swapchain, imgui, the renderer.
///
/// None of this is what either example is ABOUT, which is exactly why it lives here.
/// libs/graphics/shaped-rendering/tests/imgui-manual-test.cc is the same sequence with nothing over it, and is the
/// place to look when only the graphics bring-up is the question.

namespace cube_editor
{
class app
{
public:
    /// Null when there is no window backend, no D3D12 device at all, no shader compiler, or a broken shader.
    /// Reports why on stderr, since an example that silently does nothing is worse than one that says what is missing.
    ///
    /// Heap-held: the installed default async pool is scoped to this object's lifetime, and a scope guard is pinned
    /// by construction — so an app that could be moved would move the guard out from under the pool it named.
    [[nodiscard]] static cc::unique_ptr<app> create(cc::string_view title);

    ~app();

    [[nodiscard]] sr::window_system& windows() const { return *_wsys; }
    [[nodiscard]] sr::window& window() const { return *_win; }
    [[nodiscard]] sr::imgui_context& imgui() { return _imgui; }
    [[nodiscard]] renderer& scene_renderer() { return *_renderer; }

    /// True while the window is open. Pumps the OS queue and feeds imgui, so it must be the loop condition.
    /// Skips a minimized frame internally by reporting a zero viewport, which `render` then declines to draw.
    [[nodiscard]] bool begin_frame();

    /// Draws the cubes, then imgui over them, and presents.
    /// One scope for both: imgui composites onto the 3D image rather than replacing it.
    void end_frame(vdoc::document const& doc, orbit_camera const& cam, vdoc::entity_id selected);

    [[nodiscard]] float delta_time() const { return _delta_time; }
    [[nodiscard]] tg::vec2i viewport() const { return _viewport; }
    [[nodiscard]] tg::angle_f vertical_fov() const { return tg::angle_f::make_from_degree(50.0f); }

    /// The camera matrix the renderer and the picker must agree on, so both read it from here.
    [[nodiscard]] tg::mat4f view_projection(orbit_camera const& cam) const;

public:
    /// Empty and unusable; `create` is what produces a real one.
    /// Public because cc::optional needs to be able to default-construct its storage.
    app() = default;

private:

    cc::unique_ptr<sr::window_system> _wsys;
    cc::unique_ptr<sr::window> _win;
    sg::context_handle _ctx;
    slib::shader_library _lib;
    sg::swapchain_handle _swapchain;
    sr::imgui_context _imgui;
    cc::unique_ptr<renderer> _renderer;

    // The graphs sg and vdoc::file complete on their own threads need somewhere to resume.
    // Without an installed default, the first completion off a worker thread asserts.
    //
    // The guard is held by pointer rather than by optional: it has no default constructor, and an `app` that could
    // not be default-constructed could not be make_unique'd either.
    cc::async_thread_pool _pool;
    cc::unique_ptr<cc::scoped_default_async_pool> _default_pool;

    float _delta_time = 1.0f / 60.0f;
    double _last_time = 0.0;
    tg::vec2i _viewport = tg::vec2i(0, 0);
};
} // namespace cube_editor
