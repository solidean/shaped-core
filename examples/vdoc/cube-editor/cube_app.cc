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

    auto wsys = sr::window_system::try_create();
    if (wsys.has_error())
    {
        cc::eprintln("no window backend: {}", wsys.error().to_string());
        return nullptr;
    }
    out->_wsys = cc::move(wsys.value());
    out->_win = out->_wsys->create_window({.title = cc::string(title), .width = 1440, .height = 900});

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

    out->_default_pool = cc::make_unique<cc::scoped_default_async_pool>(out->_pool);

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
    out->_swapchain = out->_ctx->create_swapchain(
        {.native_window_handle = out->_win->native_window_handle(), .format = sg::pixel_format::bgra8_unorm});

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
    if (_win->is_close_requested())
        return false;

    _wsys->poll_events();
    _imgui.process_events(*_wsys); // must precede begin_frame: NewFrame is what commits the input

    auto const now = now_seconds();
    _delta_time = float(now - _last_time);
    _last_time = now;

    // A minimized window is 0x0, and acquire_backbuffer's auto-resize would size the swapchain to zero.
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

    auto rt = _swapchain->acquire_backbuffer();
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
    _ctx->submit_command_list_and_present(*_swapchain, cc::move(cmd));
    _ctx->advance_epoch(_swapchain->buffer_count());
}
} // namespace cube_editor
