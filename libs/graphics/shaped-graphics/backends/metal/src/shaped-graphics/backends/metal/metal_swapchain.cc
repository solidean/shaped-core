#include "metal_swapchain.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>
#include <shaped-graphics/backends/metal/metal_format.hh>
#include <shaped-graphics/backends/metal/metal_texture.hh>
#include <shaped-graphics/exceptions.hh> // sg::device_lost_exception, thrown when acquiring on a lost device

namespace sg::backend::metal
{
metal_swapchain::metal_swapchain(metal_context& ctx, sg::swapchain_description const& desc, CA::MetalLayer* layer)
  : sg::swapchain(desc), _ctx(ctx), _layer(layer)
{
    auto const initial = desc.is_windowed() ? desc.window.client_size : desc.headless_extent.value();
    recreate(initial[0] > 0 && initial[1] > 0 ? initial : tg::vec2i(1, 1));
}

metal_swapchain::~metal_swapchain()
{
    if (_drawable != nullptr)
    {
        _drawable->release();
        _drawable = nullptr;
    }
    if (_layer != nullptr)
    {
        _layer->release();
        _layer = nullptr;
    }
}

void metal_swapchain::recreate(tg::vec2i size)
{
    _size = size;
    _buffers.clear();
    _next_buffer = 0;

    if (_layer != nullptr)
    {
        _layer->setDrawableSize(CGSize{double(size[0]), double(size[1])});
        return;
    }

    // Headless: ordinary render targets standing in for back buffers.
    // They carry copy_src as well, because the whole point of a headless chain is that the presented image stays
    // readable — a null target would only prove the calls did not crash.
    for (auto i = 0; i < _desc.buffer_count; ++i)
    {
        auto texture = _ctx.create_metal_texture(
            {
                .format = _desc.format,
                .width = size[0],
                .height = size[1],
                .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src,
            },
            {});
        CC_ASSERT(texture.has_value(), "headless swapchain could not allocate a back buffer");
        _buffers.push_back(sg::raw_texture_handle(cc::move(texture.value())));
    }
}

sg::render_target_view metal_swapchain::acquire_backbuffer()
{
    if (_ctx.is_device_lost())
        throw sg::device_lost_exception(_ctx.device_loss_reason());

    auto const scope = autorelease_scope();

    if (_layer != nullptr)
    {
        // At most once per epoch, matching the contract every backend's chain follows.
        if (_last_resize_check != _ctx.current_epoch())
        {
            _last_resize_check = _ctx.current_epoch();
            if (_requested_size[0] > 0 && _requested_size[1] > 0 && _requested_size != _size)
                recreate(_requested_size);
        }

        CC_ASSERT(_drawable == nullptr, "a back buffer is already acquired; present it before acquiring another");

        // Retained: nextDrawable hands back an autoreleased object, and this one outlives the pool above by design,
        // being held until present.
        // See the render encoder for the same trap, found the hard way.
        _drawable = _layer->nextDrawable()->retain();
        CC_ASSERT(_drawable != nullptr, "the metal layer had no drawable");

        auto const size = _layer->drawableSize();
        _size = tg::vec2i(int(size.width), int(size.height));

        // The drawable's texture is owned by the drawable, so it is wrapped without ownership rather than adopted.
        auto texture = std::make_shared<metal_texture const>(_ctx,
                                                             sg::texture_description{
                                                                 .format = _desc.format,
                                                                 .width = _size[0],
                                                                 .height = _size[1],
                                                                 .usage = sg::texture_usage::render_target,
                                                             },
                                                             _drawable->texture()->retain());

        return sg::render_target_view(sg::raw_texture_handle(cc::move(texture)), sg::texture_view_dimension::tex_2d,
                                      _desc.format, sg::subresource_range());
    }

    auto const index = _next_buffer;
    _next_buffer = (_next_buffer + 1) % int(_buffers.size());

    return sg::render_target_view(_buffers[index], sg::texture_view_dimension::tex_2d, _desc.format,
                                  sg::subresource_range());
}

void metal_swapchain::record_present_transition(command_list&)
{
    // Nothing to record: a Metal texture has no layout, so there is no present layout to transition into.
    //
    // dx12 and vulkan both emit a real barrier here, and the split exists for vulkan's semaphores — which this backend
    // does not need either, because MTL4's drawable handshake is on the queue rather than on the submit.
}

void metal_swapchain::present()
{
    if (_layer == nullptr)
        return; // a headless chain rotates its index at acquire and has nothing to hand a display

    CC_ASSERT(_drawable != nullptr, "present without a matching acquire_backbuffer");

    auto const scope = autorelease_scope();

    // The queue-side half of the handshake: everything committed before this is what the drawable displays.
    _ctx.queue()->signalDrawable(_drawable);
    _drawable->present();

    _drawable->release();
    _drawable = nullptr;
}

cc::result<sg::swapchain_handle> metal_context::create_metal_swapchain(sg::swapchain_description const& desc)
{
    desc.assert_valid();

    if (pixel_format_of(desc.format) == MTL::PixelFormatInvalid)
        return cc::error("swapchain: the requested format has no metal equivalent");

    auto const scope = autorelease_scope();

    CA::MetalLayer* layer = nullptr;
    if (desc.is_windowed())
    {
        if (desc.window.platform != sg::window_platform::cocoa)
            return cc::error("swapchain: the metal backend presents to a CAMetalLayer, which is "
                             "window_platform::cocoa");

        // sg is handed the layer itself rather than a window, so there is no view to walk and no Objective-C to write.
        layer = static_cast<CA::MetalLayer*>(desc.window.handle)->retain();
        layer->setDevice(_device);
        layer->setPixelFormat(pixel_format_of(desc.format));
        layer->setMaximumDrawableCount(NS::UInteger(desc.buffer_count));

        // False, because sg lets a caller read a presented image back — which is what a headless chain is for and what
        // a capture does.
        // framebufferOnly would make that illegal.
        layer->setFramebufferOnly(false);
    }

    return sg::swapchain_handle(std::make_shared<metal_swapchain>(*this, desc, layer));
}
} // namespace sg::backend::metal
