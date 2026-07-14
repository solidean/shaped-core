#include <clean-core/common/assert.hh>
#include <shaped-graphics/swapchain.hh>

namespace sg
{
swapchain::swapchain(swapchain_description const& desc) : _desc(desc)
{
    validate_description(desc);
}

swapchain::~swapchain() = default;

void swapchain::validate_description(swapchain_description const& desc)
{
    CC_ASSERT(desc.native_window_handle != nullptr, "swapchain requires a native window handle");
    CC_ASSERT(desc.buffer_count >= 2, "swapchain buffer_count must be >= 2");
    CC_ASSERT(is_render_target_format(desc.format), "swapchain format must be a color (renderable) format");
}
} // namespace sg
