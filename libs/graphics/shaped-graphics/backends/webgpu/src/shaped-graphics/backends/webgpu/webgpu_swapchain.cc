// webgpu_swapchain: headless rotation, and a web canvas surface.

#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh>
#include <shaped-graphics/backends/webgpu/webgpu_format.hh>
#include <shaped-graphics/exceptions.hh>

namespace sg::backend::webgpu
{
webgpu_swapchain::webgpu_swapchain(webgpu_context& ctx, sg::swapchain_description const& desc)
  : sg::swapchain(desc), _ctx(ctx)
{
}

cc::result<webgpu_swapchain_handle> webgpu_swapchain::create(webgpu_context& ctx, sg::swapchain_description const& desc)
{
    desc.assert_valid();
    auto chain = std::make_shared<webgpu_swapchain>(ctx, desc);

    auto const usage = sg::texture_usage::render_target | sg::texture_usage::copy_src | sg::texture_usage::texture;
    if (!desc.is_windowed())
    {
        auto const extent = desc.headless_extent.value();
        for (int i = 0; i < desc.buffer_count; ++i)
        {
            auto texture = ctx.create_webgpu_texture(
                sg::texture_description{
                    .format = desc.format,
                    .dimension = sg::texture_dimension::d2,
                    .width = extent[0],
                    .height = extent[1],
                    .usage = usage,
                },
                {});
            CC_RETURN_IF_ERROR(texture);
            chain->_buffers.push_back(cc::move(texture.value()));
        }
        return chain;
    }

    if (desc.window.platform != sg::window_platform::web_canvas)
        return cc::error("webgpu presents only to a web canvas (sg::window_platform::web_canvas) or headlessly");

    auto source = WGPUEmscriptenSurfaceSourceCanvasHTMLSelector{};
    source.chain.next = nullptr;
    source.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
    source.selector = to_wgpu(cc::string_view(static_cast<char const*>(desc.window.handle)));
    auto const surface_desc = WGPUSurfaceDescriptor{.nextInChain = &source.chain, .label = to_wgpu("sg canvas")};
    chain->_surface = wgpu_surface(wgpuInstanceCreateSurface(ctx.instance(), &surface_desc));
    if (!chain->_surface)
        return cc::error(cc::format("no canvas matches the selector '{}'", static_cast<char const*>(desc.window.handle)));

    chain->_requested_size = desc.window.client_size;
    chain->configure_surface(desc.window.client_size);
    return chain;
}

void webgpu_swapchain::configure_surface(tg::vec2i size)
{
    auto const config = WGPUSurfaceConfiguration{
        .nextInChain = nullptr,
        .device = _ctx.device(),
        .format = to_wgpu_format(_desc.format),
        .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc,
        .width = u32(size[0]),
        .height = u32(size[1]),
        .viewFormatCount = 0,
        .viewFormats = nullptr,
        .alphaMode = WGPUCompositeAlphaMode_Opaque,
        .presentMode = WGPUPresentMode_Fifo,
    };
    wgpuSurfaceConfigure(_surface.get(), &config);
    _configured_size = size;
}

sg::render_target_view webgpu_swapchain::acquire_backbuffer()
{
    _ctx.assert_on_device_thread();
    if (_ctx.is_device_lost())
        throw sg::device_lost_exception(_ctx.device_loss_reason());

    auto const whole = sg::subresource_range();
    if (!is_windowed())
    {
        auto const& buffer = _buffers[_current];
        return sg::render_target_view(buffer, sg::texture_view_dimension::tex_2d, _desc.format, whole);
    }

    // A canvas has no size of its own: the application's last word is the size, checked once per epoch.
    if (_last_resize_check != _ctx.current_epoch())
    {
        _last_resize_check = _ctx.current_epoch();
        if (_requested_size[0] > 0 && _requested_size[1] > 0 && _requested_size != _configured_size)
            configure_surface(_requested_size);
    }

    // What the canvas hands out comes from outside the program, so a failure is an sg::exception rather than an assert.
    // A stale surface is reconfigured and asked once more, as vulkan rebuilds an out-of-date chain.
    auto const is_success = [](WGPUSurfaceGetCurrentTextureStatus status)
    {
        return status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal
            || status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal;
    };
    auto surface_texture = WGPUSurfaceTexture{};
    wgpuSurfaceGetCurrentTexture(_surface.get(), &surface_texture);
    if (surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Outdated
        || surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Lost)
    {
        configure_surface(_configured_size);
        surface_texture = WGPUSurfaceTexture{};
        wgpuSurfaceGetCurrentTexture(_surface.get(), &surface_texture);
    }
    if (!is_success(surface_texture.status))
    {
        if (_ctx.is_device_lost())
            throw sg::device_lost_exception(_ctx.device_loss_reason());
        throw sg::exception(
            cc::format("the canvas surface handed out no texture (status {})", int(surface_texture.status)));
    }

    _surface_texture
        = std::make_shared<webgpu_texture>(_ctx,
                                           sg::texture_description{
                                               .format = _desc.format,
                                               .dimension = sg::texture_dimension::d2,
                                               .width = _configured_size[0],
                                               .height = _configured_size[1],
                                               .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src,
                                           },
                                           wgpu_texture(surface_texture.texture), false);
    return sg::render_target_view(_surface_texture, sg::texture_view_dimension::tex_2d, _desc.format, whole);
}

void webgpu_swapchain::record_present_transition(sg::command_list&)
{
    // WebGPU has no present layout to transition to.
}

void webgpu_swapchain::present()
{
    _ctx.assert_on_device_thread();
    if (!is_windowed())
    {
        _current = (_current + 1) % int(_buffers.size());
        return;
    }

    // The page composites the canvas once the task that rendered it returns, so there is nothing to call.
    _surface_texture = nullptr;
}
} // namespace sg::backend::webgpu
