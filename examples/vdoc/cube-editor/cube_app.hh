#pragma once

#include "camera.hh"
#include "cube_renderer.hh"

#include <clean-core/error/optional.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/capture.hh>
#include <shaped-rendering/imgui_context.hh>
#include <shaped-rendering/window.hh>
#include <shaped-shader-library/shader_library.hh>

/// The bring-up both examples share: a window, a dx12 context, the shader library, a swapchain, imgui, the renderer.
///
/// None of this is what either example is ABOUT, which is exactly why it lives here.
/// libs/graphics/shaped-rendering/tests/imgui-manual-test.cc is the same sequence with nothing over it, and is the
/// place to look when only the graphics bring-up is the question.
///
/// **It also answers the capture protocol itself**, which is the other reason to read this file.
/// `sv::interactive` gives an example capture for free, but only an example built on sv; this one owns its own window,
/// swapchain and renderer, so it opts in by handling `sr::capture_request` here — see docs/guides/examples.md.
/// Both example bodies are unchanged by it, because the whole protocol is answered in the two functions below:
/// `begin_frame` stops the loop when the run is done, and `end_frame` renders offscreen and writes the image.
///
/// This app offers the default view and no named captures.
/// One that wanted them would compare `sr::capture_request::name` against what it offers and refuse an unknown one,
/// exactly as `create` refuses any name today: falling back to the default view would hand back a plausible, wrong
/// image under the requested name's filename.

namespace cube_editor
{
class app
{
public:
    /// Null when there is no window backend, no D3D12 device at all, no shader compiler, or a broken shader.
    /// Reports why on stderr, since an example that silently does nothing is worse than one that says what is missing.
    ///
    /// Heap-held for the renderer's sake: a pipeline cache guards its map with a mutex, so nothing around it can move either.
    [[nodiscard]] static cc::unique_ptr<app> create(cc::string_view title);

    ~app();

    [[nodiscard]] sr::window_system& windows() const { return *_wsys; }
    [[nodiscard]] sr::window& window() const { return *_win; }
    [[nodiscard]] sr::imgui_context& imgui() { return _imgui; }
    [[nodiscard]] renderer& scene_renderer() { return *_renderer; }

    /// True while the window is open. Pumps the OS queue and feeds imgui, so it must be the loop condition.
    /// Skips a minimized frame internally by reporting a zero viewport, which `render` then declines to draw.
    ///
    /// Under capture it is also what ENDS the run: there is no window to close, so it returns false once the
    /// requested frames have been drawn and the image written.
    [[nodiscard]] bool begin_frame();

    /// Draws the cubes, then imgui over them, and presents.
    /// One scope for both: imgui composites onto the 3D image rather than replacing it.
    void end_frame(vdoc::document const& doc, orbit_camera const& cam, vdoc::entity_id selected);

    /// Whether this run is a capture rather than a session someone is sitting in front of.
    ///
    /// The examples ask because a reference image should show what a FIRST run looks like.
    /// Both of them restore the camera and the panel layout their last session left in the document's workspace, and
    /// a capture that did the same would bake local state into a committed image — so the same refresh would produce
    /// a different picture on every machine.
    [[nodiscard]] bool is_capturing() const { return _capture.active; }

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

    /// What the environment asked for, or an inactive request on an ordinary interactive run.
    sr::capture_request _capture;

    /// Capture only: what the frame is composited into, in place of a back buffer.
    sg::texture_2d _capture_target;
    u32 _captured_frames = 0;

    float _delta_time = 1.0f / 60.0f;
    double _last_time = 0.0;
    tg::vec2i _viewport = tg::vec2i(0, 0);
};
} // namespace cube_editor
