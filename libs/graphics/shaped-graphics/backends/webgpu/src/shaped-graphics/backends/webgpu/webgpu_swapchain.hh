#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/webgpu/fwd.hh>
#include <shaped-graphics/backends/webgpu/webgpu_common.hh>
#include <shaped-graphics/present/swapchain.hh>

/// WebGPU implementation of sg::swapchain.
///
/// **Headless** is emulated the way dx12 emulates it: `buffer_count` ordinary render-target textures in rotation, the one just presented readable until its epoch retires.
/// **A web canvas** is a WGPUSurface configured on the canvas the selector names; the page composites its current texture when the task that rendered it returns, so present only rotates bookkeeping.
/// Every other window platform is refused at creation.
class sg::backend::webgpu::webgpu_swapchain final : public sg::swapchain
{
public:
    [[nodiscard]] static cc::result<webgpu_swapchain_handle> create(webgpu_context& ctx,
                                                                    sg::swapchain_description const& desc);

    webgpu_swapchain(webgpu_context& ctx, sg::swapchain_description const& desc);

    [[nodiscard]] sg::render_target_view acquire_backbuffer() override;

protected:
    void record_present_transition(sg::command_list& cmd) override;
    void present() override;

private:
    /// Configures the surface at `size`, the only resize a canvas has.
    void configure_surface(tg::vec2i size);

    webgpu_context& _ctx;

    // Headless.
    cc::vector<webgpu_texture_handle> _buffers;
    int _current = 0;

    // Web canvas.
    wgpu_surface _surface;
    tg::vec2i _configured_size = tg::vec2i(0, 0);
    webgpu_texture_handle _surface_texture;
    sg::epoch _last_resize_check = sg::epoch::invalid;
};
