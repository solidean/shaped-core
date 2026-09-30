#include <clean-core/thread/async_coroutine.hh>
#include <shaped-rendering/impl/fsr_backend.hh>

// A build without the FidelityFX sources: nothing is available, and the routine reports `unsupported`.

namespace sr::impl
{
class fsr_stream
{
};

bool fsr_passes_available(sg::context const&)
{
    return false;
}

cc::shared_async<std::shared_ptr<fsr_pass_programs const>> fsr_build_passes(sg::context&)
{
    co_return nullptr;
}

std::shared_ptr<fsr_stream> fsr_create_stream(sg::context&,
                                              std::shared_ptr<fsr_upscale_routine::programs const>,
                                              tg::vec2i,
                                              tg::vec2i)
{
    return nullptr;
}

fsr_upscale_routine::programs const* fsr_stream_programs(fsr_stream const&)
{
    return nullptr;
}

bool fsr_dispatch(fsr_stream&, sg::command_list&, upscale_inputs const&, fsr_options const&, bool)
{
    return false;
}

tg::vec2f fsr_jitter_offset(u32, tg::vec2i, tg::vec2i)
{
    return tg::vec2f(0, 0);
}
} // namespace sr::impl
