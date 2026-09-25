#include <clean-core/common/assert.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/impl/denoise_images.hh>
#include <shaped-rendering/impl/oidn_device.hh>
#include <shaped-rendering/impl/oidn_network.hh>
#include <shaped-rendering/oidn_denoise_routine.hh>
#include <sr_shaders.hh>

namespace sr
{
using impl::extent_of;
using impl::is_set;

namespace
{
/// The per-stream network, held in the caller's history through its opaque vendor slot.
void release_network(void* p)
{
    delete static_cast<impl::oidn_network*>(p);
}
} // namespace

oidn_options oidn_denoise_routine::options_for(denoise_settings const& settings)
{
    return {.input_scale = settings.exposure};
}

bool oidn_denoise_routine::is_available(sg::context const& ctx)
{
    (void)ctx;
    return impl::oidn_weights_present();
}

cc::shared_async<cc::unit> oidn_denoise_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    // The five pipelines the network dispatches, built here rather than on the first call.
    //
    // Lazily is not an option: a caller advancing an epoch every frame — which is every real frame loop — drives
    // nothing the compile could finish on, so a network built on first use can stay pending forever.
    if (!co_await impl::oidn_prewarm_pipelines(ctx))
        fail_init();
    co_return;
}

denoise_outcome oidn_denoise_routine::execute(sg::command_list& cmd,
                                              denoise_inputs const& in,
                                              denoise_history& history,
                                              oidn_options const& options)
{
    CC_ASSERT(is_set(in.color), "a denoise call needs a colour texture");
    CC_ASSERT(is_set(in.output), "a denoise call needs an output texture");
    CC_ASSERT(extent_of(in.output) == extent_of(in.color), "OIDN does not upscale: output and input extents differ");

    auto const outcome_of = [](denoise_status s, bool restarted = false)
    { return denoise_outcome{.status = s, .method = denoise_method::oidn, .restarted = restarted}; };

    // The network has nine input channels and six of them are the albedo and the normal, so a call without them is
    // `unsupported` rather than a run with zeros — zeros are a surface the network would believe.
    if (!required_guides(denoise_method::oidn).without(in.present_guides()).is_empty())
        return outcome_of(denoise_status::unsupported);

    auto const self = try_acquire(cmd);
    if (self.is_pending())
        return outcome_of(denoise_status::pending);
    if (self.is_failed())
        return outcome_of(denoise_status::failed);

    auto& ctx = cmd.context();
    auto const extent = extent_of(in.color);
    auto const restarted = history._prepare(denoise_method::oidn, extent);

    if (history._vendor_state == nullptr)
    {
        // Owned raw because the history's slot is a `void*` with a release function beside it — the one shape that
        // lets `denoise.hh` free a member's state without naming its type.
        auto* const fresh = new impl::oidn_network();
        auto const tile = options.max_tile > 0 ? options.max_tile : impl::oidn_network::k_default_tile;
        if (!fresh->create(ctx, extent, tile))
        {
            delete fresh;
            return outcome_of(denoise_status::failed);
        }

        history._vendor_state = fresh;
        history._release_vendor_state = &release_network;
    }

    auto& network = *static_cast<impl::oidn_network*>(history._vendor_state);

    // The pipelines are built from shaders `init` already compiled, so this is a tick rather than a wait.
    if (!network.prepare())
        return outcome_of(denoise_status::pending, restarted);

    if (!network.execute(cmd, in.color, in.guides.albedo, in.guides.normal, in.output, options.input_scale))
        return outcome_of(denoise_status::failed, restarted);

    return outcome_of(denoise_status::denoised, restarted);
}
} // namespace sr
