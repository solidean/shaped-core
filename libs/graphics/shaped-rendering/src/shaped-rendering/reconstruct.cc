#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/record/log.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/atomic.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-rendering/atrous_denoise_routine.hh>
#include <shaped-rendering/fsr_upscale_routine.hh>
#include <shaped-rendering/impl/denoise_images.hh>
#include <shaped-rendering/nrd_denoise_routine.hh>
#include <shaped-rendering/oidn_denoise_routine.hh>
#include <shaped-rendering/reconstruct.hh>
#include <shaped-rendering/svgf_denoise_routine.hh>
#include <sr_shaders.hh>

namespace sr
{
using impl::is_set;

namespace
{
/// The ratio of output to input each vendor preset stands for.
///
/// The vendors publish the same four, and the numbers below are provisional until a vendor member reads them:
/// check each against NGX's `NVSDK_NGX_PerfQuality_Value` table and FidelityFX's upscale ratios when dlss_rr and
/// fsr_rr land, since a wrong ratio here is an image traced at the wrong size rather than an error.
[[nodiscard]] f32 vendor_ratio(render_scale_preset p)
{
    switch (p)
    {
    case render_scale_preset::native:
        return 1.0f;
    case render_scale_preset::quality:
        return 1.5f;
    case render_scale_preset::balanced:
        return 1.7f;
    case render_scale_preset::performance:
        return 2.0f;
    }
    return 1.0f;
}

/// The members `automatic` walks, best first.
///
/// `oidn` is in neither: at roughly 0.2 s per megapixel it is a reference-quality member, not one a frame loop can
/// afford, so a caller has to name it.
constexpr denoise_method temporal_preference[] = {
    denoise_method::dlss_rr, denoise_method::fsr_rr, denoise_method::nrd, denoise_method::svgf, denoise_method::atrous,
};
constexpr denoise_method spatial_preference[] = {denoise_method::atrous};

/// Which refusal a bit stands for, so one reason being logged does not silence the other.
enum class refusal_reason : u32
{
    unsupported,
    missing_guide,

    count_
};

/// One bit per (method, reason), set once that pair has been logged.
/// Per reason and not just per method: a member refused once for a missing guide would otherwise never report that
/// this build cannot run it either, and the two send a reader to different places.
/// Process-wide rather than per context: the reason is nearly always the build or the machine, which every context shares.
static_assert(u32(denoise_method::count_) * u32(refusal_reason::count_) <= 32, "the refusal bitset no longer fits");
auto g_refusals_logged = cc::atomic<u32>(0);

/// Logs why `m` did not run, the first time it happens for that method and reason in this process.
/// A refusal repeats every frame, and one line is what a person needs to find out why the image is noisy.
void log_refusal_once(denoise_method m, refusal_reason reason, cc::string_view why)
{
    auto const bit = u32(1) << (u32(m) * u32(refusal_reason::count_) + u32(reason));
    if ((g_refusals_logged.fetch_or(bit, cc::memory_order_relaxed) & bit) != 0)
        return;
    CC_LOG_WARNING("denoiser '{}' did not run: {}", to_string(m), why);
}
} // namespace

cc::string_view to_string(denoise_method m)
{
    switch (m)
    {
    case denoise_method::none:
        return "none";
    case denoise_method::automatic:
        return "automatic";
    case denoise_method::atrous:
        return "atrous";
    case denoise_method::svgf:
        return "svgf";
    case denoise_method::oidn:
        return "oidn";
    case denoise_method::dlss_rr:
        return "dlss_rr";
    case denoise_method::fsr_rr:
        return "fsr_rr";
    case denoise_method::nrd:
        return "nrd";
    case denoise_method::count_:
        break;
    }
    return "?";
}

cc::string_view to_string(upscale_method m)
{
    switch (m)
    {
    case upscale_method::none:
        return "none";
    case upscale_method::automatic:
        return "automatic";
    case upscale_method::fsr:
        return "fsr";
    case upscale_method::count_:
        break;
    }
    return "?";
}

cc::string_view to_string(reconstruct_status s)
{
    switch (s)
    {
    case reconstruct_status::denoised:
        return "denoised";
    case reconstruct_status::pending:
        return "pending";
    case reconstruct_status::unsupported:
        return "unsupported";
    case reconstruct_status::failed:
        return "failed";
    }
    return "?";
}

reconstruct_guide_set reconstruct_inputs::present_guides() const
{
    auto set = reconstruct_guide_set();
    set.set(reconstruct_guide::albedo, is_set(guides.albedo));
    set.set(reconstruct_guide::specular_albedo, is_set(guides.specular_albedo));
    set.set(reconstruct_guide::normal, is_set(guides.normal));
    set.set(reconstruct_guide::roughness, is_set(guides.roughness));
    set.set(reconstruct_guide::depth, is_set(guides.depth));
    set.set(reconstruct_guide::motion, is_set(guides.motion));
    set.set(reconstruct_guide::hit_distance, is_set(guides.hit_distance));
    set.set(reconstruct_guide::split_diffuse_specular, is_set(specular));
    return set;
}

bool reconstruct_history::_prepare(denoise_method method, tg::vec2i extent)
{
    auto const changed = _method != method || _extent != extent;
    auto const restarted = changed || _reset_requested;
    if (changed)
    {
        // The member's object is built for one extent and one member, so it goes with them.
        _member_state = nullptr;

        // Built for another member or size, so nothing in it can be reused.
        for (auto& t : _state)
            t = {};
        _method = method;
        _extent = extent;
        _frame = 0;
    }
    _reset_requested = false;
    return restarted;
}

bool upscale_history::_prepare(tg::vec2i input_extent, tg::vec2i output_extent)
{
    auto const changed = _input_extent != input_extent || _output_extent != output_extent;
    auto const restarted = changed || _reset_requested;
    if (changed)
    {
        // The upscaler's state is built for one pair of extents, so it goes with them.
        _state = nullptr;
        _input_extent = input_extent;
        _output_extent = output_extent;
    }
    _reset_requested = false;
    return restarted;
}

bool reconstruct_support::supports(upscale_method m) const
{
    switch (m)
    {
    case upscale_method::fsr:
        return fsr;
    case upscale_method::none:
    case upscale_method::automatic:
    case upscale_method::count_:
        return false;
    }
    return false;
}

bool reconstruct_support::supports(denoise_method m) const
{
    switch (m)
    {
    case denoise_method::atrous:
        return atrous;
    case denoise_method::svgf:
        return svgf;
    case denoise_method::oidn:
        return oidn;
    case denoise_method::dlss_rr:
        return dlss_rr;
    case denoise_method::fsr_rr:
        return fsr_rr;
    case denoise_method::nrd:
        return nrd;
    case denoise_method::none:
    case denoise_method::automatic:
    case denoise_method::count_:
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Adding a member touches exactly the three places below, in this order, and nothing else in this file:
//   1. query_reconstruct_support  — whether this build and device can run it
//   2. reconstruct_routine::init  — prewarming it, so its shaders compile before the first call
//   3. reconstruct_routine::execute's switch — forwarding to it
// They are kept together so a member behind a build option is one contiguous block of `#if` rather than three.
// ---------------------------------------------------------------------------------------------------------------

reconstruct_support query_reconstruct_support(sg::context const& ctx)
{
    // The native members are HLSL, and slib builds HLSL only through DXC — so a context whose library has no compiler
    // reaching a format it accepts cannot run them, whatever the backend.
    // Asking the assets rather than assuming is what keeps the promise `reconstruct_status::unsupported` makes: without
    // it, `automatic` picks a member whose init then fails, and every call reports `failed` instead.
    //
    // A handle is null until its package has been added to a library, which is the same answer as "cannot build".
    auto const buildable
        = [&](slib::shader_asset_handle const& asset) { return asset != nullptr && asset->can_acquire(ctx); };

    // A member answers for itself: whether its shaders build, whether it is compiled in, and whether this device
    // can run it.
    // The ones still unimplemented stay false, which is what makes `automatic` skip them and a named request report
    // `unsupported` rather than silently running something else.
    return {
        .atrous = buildable(sr::shaders::atrous_denoise.compute.main_cs),
        .svgf = buildable(sr::shaders::svgf_temporal.compute.main_cs)
             && buildable(sr::shaders::svgf_variance.compute.main_cs)
             && buildable(sr::shaders::svgf_atrous.compute.main_cs),
        .oidn = oidn_denoise_routine::is_available(ctx),
        .nrd = nrd_denoise_routine::is_available(ctx),
        .fsr = fsr_upscale_routine::is_available(ctx),
    };
}

bool is_temporal(denoise_method m)
{
    return m == denoise_method::svgf || m == denoise_method::dlss_rr || m == denoise_method::fsr_rr
        || m == denoise_method::nrd;
}

reconstruct_guide_set required_guides(denoise_method m)
{
    using g = reconstruct_guide;
    switch (m)
    {
    case denoise_method::svgf:
        return g::normal | g::depth | g::motion;
    case denoise_method::dlss_rr:
        return g::albedo | g::specular_albedo | g::normal | g::roughness | g::depth | g::motion;
    case denoise_method::fsr_rr:
        return g::albedo | g::normal | g::roughness | g::depth | g::motion;
    case denoise_method::nrd:
        // The albedo pair is required rather than optional: NRD asks for radiance with no material information in it,
        // and the member divides both out rather than handing it texture to filter as noise.
        return g::albedo | g::specular_albedo | g::normal | g::roughness | g::depth | g::motion | g::hit_distance
             | g::split_diffuse_specular;
    case denoise_method::oidn:
        // Six of the network's nine input channels are these two, so a call without them is not a degraded run.
        return g::albedo | g::normal;
    case denoise_method::atrous:
    case denoise_method::none:
    case denoise_method::automatic:
    case denoise_method::count_:
        return {};
    }
    return {};
}

reconstruct_guide_set optional_guides(denoise_method m)
{
    using g = reconstruct_guide;
    switch (m)
    {
    case denoise_method::atrous:
        return g::albedo | g::specular_albedo | g::normal | g::depth;
    case denoise_method::svgf:
        return g::albedo | g::specular_albedo;
    case denoise_method::oidn:
        return {};
    case denoise_method::dlss_rr:
        return g::hit_distance;
    case denoise_method::fsr_rr:
        return g::specular_albedo | g::hit_distance;
    case denoise_method::nrd:
        return {};
    case denoise_method::none:
    case denoise_method::automatic:
    case denoise_method::count_:
        return {};
    }
    return {};
}

namespace
{
/// `resolve_denoise_method` against a support answer the caller already has.
[[nodiscard]] denoise_method resolve_with(reconstruct_support const& support, reconstruct_settings const& settings)
{
    if (settings.denoiser != denoise_method::automatic)
        return settings.denoiser;

    auto const preference = settings.fresh_samples ? cc::span<denoise_method const>(temporal_preference)
                                                   : cc::span<denoise_method const>(spatial_preference);
    for (auto const m : preference)
        if (support.supports(m))
            return m;
    return denoise_method::none;
}

/// Whether a denoiser upscales by itself, so that no upscaler runs behind it.
[[nodiscard]] bool upscales_itself(denoise_method m)
{
    return m == denoise_method::dlss_rr || m == denoise_method::fsr_rr;
}

/// `resolve_upscale_method` against a support answer and a resolved denoiser the caller already has.
[[nodiscard]] upscale_method resolve_upscaler_with(reconstruct_support const& support,
                                                   reconstruct_settings const& settings,
                                                   denoise_method denoiser)
{
    if (upscales_itself(denoiser))
        return upscale_method::none;
    if (settings.upscaler != upscale_method::automatic)
        return settings.upscaler;

    // Automatic upscales only when there is something to upscale.
    if (settings.scale == render_scale_preset::native)
        return upscale_method::none;
    return support.fsr ? upscale_method::fsr : upscale_method::none;
}

/// The ratio the upscaler runs `preset` at.
[[nodiscard]] f32 upscaler_ratio(upscale_method m, render_scale_preset preset)
{
    switch (m)
    {
    case upscale_method::fsr:
        return fsr_upscale_routine::ratio_of(preset);
    case upscale_method::none:
    case upscale_method::automatic:
    case upscale_method::count_:
        break;
    }
    return 1.0f;
}

/// The upscaler a call with `settings` actually runs, which is what planning a frame must follow.
/// `none` when the denoiser in front of it will be refused, since `execute` then refuses the whole call, and when the
/// upscaler itself cannot run here.
[[nodiscard]] upscale_method planned_upscaler(reconstruct_support const& support, reconstruct_settings const& settings)
{
    auto const denoiser = resolve_with(support, settings);
    if (settings.denoiser != denoise_method::none && (denoiser == denoise_method::none || !support.supports(denoiser)))
        return upscale_method::none;
    auto const u = resolve_upscaler_with(support, settings, denoiser);
    return u != upscale_method::none && support.supports(u) ? u : upscale_method::none;
}

[[nodiscard]] tg::vec2i scaled_down(tg::vec2i output_extent, f32 ratio)
{
    auto const scaled = [&](int v) { return cc::max(1, int(f32(v) / ratio + 0.5f)); };
    return tg::vec2i(scaled(output_extent[0]), scaled(output_extent[1]));
}

/// The same once-per-process refusal log as the denoisers', for the upscalers.
static_assert(u32(upscale_method::count_) * u32(refusal_reason::count_) <= 32, "the refusal bitset no longer fits");
auto g_upscaler_refusals_logged = cc::atomic<u32>(0);

void log_upscaler_refusal_once(upscale_method m, refusal_reason reason, cc::string_view why)
{
    auto const bit = u32(1) << (u32(m) * u32(refusal_reason::count_) + u32(reason));
    if ((g_upscaler_refusals_logged.fetch_or(bit, cc::memory_order_relaxed) & bit) != 0)
        return;
    CC_LOG_WARNING("upscaler '{}' did not run: {}", to_string(m), why);
}

/// The denoiser forwarded to, writing `in.output`.
[[nodiscard]] reconstruct_outcome run_denoiser(sg::command_list& cmd,
                                               reconstruct_inputs const& in,
                                               reconstruct_history& history,
                                               reconstruct_settings const& settings,
                                               denoise_method method)
{
    switch (method)
    {
    case denoise_method::atrous:
        return atrous_denoise_routine::execute(cmd, in, history, atrous_denoise_routine::options_for(settings));
    case denoise_method::svgf:
        return svgf_denoise_routine::execute(cmd, in, history, svgf_denoise_routine::options_for(settings));
    case denoise_method::nrd:
        return nrd_denoise_routine::execute(cmd, in, history, nrd_denoise_routine::options_for(settings));
    case denoise_method::oidn:
        return oidn_denoise_routine::execute(cmd, in, history, oidn_denoise_routine::options_for(settings));
    case denoise_method::dlss_rr:
    case denoise_method::fsr_rr:
    case denoise_method::none:
    case denoise_method::automatic:
    case denoise_method::count_:
        break;
    }
    // Reached only by a member query_reconstruct_support calls supported and this switch does not forward yet.
    CC_UNREACHABLE("a supported denoise member has no case in reconstruct_routine::execute");
}
} // namespace

denoise_method resolve_denoise_method(sg::context const& ctx, reconstruct_settings const& settings)
{
    if (settings.denoiser != denoise_method::automatic)
        return settings.denoiser;
    return resolve_with(query_reconstruct_support(ctx), settings);
}

upscale_method resolve_upscale_method(sg::context const& ctx, reconstruct_settings const& settings)
{
    auto const support = query_reconstruct_support(ctx);
    return resolve_upscaler_with(support, settings, resolve_with(support, settings));
}

tg::vec2i reconstruct_input_extent(sg::context const& ctx, reconstruct_settings const& settings, tg::vec2i output_extent)
{
    auto const support = query_reconstruct_support(ctx);
    auto const m = resolve_with(support, settings);

    // A member that upscales by itself maps the preset onto its own ratios.
    // A named member this context cannot run will be refused, and a caller that traced smaller for it would then
    // composite a smaller image into its own output.
    if (upscales_itself(m))
        return support.supports(m) ? scaled_down(output_extent, vendor_ratio(settings.scale)) : output_extent;

    // Any other denoiser runs at one ratio, and the upscaler behind it maps the preset, under the same refusal rule.
    auto const u = planned_upscaler(support, settings);
    if (u == upscale_method::none)
        return output_extent;
    return scaled_down(output_extent, upscaler_ratio(u, settings.scale));
}

tg::vec2f reconstruct_jitter(sg::context const& ctx,
                             reconstruct_settings const& settings,
                             tg::vec2i output_extent,
                             u32 frame_index)
{
    if (planned_upscaler(query_reconstruct_support(ctx), settings) != upscale_method::fsr)
        return tg::vec2f(0, 0);
    return fsr_upscale_routine::jitter(frame_index, reconstruct_input_extent(ctx, settings, output_extent),
                                       output_extent);
}

cc::shared_async<cc::unit> reconstruct_routine::init(sg::routine_init_scope scope)
{
    // The front holds nothing of its own.
    // Its init registers every supported member, so prewarming the front starts their compiles on the next tick
    // rather than on the first call.
    // Through prewarm rather than dependency tokens: a token would hold the front pending until every member is ready,
    // and one member this device cannot initialize would then hold every other member hostage.
    auto& ctx = scope.context();
    auto const support = query_reconstruct_support(ctx);
    if (support.atrous)
        atrous_denoise_routine::prewarm(ctx);
    if (support.svgf)
        svgf_denoise_routine::prewarm(ctx);
    if (support.nrd)
        nrd_denoise_routine::prewarm(ctx);
    if (support.oidn)
        oidn_denoise_routine::prewarm(ctx);
    if (support.fsr)
        fsr_upscale_routine::prewarm(ctx);
    co_return;
}

reconstruct_outcome reconstruct_routine::execute(sg::command_list& cmd,
                                                 reconstruct_inputs const& in,
                                                 reconstruct_history& history,
                                                 reconstruct_settings const& settings)
{
    // Registers the front on first use, so its init prewarms the members; its own readiness gates nothing.
    (void)try_acquire(cmd);

    auto& ctx = cmd.context();
    // Asked once and used throughout, since every resolver and support check wants the same answer.
    auto const support = query_reconstruct_support(ctx);
    auto const method = resolve_with(support, settings);
    auto const upscaler = resolve_upscaler_with(support, settings, method);
    CC_ASSERT(settings.denoiser != denoise_method::none || settings.upscaler != upscale_method::none,
              "a caller with denoising and upscaling both off does not call the front");

    auto const denoising = settings.denoiser != denoise_method::none;
    if (!denoising && upscaler == upscale_method::none)
    {
        // What could not run is the request itself, which is why the log names `automatic`.
        log_upscaler_refusal_once(upscale_method::automatic, refusal_reason::unsupported,
                                  "resolves to no upscaler on this context, and the denoiser is none");
        return {.status = reconstruct_status::unsupported,
                .denoiser = denoise_method::none,
                .upscaler = upscale_method::none};
    }
    if (denoising)
    {
        if (method == denoise_method::none || !support.supports(method))
        {
            // The resolved method rather than what was asked for, so both refusal paths report a member rather than
            // `automatic`, which is not one.
            log_refusal_once(method, refusal_reason::unsupported, "not supported by this build or device");
            return {.status = reconstruct_status::unsupported, .denoiser = method, .upscaler = upscaler};
        }
        if (!required_guides(method).without(in.present_guides()).is_empty())
        {
            log_refusal_once(method, refusal_reason::missing_guide,
                             "the call is missing a guide buffer this member requires");
            return {.status = reconstruct_status::unsupported, .denoiser = method, .upscaler = upscaler};
        }
    }

    if (upscaler == upscale_method::none)
        return run_denoiser(cmd, in, history, settings, method);

    if (!support.supports(upscaler))
    {
        log_upscaler_refusal_once(upscaler, refusal_reason::unsupported, "not supported by this build or device");
        return {.status = reconstruct_status::unsupported, .denoiser = method, .upscaler = upscaler};
    }
    if (!is_set(in.guides.depth) || !is_set(in.guides.motion))
    {
        log_upscaler_refusal_once(upscaler, refusal_reason::missing_guide,
                                  "an upscaler needs the depth and motion guides");
        return {.status = reconstruct_status::unsupported, .denoiser = method, .upscaler = upscaler};
    }

    // The denoiser writes a scratch image at the input extent, and the upscaler reads that.
    auto source = in.color;
    auto denoised = reconstruct_outcome{.status = reconstruct_status::denoised};
    if (denoising)
    {
        impl::ensure_image(ctx, history._upscale_source, impl::extent_of(in.color), sg::pixel_format::rgba32_float);
        auto scratch_inputs = in;
        scratch_inputs.output = history._upscale_source;
        denoised = run_denoiser(cmd, scratch_inputs, history, settings, method);
        if (!denoised.is_denoised())
        {
            denoised.upscaler = upscaler;
            return denoised;
        }
        source = history._upscale_source;
    }

    // A different denoiser than last time is a different image to accumulate, so the upscaler starts over with it.
    auto const source_denoiser = denoising ? method : denoise_method::none;
    if (history._upscale._source_denoiser != source_denoiser)
    {
        history._upscale.reset();
        history._upscale._source_denoiser = source_denoiser;
    }

    auto const upscaled = fsr_upscale_routine::execute(cmd,
                                                       {.color = source,
                                                        .depth = in.guides.depth,
                                                        .motion = in.guides.motion,
                                                        .jitter = in.guides.jitter,
                                                        .view_to_clip = in.guides.view_to_clip,
                                                        .exposure = settings.exposure,
                                                        .output = in.output},
                                                       history._upscale, fsr_upscale_routine::options_for(settings));
    return {.status = upscaled.status,
            .denoiser = source_denoiser,
            .upscaler = upscaler,
            .restarted = denoised.restarted || upscaled.restarted};
}
} // namespace sr
