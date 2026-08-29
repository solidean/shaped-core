#include <clean-core/common/assert.hh>
#include <shaped-graphics/present/swapchain.hh>

namespace sg
{
swapchain::swapchain(swapchain_description const& desc) : _desc(desc)
{
    _desc.assert_valid();
}

swapchain::~swapchain() = default;

bool swapchain_description::is_valid() const
{
    if (is_windowed() && !window.is_valid())
        return false;
    if (!is_windowed() && (headless_extent.value()[0] <= 0 || headless_extent.value()[1] <= 0))
        return false;
    if (buffer_count < 2)
        return false;
    if (!is_render_target_format(format))
        return false;

    return true;
}

void swapchain_description::assert_valid() const
{
    // A handle is required exactly when the chain is windowed, which is what headless_extent decides.
    CC_ASSERT(!is_windowed() || window.is_valid(), "a windowed swapchain requires a window (set headless_extent to "
                                                   "present without one)");
    if (!is_windowed())
        CC_ASSERT(headless_extent.value()[0] > 0 && headless_extent.value()[1] > 0, "headless_extent must be positive "
                                                                                    "in both dimensions");
    CC_ASSERT(buffer_count >= 2, "swapchain buffer_count must be >= 2");
    CC_ASSERT(is_render_target_format(format), "swapchain format must be a color (renderable) format");
}
} // namespace sg
