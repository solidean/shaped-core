#include <clean-core/common/assert.hh>
#include <shaped-rendering/impl/metalfx_backend.hh>

// A build without the Metal backend: MetalFX is never available, so nothing below is ever reached.

class sr::impl::metalfx_stream
{
};

class sr::impl::metalfx_denoised_stream
{
};

namespace sr::impl
{
bool metalfx_available(sg::context const&)
{
    return false;
}

f32 metalfx_max_scale(sg::context const&)
{
    return 0.0f;
}

std::shared_ptr<metalfx_stream> metalfx_create_stream(sg::context&, metalfx_stream_desc const&)
{
    CC_UNREACHABLE("MetalFX is not available in this build");
}

bool metalfx_dispatch(metalfx_stream&, sg::command_list&, metalfx_dispatch_desc const&)
{
    CC_UNREACHABLE("MetalFX is not available in this build");
}

bool metalfx_denoised_available(sg::context const&)
{
    return false;
}

f32 metalfx_denoised_max_scale(sg::context const&)
{
    return 0.0f;
}

std::shared_ptr<metalfx_denoised_stream> metalfx_create_denoised_stream(sg::context&, metalfx_denoised_stream_desc const&)
{
    CC_UNREACHABLE("MetalFX is not available in this build");
}

bool metalfx_denoised_dispatch(metalfx_denoised_stream&, sg::command_list&, metalfx_denoised_dispatch_desc const&)
{
    CC_UNREACHABLE("MetalFX is not available in this build");
}
} // namespace sr::impl
