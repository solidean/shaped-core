#include <clean-core/common/assert.hh>
#include <shaped-graphics/swapchain.hh>

namespace sg
{
swapchain::swapchain(swapchain_description const& desc) : _desc(desc)
{
    _desc.assert_valid();
}

swapchain::~swapchain() = default;

bool swapchain_description::is_valid() const
{
    if (native_window_handle == nullptr)
        return false;
    if (buffer_count < 2)
        return false;
    if (!is_render_target_format(format))
        return false;

    return true;
}

void swapchain_description::assert_valid() const
{
    CC_ASSERT(native_window_handle != nullptr, "swapchain requires a native window handle");
    CC_ASSERT(buffer_count >= 2, "swapchain buffer_count must be >= 2");
    CC_ASSERT(is_render_target_format(format), "swapchain format must be a color (renderable) format");
}
} // namespace sg
