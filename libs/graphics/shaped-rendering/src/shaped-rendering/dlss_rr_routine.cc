#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/dlss_rr_routine.hh>
#include <shaped-rendering/impl/denoise_images.hh>
#include <shaped-rendering/impl/dlss_ngx.hh>

namespace sr
{
using impl::extent_of;
using impl::is_set;

dlss_options dlss_rr_routine::options_for(denoise_settings const& settings)
{
    auto options = dlss_options{};
    switch (settings.quality)
    {
    case denoise_quality::fast:
        options.quality = 0;
        break;
    case denoise_quality::balanced:
        options.quality = 1;
        break;
    case denoise_quality::best:
        options.quality = 2;
        break;
    }
    return options;
}

bool dlss_rr_routine::is_available(sg::context const& ctx)
{
    return impl::dlss_is_available(ctx);
}

cc::shared_async<cc::unit> dlss_rr_routine::init(sg::routine_init_scope scope)
{
    // Nothing to build.
    // The networks belong to NGX's runtime and the feature is per stream, so there is no per-context object for a
    // routine to hold — this exists so the member is a routine like every other one.
    (void)scope;
    co_return;
}

denoise_outcome dlss_rr_routine::execute(sg::command_list& cmd,
                                         denoise_inputs const& in,
                                         denoise_history& history,
                                         dlss_options const& options)
{
    CC_ASSERT(is_set(in.color), "a denoise call needs a colour texture");
    CC_ASSERT(is_set(in.output), "a denoise call needs an output texture");
    CC_ASSERT(in.output.raw() != in.color.raw(), "DLSS needs an output texture other than its input");

    auto const unsupported = denoise_outcome{.status = denoise_status::unsupported, .method = denoise_method::dlss_rr};

    if (!impl::dlss_is_available(cmd.context()))
        return unsupported;

    // The guides are a hard requirement rather than a preference: NGX reads every one of them, and a call without the
    // specular pair is a call this member cannot make.
    // Checked here as well as in the front, because a member is callable directly with its own options.
    if (!required_guides(denoise_method::dlss_rr).without(in.present_guides()).is_empty())
        return unsupported;

    auto const input_extent = extent_of(in.color);
    auto const output_extent = extent_of(in.output);

    // The feature is built for one pair of extents, so `_prepare` dropping it is what a resize means here.
    auto const restarted = history._prepare(denoise_method::dlss_rr, input_extent);

    if (history._vendor_state == nullptr)
    {
        auto* const feature = impl::dlss_create_feature(cmd, {.input_extent = input_extent,
                                                              .output_extent = output_extent,
                                                              .quality = options.quality,
                                                              .hdr = options.hdr});
        if (feature == nullptr)
            return {.status = denoise_status::failed, .method = denoise_method::dlss_rr, .restarted = restarted};

        history._vendor_state = feature;
        history._release_vendor_state = &impl::dlss_release_feature;
    }

    auto const evaluated = impl::dlss_evaluate(cmd, history._vendor_state,
                                               {.color = in.color,
                                                .albedo = in.guides.albedo,
                                                .specular_albedo = in.guides.specular_albedo,
                                                .normal = in.guides.normal,
                                                .roughness = in.guides.roughness,
                                                .depth = in.guides.depth,
                                                .motion = in.guides.motion,
                                                .output = in.output,
                                                .jitter = in.guides.jitter,
                                                .reset = restarted,
                                                .exposure = 1.0f});
    if (!evaluated)
        return {.status = denoise_status::failed, .method = denoise_method::dlss_rr, .restarted = restarted};

    ++history._frame;
    return {.status = denoise_status::denoised, .method = denoise_method::dlss_rr, .restarted = restarted};
}
} // namespace sr
