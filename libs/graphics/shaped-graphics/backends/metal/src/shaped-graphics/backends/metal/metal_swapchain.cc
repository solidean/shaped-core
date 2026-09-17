#include "metal_swapchain.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>
#include <shaped-graphics/backends/metal/metal_format.hh>
#include <shaped-graphics/backends/metal/metal_texture.hh>
#include <shaped-graphics/exceptions.hh> // sg::device_lost_exception, thrown when acquiring on a lost device

namespace sg::backend::metal
{
namespace
{
/// The layer's drawable size, in whole texels.
[[nodiscard]] tg::vec2i layer_size(CA::MetalLayer const* layer)
{
    auto const size = layer->drawableSize();
    return tg::vec2i(int(size.width), int(size.height));
}
} // namespace

metal_swapchain::metal_swapchain(metal_context& ctx, sg::swapchain_description const& desc, CA::MetalLayer* layer)
  : sg::swapchain(desc), _ctx(ctx), _layer(layer)
{
    // **A windowed chain's size is the layer's, not the description's.**
    // `client_size` is documented as needed by wayland alone and is 0 everywhere else, so taking it here gave every
    // cocoa chain a 1x1 surface — and nothing but `set_window_size` ever changed it.
    // `drawableSize` is what the layer will actually hand out, and it already tracks the window.
    auto const initial = layer != nullptr ? layer_size(layer) : desc.headless_extent.value();
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

            // An explicit request wins, and otherwise the layer's own size does: a cocoa window resizes the layer
            // without anyone telling sg, and `set_window_size` is the wayland-shaped path rather than the usual one.
            auto const target = _requested_size[0] > 0 && _requested_size[1] > 0 ? _requested_size : layer_size(_layer);
            if (target[0] > 0 && target[1] > 0 && target != _size)
                recreate(target);
        }

        CC_ASSERT(_drawable == nullptr, "a back buffer is already acquired; present it before acquiring another");

        // Retained: nextDrawable hands back an autoreleased object, and this one outlives the pool above by design,
        // being held until present.
        // See the render encoder for the same trap, found the hard way.
        // **Checked before it is retained.** `nextDrawable` returns nil on an occluded window after about a second,
        // and messaging `retain` on nil is a silent null rather than a crash — so the assert below never fired and the
        // nil travelled on into the texture wrapper.
        auto* const next = _layer->nextDrawable();
        if (next == nullptr)
            throw sg::exception("the metal layer had no drawable — the window may be occluded or the chain stale");

        // Retained: nextDrawable hands back an autoreleased object, and this one outlives the pool above by design,
        // being held until present.
        _drawable = next->retain();

        _size = layer_size(_layer);

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
    // No barrier to record: a Metal texture has no layout, so there is no present layout to transition into.
    //
    // **What this slot is for here is the waiting half of the drawable handshake.**
    // `waitForDrawable` must run before the work that draws into the drawable and `signalDrawable` after it, which is
    // exactly the room sg's split leaves — and the header has described that pairing since this backend was written
    // while only the signal was ever emitted.
    // A queue wait applies to what is committed after it, so issuing it while the frame's list is still recording is
    // what puts it ahead of that list's commit.
    if (_drawable != nullptr)
        _ctx.queue()->wait(_drawable);
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

        // **A CAMetalLayer accepts a fixed set of formats and raises an Objective-C exception on any other**, which no
        // `cc::result` can catch and no validation layer reports first.
        // Apple's list for `CAMetalLayer.pixelFormat`, in full.
        auto const layer_format_is_allowed = [](sg::pixel_format f)
        {
            switch (f)
            {
            case sg::pixel_format::bgra8_unorm:
            case sg::pixel_format::bgra8_unorm_srgb:
            case sg::pixel_format::rgba16_float:
            case sg::pixel_format::rgb10a2_unorm:
                return true;
            default:
                // The remaining four Apple names — BGR10A2Unorm and the three XR variants — have no sg format.
                return false;
            }
        };
        if (!layer_format_is_allowed(desc.format))
            return cc::error(cc::format("swapchain: pixel format {} is not one a CAMetalLayer accepts — "
                                        "bgra8_unorm, bgra8_unorm_srgb, rgba16_float or rgb10a2_unorm",
                                        int(desc.format)));

        // EDR needs `wantsExtendedDynamicRangeContent` and a matching colorspace, neither of which metal-cpp exposes.
        // Refused rather than accepted and ignored: a caller asking for HDR and silently getting SDR has no way to
        // find that out.
        if (desc.enable_hdr)
            return cc::error("swapchain: the metal backend has no EDR path yet, so enable_hdr cannot be honoured");

        // sg is handed the layer itself rather than a window, so there is no view to walk and no Objective-C to write.
        layer = static_cast<CA::MetalLayer*>(desc.window.handle)->retain();
        layer->setDevice(_device);
        layer->setPixelFormat(pixel_format_of(desc.format));

        // Two or three, and anything else raises rather than clamping itself — the same clamp vulkan applies against
        // its surface capabilities.
        layer->setMaximumDrawableCount(NS::UInteger(cc::clamp(desc.buffer_count, 2, 3)));

        // vsync is the layer's default; `immediate` is display sync off, which is what lets a frame tear.
        layer->setDisplaySyncEnabled(desc.present_mode == sg::present_mode::vsync);

        // False, because sg lets a caller read a presented image back — which is what a headless chain is for and what
        // a capture does.
        // framebufferOnly would make that illegal.
        layer->setFramebufferOnly(false);
    }

    return sg::swapchain_handle(std::make_shared<metal_swapchain>(*this, desc, layer));
}
} // namespace sg::backend::metal
