#include "cube_app.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/print.hh>
#include <imgui/imgui.h>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-rendering/imgui_routine.hh>
#include <shaped-rendering/shaders.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <cube_shaders.hh>

#include <chrono>

namespace cube_editor
{
namespace
{
[[nodiscard]] double now_seconds()
{
    auto const t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double>(t).count();
}
} // namespace

cc::unique_ptr<app> app::create(cc::string_view title)
{
    auto out = cc::make_unique<app>();

    // Read once, before anything is brought up: it decides whether there is a display in the picture at all.
    out->_capture = sr::capture_request::from_environment();

    // This app offers the default view only. A name is refused rather than quietly answered with that view, since the
    // image would then be committed under a name nothing produced.
    if (out->_capture.active && !out->_capture.name.empty())
    {
        cc::eprintln("this example offers no named capture, so it cannot take {}", out->_capture.name);
        return nullptr;
    }

    // Headless still creates a window — imgui reads its size, and the event pump stays the same shape — it simply
    // never appears and hands out no native handle, which is why the swapchain below is skipped.
    auto wsys = sr::window_system::try_create({.headless = out->_capture.active});
    if (wsys.has_error())
    {
        cc::eprintln("no window backend: {}", wsys.error().to_string());
        return nullptr;
    }
    out->_wsys = cc::move(wsys.value());

    auto const size = out->_capture.active ? out->_capture.size : tg::vec2i(1440, 900);
    out->_win = out->_wsys->create_window({.title = cc::string(title), .width = size[0], .height = size[1]});

    // A real adapter by preference, WARP otherwise: it renders this just as correctly, only slower, so a machine
    // with no usable D3D12 GPU still gets to run the example.
    auto context = sg::create_dx12_context({});
    if (context.has_error())
    {
        cc::println("no hardware D3D12 adapter ({}) — falling back to WARP", context.error().to_string());
        context = sg::create_dx12_context({.use_warp = true});
    }
    if (context.has_error())
    {
        cc::eprintln("no D3D12 device at all: {}", context.error().to_string());
        return nullptr;
    }
    out->_ctx = cc::move(context.value());

    auto compiler = slib::create_dxc_compiler();
    if (compiler.has_error())
    {
        cc::eprintln("no shader compiler: {}", compiler.error().to_string());
        return nullptr;
    }
    out->_lib.add_compiler(cc::move(compiler.value()));
    out->_lib.add_package(sr::shader_package()); // imgui's shaders
    out->_lib.add_package(shaders::package());   // ours

    // bgra8_unorm rather than its _srgb sibling: imgui's colors are already sRGB-encoded and the routine refuses a
    // target that would encode them twice.
    //
    // Under capture there is nothing to present to, so the frame goes into a texture of the same format instead —
    // which is all `end_frame` needs to be told, since a render target is a render target.
    if (out->_capture.active)
    {
        out->_capture_target = out->_ctx->persistent.create_texture_2d(
            {.format = sg::pixel_format::bgra8_unorm,
             .width = size[0],
             .height = size[1],
             .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});
    }
    else
    {
        out->_swapchain = out->_ctx->create_swapchain(
            {.native_window_handle = out->_win->native_window_handle(), .format = sg::pixel_format::bgra8_unorm});
    }

    out->_imgui = sr::imgui_context::create();

    auto scene_renderer = renderer::create(*out->_ctx, out->_lib);
    if (scene_renderer.has_error())
    {
        cc::eprintln("{}", scene_renderer.error().to_string());
        return nullptr;
    }
    out->_renderer = cc::move(scene_renderer.value());

    out->_last_time = now_seconds();
    return out;
}

app::~app()
{
    if (_ctx != nullptr)
        _ctx->advance_epoch_and_wait_for_idle(); // the last frames are still in flight
}

bool app::begin_frame()
{
    // A capture has no window to close, so this is where the run ends: `end_frame` writes the image on the last
    // frame it draws, and the next call through here stops the loop.
    if (_capture.active && _captured_frames >= _capture.accumulate_frames)
        return false;

    if (_win->is_close_requested())
        return false;

    _wsys->poll_events();
    _imgui.process_events(*_wsys); // must precede begin_frame: NewFrame is what commits the input

    auto const now = now_seconds();
    _delta_time = float(now - _last_time);
    _last_time = now;

    // A minimized window is 0x0, and acquire_backbuffer's auto-resize would size the swapchain to zero.
    // A headless window is never minimized, and its size is the one the capture asked for.
    _viewport = _win->is_minimized() ? tg::vec2i(0, 0) : tg::vec2i(_win->width(), _win->height());
    if (_viewport[0] == 0 || _viewport[1] == 0)
        return true;

    _imgui.begin_frame(*_win, _delta_time);
    return true;
}

tg::mat4f app::view_projection(orbit_camera const& cam) const
{
    auto const aspect = _viewport[1] > 0 ? float(_viewport[0]) / float(_viewport[1]) : 1.0f;
    return perspective(this->vertical_fov(), aspect, 0.1f, 500.0f) * cam.view();
}

void app::end_frame(vdoc::document const& doc, orbit_camera const& cam, vdoc::entity_id selected)
{
    if (_viewport[0] == 0 || _viewport[1] == 0)
        return; // minimized: begin_frame never opened an imgui frame either

    _imgui.end_frame();

    // The frame's output: a back buffer normally, the capture texture when there is no display.
    // Everything below is written against a render target and cannot tell which it got.
    auto rt = _capture.active ? _capture_target.as_render_target_view() : _swapchain->acquire_backbuffer();
    auto cmd = _ctx->create_command_list();
    {
        // Transient, so it is sized to this frame's backbuffer and recycled with the epoch — which is also what
        // makes a window resize need no handling at all.
        auto const depth = _ctx->transient.create_texture_2d({.format = sg::pixel_format::depth32_float,
                                                              .width = rt.width(),
                                                              .height = rt.height(),
                                                              .usage = sg::texture_usage::depth_stencil});

        auto pass = cmd->raster.render_to({.color_targets = {rt.cleared(tg::vec4f(0.043f, 0.051f, 0.071f, 1.0f))},
                                           .depth_stencil_target = depth.as_depth_stencil_view().cleared(1.0f)});
        _renderer->draw(pass, doc, this->view_projection(cam), selected);
    }
    {
        // imgui gets its own scope, with no depth target: a pipeline bakes in the formats it was built against, and
        // imgui's carries no depth format at all — binding one here would not match the pipeline the routine uses.
        // `preserved()` is what keeps the scene that the scope above just drew.
        auto pass = cmd->raster.render_to({.color_targets = {rt.preserved()}});
        sr::imgui_routine::execute(pass, ImGui::GetDrawData());
    }
    if (!_capture.active)
    {
        _ctx->submit_command_list_and_present(*_swapchain, cc::move(cmd));
        _ctx->advance_epoch(_swapchain->buffer_count());
        return;
    }

    _ctx->submit_command_list(cc::move(cmd));
    _ctx->advance_epoch(2);
    ++_captured_frames;

    // Written on the last frame rather than after the loop, because the loop is the caller's and this is the only
    // place that knows the frame just drawn was the final one.
    //
    // No convergence to wait for here: this is a raster pass, so the image is finished the moment it is drawn, and
    // the frame count exists to let imgui settle its layout rather than to let an estimator converge.
    if (_captured_frames < _capture.accumulate_frames)
        return;

    auto const written = sr::write_capture_image(*_ctx, _capture_target, _viewport, _capture.output_path);
    if (written.has_error())
        cc::eprintln("capture failed: {}", written.error().to_string());
}
} // namespace cube_editor
