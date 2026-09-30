#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/dlss_rr_routine.hh>
#include <shaped-rendering/impl/denoise_images.hh>
#include <shaped-rendering/impl/dlss_ngx.hh>

#include <memory> // std::shared_ptr, which is what denoise_history::_member_state is

namespace sr
{
using impl::extent_of;
using impl::is_set;

dlss_options dlss_rr_routine::options_for(denoise_settings const& settings)
{
    return {.quality = settings.quality, .exposure = settings.exposure};
}

bool dlss_rr_routine::is_available(sg::context const& ctx)
{
    return impl::dlss_is_available(ctx);
}

dlss_rr_routine::~dlss_rr_routine()
{
    if (_instance == nullptr)
        return;
    _ctx->defer_until_retired([instance = cc::move(_instance)] { impl::dlss_close(instance); });
}

cc::shared_async<cc::unit> dlss_rr_routine::init(sg::routine_init_scope scope)
{
    _ctx = &scope.context();
    _instance = impl::dlss_open(*_ctx);
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

    // The routine owns the NGX instance every stream is created under, so it is acquired before any stream is.
    //
    // It declines until a tick has brought it up, exactly as every other member does, even though its init compiles
    // nothing: `try_acquire` reports readiness and never establishes it.
    // `denoise_routine::init` prewarms this member, so a caller going through the front pays that tick once.
    auto const self = try_acquire(cmd);
    if (self.is_pending())
        return {.status = denoise_status::pending, .method = denoise_method::dlss_rr};
    if (self.is_failed() || self->_instance == nullptr)
        return {.status = denoise_status::failed, .method = denoise_method::dlss_rr};

    auto const input_extent = extent_of(in.color);
    auto const output_extent = extent_of(in.output);

    // Both extents, because the feature is built for the pair: NGX fixes `InTargetWidth` at creation, and an output
    // that moved while the traced size rounded to the same value would otherwise keep a feature built for the old one.
    auto restarted = history._prepare(denoise_method::dlss_rr, input_extent, output_extent);

    // `quality` and `hdr` are fixed at creation like the extents, so a change to either is a new stream too.
    auto const* const existing = static_cast<impl::dlss_stream const*>(history._member_state.get());
    if (existing != nullptr && (existing->quality != options.quality || existing->hdr != options.hdr))
    {
        history._member_state = nullptr;
        history._frame = 0;
        restarted = true;
    }

    if (history._member_state == nullptr)
    {
        auto* const stream = impl::dlss_create_stream(
            cmd, self->_instance,
            {.input_extent = input_extent, .output_extent = output_extent, .quality = options.quality, .hdr = options.hdr});
        if (stream == nullptr)
            return {.status = denoise_status::failed, .method = denoise_method::dlss_rr, .restarted = restarted};

        // Dropped while the caller is still recording — `_prepare` and the rebuild above both do it — so the release
        // waits for the epoch current at the drop, and every frame that evaluated the stream has finished by then.
        auto* const ctx = &cmd.context();
        history._member_state = std::shared_ptr<void>(
            stream, [ctx](void* s)
            { ctx->defer_until_retired([s] { impl::dlss_release_stream(static_cast<impl::dlss_stream*>(s)); }); });
    }

    auto const evaluated = impl::dlss_evaluate(cmd, *static_cast<impl::dlss_stream*>(history._member_state.get()),
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
                                                .exposure = options.exposure});
    if (!evaluated)
        return {.status = denoise_status::failed, .method = denoise_method::dlss_rr, .restarted = restarted};

    ++history._frame;
    return {.status = denoise_status::denoised, .method = denoise_method::dlss_rr, .restarted = restarted};
}
} // namespace sr
