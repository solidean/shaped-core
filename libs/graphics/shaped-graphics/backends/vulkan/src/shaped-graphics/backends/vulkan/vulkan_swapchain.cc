// vulkan_swapchain: the VkSwapchainKHR half of presentation, plus its surface.
// See libs/graphics/shaped-graphics/docs/concepts/presentation.md.

#include <clean-core/common/assert.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/backends/vulkan/vulkan_barrier.hh>
#include <shaped-graphics/backends/vulkan/vulkan_command_list.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_format.hh>
#include <shaped-graphics/backends/vulkan/vulkan_swapchain.hh>
#include <shaped-graphics/backends/vulkan/vulkan_texture.hh>
#include <shaped-graphics/exceptions.hh>

namespace sg::backend::vulkan
{
namespace
{
/// The surface for `desc`, or an error naming what is missing.
///
/// A headless chain takes VK_EXT_headless_surface, which is a real surface with no display rather than an emulation —
/// so the whole acquire / present handshake is exercised either way.
[[nodiscard]] cc::result<VkSurfaceKHR> create_surface(vulkan_context& ctx, sg::swapchain_description const& desc)
{
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    if (!desc.is_windowed())
    {
        if (!ctx.is_headless_present_supported())
            return cc::error("this Vulkan instance has no VK_EXT_headless_surface, so it cannot present headlessly");

        auto const fn
            = PFN_vkCreateHeadlessSurfaceEXT(vkGetInstanceProcAddr(ctx._instance, "vkCreateHeadlessSurfaceEXT"));
        if (fn == nullptr)
            return cc::error("VK_EXT_headless_surface is advertised but vkCreateHeadlessSurfaceEXT is missing");

        auto const info = VkHeadlessSurfaceCreateInfoEXT{
            .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT,
        };
        if (VkResult const r = fn(ctx._instance, &info, nullptr, &surface); r != VK_SUCCESS)
            return vulkan_error(r, "vkCreateHeadlessSurfaceEXT failed");
        return surface;
    }

    // One call per windowing system, each behind the instance extension for it.
    // The platform tag is what makes this a switch rather than a guess: sg carries the window in its own system's
    // terms, so there is nothing to infer here.
    switch (desc.window.platform)
    {
    case sg::window_platform::win32:
    {
#ifdef VK_USE_PLATFORM_WIN32_KHR
        if (!ctx.is_window_platform_supported(sg::window_platform::win32))
            return cc::error("this Vulkan instance has no VK_KHR_win32_surface");
        auto const info = VkWin32SurfaceCreateInfoKHR{
            .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
            .hinstance = GetModuleHandleW(nullptr),
            .hwnd = HWND(desc.window.handle),
        };
        if (VkResult const r = vkCreateWin32SurfaceKHR(ctx._instance, &info, nullptr, &surface); r != VK_SUCCESS)
            return vulkan_error(r, "vkCreateWin32SurfaceKHR failed");
        return surface;
#else
        return cc::error("this build has no Win32 surface support");
#endif
    }
    case sg::window_platform::xlib:
    {
#ifdef VK_USE_PLATFORM_XLIB_KHR
        if (!ctx.is_window_platform_supported(sg::window_platform::xlib))
            return cc::error("this Vulkan instance has no VK_KHR_xlib_surface");
        auto const info = VkXlibSurfaceCreateInfoKHR{
            .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
            .dpy = static_cast<Display*>(desc.window.display),
            .window = ::Window(desc.window.window_id),
        };
        if (VkResult const r = vkCreateXlibSurfaceKHR(ctx._instance, &info, nullptr, &surface); r != VK_SUCCESS)
            return vulkan_error(r, "vkCreateXlibSurfaceKHR failed");
        return surface;
#else
        return cc::error("this build has no Xlib surface support");
#endif
    }
    case sg::window_platform::xcb:
    {
#ifdef VK_USE_PLATFORM_XCB_KHR
        if (!ctx.is_window_platform_supported(sg::window_platform::xcb))
            return cc::error("this Vulkan instance has no VK_KHR_xcb_surface");
        auto const info = VkXcbSurfaceCreateInfoKHR{
            .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
            .connection = static_cast<xcb_connection_t*>(desc.window.display),
            .window = xcb_window_t(desc.window.window_id),
        };
        if (VkResult const r = vkCreateXcbSurfaceKHR(ctx._instance, &info, nullptr, &surface); r != VK_SUCCESS)
            return vulkan_error(r, "vkCreateXcbSurfaceKHR failed");
        return surface;
#else
        return cc::error("this build has no XCB surface support");
#endif
    }
    case sg::window_platform::wayland:
    {
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
        if (!ctx.is_window_platform_supported(sg::window_platform::wayland))
            return cc::error("this Vulkan instance has no VK_KHR_wayland_surface");
        auto const info = VkWaylandSurfaceCreateInfoKHR{
            .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
            .display = static_cast<wl_display*>(desc.window.display),
            .surface = static_cast<wl_surface*>(desc.window.handle),
        };
        if (VkResult const r = vkCreateWaylandSurfaceKHR(ctx._instance, &info, nullptr, &surface); r != VK_SUCCESS)
            return vulkan_error(r, "vkCreateWaylandSurfaceKHR failed");
        return surface;
#else
        return cc::error("this build has no Wayland surface support");
#endif
    }
    }
    return cc::error("unhandled window platform");
}

/// The surface format matching `format`, or an error when the surface does not offer it.
[[nodiscard]] cc::result<VkSurfaceFormatKHR> pick_surface_format(vulkan_context& ctx,
                                                                 VkSurfaceKHR surface,
                                                                 sg::pixel_format format,
                                                                 bool want_hdr)
{
    u32 count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx._physical_device, surface, &count, nullptr);
    auto formats = cc::vector<VkSurfaceFormatKHR>::create_uninitialized(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx._physical_device, surface, &count, formats.data());

    auto const wanted = to_vk_format(format);
    auto const wanted_space = want_hdr ? VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

    // The requested colorspace first, then the format in any colorspace: HDR is best-effort at the sg level, so a
    // surface that has the format but not the space is a success rather than a failure.
    for (auto const& f : formats)
        if (f.format == wanted && f.colorSpace == wanted_space)
            return f;
    for (auto const& f : formats)
        if (f.format == wanted)
            return f;

    return cc::error(cc::format("the surface does not offer the requested swapchain format ({} candidates)", count));
}
} // namespace

cc::result<vulkan_swapchain_handle> vulkan_swapchain::create(vulkan_context& ctx, sg::swapchain_description const& desc)
{
    desc.assert_valid();
    if (!ctx.is_swapchain_supported())
        return cc::error("this Vulkan device has no VK_KHR_swapchain, so it cannot present");

    auto surface_result = create_surface(ctx, desc);
    CC_RETURN_IF_ERROR(surface_result);

    auto chain = std::make_shared<vulkan_swapchain>(ctx, desc, surface_result.value());

    // The queue family must actually be able to present to this surface.
    VkBool32 present_supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(ctx._physical_device, ctx._queue_family_index, chain->_surface,
                                         &present_supported);
    if (present_supported != VK_TRUE)
        return cc::error("the context's queue family cannot present to this surface");

    auto format_result = pick_surface_format(ctx, chain->_surface, desc.format, desc.enable_hdr);
    CC_RETURN_IF_ERROR(format_result);
    chain->_vk_format = format_result.value().format;

    if (auto r = chain->build(chain->current_extent()); r.has_error())
        return cc::error(cc::move(r).error());

    return vulkan_swapchain_handle(cc::move(chain));
}

tg::vec2i vulkan_swapchain::current_extent() const
{
    if (!_desc.is_windowed())
        return _desc.headless_extent.value();

    // A windowed chain follows its surface where the surface knows its own size — Vulkan reports it here rather than
    // making the app ask the window system, which is one platform call fewer than dx12 needs.
    VkSurfaceCapabilitiesKHR caps = {};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_ctx._physical_device, _surface, &caps);
    if (caps.currentExtent.width != UINT32_MAX)
        return tg::vec2i(int(caps.currentExtent.width), int(caps.currentExtent.height));

    // UINT32_MAX means the surface defers to us, which on wayland is always: a wl_surface has no size of its own, and
    // the compositor takes whatever the chain is built at.
    // So the application is the only authority, and `set_window_size` (seeded from native_window::client_size) is how
    // it says so.
    // Falling back to `_size` without one is what leaves a chain built with neither at the 1x1 minImageExtent.
    if (_requested_size[0] > 0 && _requested_size[1] > 0)
        return _requested_size;
    return _size;
}

cc::result<cc::unit> vulkan_swapchain::build(tg::vec2i size)
{
    VkSurfaceCapabilitiesKHR caps = {};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_ctx._physical_device, _surface, &caps);

    auto const clamp = [](int v, u32 lo, u32 hi) { return u32(v) < lo ? lo : (u32(v) > hi ? hi : u32(v)); };
    auto const extent = VkExtent2D{
        .width = clamp(size[0], caps.minImageExtent.width, caps.maxImageExtent.width),
        .height = clamp(size[1], caps.minImageExtent.height, caps.maxImageExtent.height),
    };

    u32 image_count = u32(_desc.buffer_count);
    if (image_count < caps.minImageCount)
        image_count = caps.minImageCount;
    if (caps.maxImageCount != 0 && image_count > caps.maxImageCount)
        image_count = caps.maxImageCount;

    // FIFO is the vsync mode every implementation must offer; immediate is a request, so fall back rather than fail.
    auto present_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (_desc.present_mode == sg::present_mode::immediate)
    {
        u32 mode_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(_ctx._physical_device, _surface, &mode_count, nullptr);
        auto modes = cc::vector<VkPresentModeKHR>::create_uninitialized(mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(_ctx._physical_device, _surface, &mode_count, modes.data());
        for (auto const m : modes)
            if (m == VK_PRESENT_MODE_IMMEDIATE_KHR)
                present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }

    VkSwapchainKHR const old_chain = _swapchain;
    auto const info = VkSwapchainCreateInfoKHR{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = _surface,
        .minImageCount = image_count,
        .imageFormat = _vk_format,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        // TRANSFER_SRC is what makes a presented image readable, which is the whole point of a headless chain being
        // pixel-verifiable rather than merely non-crashing.
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = present_mode,
        .clipped = VK_TRUE,
        // Handing the old chain over lets the driver reuse it, and is what makes a resize legal while its images are
        // still referenced.
        .oldSwapchain = old_chain,
    };

    VkSwapchainKHR created = VK_NULL_HANDLE;
    if (VkResult const r = vkCreateSwapchainKHR(_ctx._device, &info, nullptr, &created); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateSwapchainKHR failed");

    // The old chain and its wrappers go now: the caller drained the GPU before a rebuild, so nothing is in flight.
    _backbuffers.clear();
    if (old_chain != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(_ctx._device, old_chain, nullptr);
    _swapchain = created;
    _size = tg::vec2i(int(extent.width), int(extent.height));

    u32 actual_count = 0;
    vkGetSwapchainImagesKHR(_ctx._device, _swapchain, &actual_count, nullptr);
    auto images = cc::vector<VkImage>::create_uninitialized(actual_count);
    vkGetSwapchainImagesKHR(_ctx._device, _swapchain, &actual_count, images.data());

    // Each image becomes a texture with borrowed storage, so a back buffer flows through the ordinary rendering and
    // barrier path — which is what leaves its canonical layout at `present` for the next frame.
    // The images start UNDEFINED, which is exactly what the access tracker seeds a texture with.
    sg::texture_description tex_desc;
    tex_desc.format = _desc.format;
    tex_desc.dimension = sg::texture_dimension::d2;
    tex_desc.width = _size[0];
    tex_desc.height = _size[1];
    tex_desc.usage = sg::texture_usage::render_target | sg::texture_usage::copy_src;
    for (auto const image : images)
        _backbuffers.push_back(std::make_shared<vulkan_texture>(_ctx, _ctx.current_epoch(), tex_desc, image,
                                                                VK_NULL_HANDLE, /*owns_image =*/false));

    // Semaphores: one acquire per frame in flight, one render-finished per image (see the members' note).
    for (auto const s : _acquire_semaphores)
        vkDestroySemaphore(_ctx._device, s, nullptr);
    for (auto const s : _render_semaphores)
        vkDestroySemaphore(_ctx._device, s, nullptr);
    _acquire_semaphores.clear();
    _render_semaphores.clear();
    _frame = 0;

    auto const semaphore_info = VkSemaphoreCreateInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (isize i = 0; i < _backbuffers.size(); ++i)
    {
        VkSemaphore acquire = VK_NULL_HANDLE;
        VkSemaphore render = VK_NULL_HANDLE;
        if (vkCreateSemaphore(_ctx._device, &semaphore_info, nullptr, &acquire) != VK_SUCCESS
            || vkCreateSemaphore(_ctx._device, &semaphore_info, nullptr, &render) != VK_SUCCESS)
            return cc::error("vkCreateSemaphore failed for a swapchain semaphore");
        _acquire_semaphores.push_back(acquire);
        _render_semaphores.push_back(render);
    }

    return cc::unit{};
}

sg::render_target_view vulkan_swapchain::acquire_backbuffer()
{
    CC_RECORD_SCOPE("sg.swapchain.acquire_backbuffer");
    CC_ASSERT(!_acquired, "acquire_backbuffer() called twice without an intervening present()");

    // Auto-resize to the window, but only the first acquire of each epoch checks, so a resize — which drains the GPU
    // — never advances an epoch under the caller.
    // A headless chain never resizes: there is no window to follow, and its extent is the one it was created with.
    if (_desc.is_windowed())
        if (sg::epoch const epoch = _ctx.current_epoch(); epoch != _last_resize_epoch)
        {
            _last_resize_epoch = epoch;
            if (tg::vec2i const client = current_extent(); client != _size && client[0] > 0 && client[1] > 0)
            {
                _ctx.queue_guard().lock([&](int&) { vkDeviceWaitIdle(_ctx._device); });
                if (auto r = build(client); r.has_error())
                    throw sg::exception(cc::format("swapchain resize failed: {}", cc::move(r).error()));
            }
        }

    VkSemaphore const acquire_semaphore = _acquire_semaphores[_frame];
    VkResult r = vkAcquireNextImageKHR(_ctx._device, _swapchain, UINT64_MAX, acquire_semaphore, VK_NULL_HANDLE,
                                       &_acquired_index);

    // OUT_OF_DATE has no DXGI counterpart: the surface itself says the chain is stale, where dx12 has to notice by
    // polling the window's client area.
    // Rebuild and retry once — twice in a row means something other than a resize is wrong.
    if (r == VK_ERROR_OUT_OF_DATE_KHR)
    {
        _ctx.queue_guard().lock([&](int&) { vkDeviceWaitIdle(_ctx._device); });
        if (auto rebuilt = build(current_extent()); rebuilt.has_error())
            throw sg::exception(cc::format("swapchain rebuild failed: {}", cc::move(rebuilt).error()));
        r = vkAcquireNextImageKHR(_ctx._device, _swapchain, UINT64_MAX, _acquire_semaphores[_frame], VK_NULL_HANDLE,
                                  &_acquired_index);
    }

    // SUBOPTIMAL is a successful acquire: the image is usable, and the chain is rebuilt at the next resize check.
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
    {
        if (_ctx.note_device_lost_if_lost(r, "vkAcquireNextImageKHR"))
            throw sg::device_lost_exception(_ctx.device_loss_reason());
        throw sg::exception("vkAcquireNextImageKHR failed");
    }

    _acquired = true;
    return sg::render_target_view(_backbuffers[_acquired_index], sg::texture_view_dimension::tex_2d, _desc.format,
                                  sg::subresource_range{});
}

void vulkan_swapchain::record_present_transition(sg::command_list& cmd)
{
    CC_ASSERT(_acquired, "record_present_transition without a matching acquire_backbuffer()");
    auto* const vk = dynamic_cast<vulkan_command_list*>(&cmd);
    CC_ASSERT(vk != nullptr, "command list is not a vulkan command list");

    // Transition the acquired back buffer to the present layout on the caller's still-open list.
    // Going through the tracker computes it from whatever layout the frame left it in, and leaves the canonical
    // layout at `present` for next frame.
    // A pure layout transition: no destination stage or access, since the consumer is the presentation engine rather
    // than anything on this queue's timeline.
    auto const& backbuffer = *_backbuffers[_acquired_index];
    auto const whole = sg::subresource_range::whole(subresource_extent_of(backbuffer.description()));
    vk->track_texture_access(backbuffer, whole, {}, {}, sg::texture_layout::present);
    vk->flush_barriers();

    // This submit is the present handshake's other half — see the members on vulkan_command_list.
    vk->_present_wait = _acquire_semaphores[_frame];
    vk->_present_signal = _render_semaphores[_acquired_index];
}

void vulkan_swapchain::present()
{
    CC_RECORD_SCOPE("sg.swapchain.present");
    CC_ASSERT(_acquired, "present() without a matching acquire_backbuffer()");
    _acquired = false;

    auto const info = VkPresentInfoKHR{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &_render_semaphores[_acquired_index],
        .swapchainCount = 1,
        .pSwapchains = &_swapchain,
        .pImageIndices = &_acquired_index,
    };
    VkResult const r = _ctx.queue_guard().lock([&](int&) { return vkQueuePresentKHR(_ctx._queue, &info); });

    // Both of these mean "rebuild", which the next acquire does — presenting is not the place to drain the GPU.
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR && r != VK_ERROR_OUT_OF_DATE_KHR)
    {
        if (_ctx.note_device_lost_if_lost(r, "vkQueuePresentKHR"))
            throw sg::device_lost_exception(_ctx.device_loss_reason());
        throw sg::exception("vkQueuePresentKHR failed");
    }

    _frame = (_frame + 1) % int(_acquire_semaphores.size());
}

vulkan_swapchain::~vulkan_swapchain()
{
    // Borrowed images and semaphores the presentation engine may still hold, so this waits rather than deferring:
    // a swapchain outlives no epoch, and its images are not ours to hand to the deletion queue.
    if (_ctx._device != VK_NULL_HANDLE)
        _ctx.queue_guard().lock([&](int&) { vkDeviceWaitIdle(_ctx._device); });

    _backbuffers.clear();
    for (auto const s : _acquire_semaphores)
        vkDestroySemaphore(_ctx._device, s, nullptr);
    for (auto const s : _render_semaphores)
        vkDestroySemaphore(_ctx._device, s, nullptr);
    if (_swapchain != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(_ctx._device, _swapchain, nullptr);
    if (_surface != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(_ctx._instance, _surface, nullptr);
}
} // namespace sg::backend::vulkan

namespace sg::backend::vulkan
{
cc::result<vulkan_swapchain_handle> vulkan_context::create_vulkan_swapchain(sg::swapchain_description const& desc)
{
    return vulkan_swapchain::create(*this, desc);
}
} // namespace sg::backend::vulkan
