#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/record/log.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/atomic.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-rendering/atrous_denoise_routine.hh>
#include <shaped-rendering/denoise.hh>
#include <shaped-rendering/impl/denoise_images.hh>
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
constexpr denoise_method temporal_preference[] = {
    denoise_method::dlss_rr, denoise_method::fsr_rr, denoise_method::svgf, denoise_method::oidn, denoise_method::atrous,
};
constexpr denoise_method spatial_preference[] = {denoise_method::oidn, denoise_method::atrous};

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
    case denoise_method::count_:
        break;
    }
    return "?";
}

cc::string_view to_string(denoise_status s)
{
    switch (s)
    {
    case denoise_status::denoised:
        return "denoised";
    case denoise_status::pending:
        return "pending";
    case denoise_status::unsupported:
        return "unsupported";
    case denoise_status::failed:
        return "failed";
    }
    return "?";
}

denoise_guide_set denoise_inputs::present_guides() const
{
    auto set = denoise_guide_set();
    set.set(denoise_guide::albedo, is_set(guides.albedo));
    set.set(denoise_guide::specular_albedo, is_set(guides.specular_albedo));
    set.set(denoise_guide::normal, is_set(guides.normal));
    set.set(denoise_guide::roughness, is_set(guides.roughness));
    set.set(denoise_guide::depth, is_set(guides.depth));
    set.set(denoise_guide::motion, is_set(guides.motion));
    set.set(denoise_guide::hit_distance, is_set(guides.hit_distance));
    set.set(denoise_guide::split_diffuse_specular, is_set(specular));
    return set;
}

bool denoise_history::_prepare(denoise_method method, tg::vec2i extent)
{
    auto const changed = _method != method || _extent != extent;
    auto const restarted = changed || _reset_requested;
    if (changed)
    {
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

bool denoise_support::supports(denoise_method m) const
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
    case denoise_method::none:
    case denoise_method::automatic:
    case denoise_method::count_:
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Adding a member touches exactly the three places below, in this order, and nothing else in this file:
//   1. query_denoise_support  — whether this build and device can run it
//   2. denoise_routine::init  — prewarming it, so its shaders compile before the first call
//   3. denoise_routine::execute's switch — forwarding to it
// They are kept together so a member behind a build option is one contiguous block of `#if` rather than three.
// ---------------------------------------------------------------------------------------------------------------

denoise_support query_denoise_support(sg::context const& ctx)
{
    // The native members are HLSL, and slib builds HLSL only through DXC — so a context whose library has no compiler
    // reaching a format it accepts cannot run them, whatever the backend.
    // Asking the assets rather than assuming is what keeps the promise `denoise_status::unsupported` makes: without
    // it, `automatic` picks a member whose init then fails, and every call reports `failed` instead.
    //
    // A handle is null until its package has been added to a library, which is the same answer as "cannot build".
    auto const buildable
        = [&](slib::shader_asset_handle const& asset) { return asset != nullptr && asset->can_acquire(ctx); };

    // The vendor members will read the adapter here; none of them is implemented yet, and saying so is what makes
    // `automatic` skip them and a named request report `unsupported` rather than silently running something else.
    return {
        .atrous = buildable(sr::shaders::atrous_denoise.compute.main_cs),
        .svgf = buildable(sr::shaders::svgf_temporal.compute.main_cs)
             && buildable(sr::shaders::svgf_variance.compute.main_cs)
             && buildable(sr::shaders::svgf_atrous.compute.main_cs),
    };
}

bool is_temporal(denoise_method m)
{
    return m == denoise_method::svgf || m == denoise_method::dlss_rr || m == denoise_method::fsr_rr;
}

denoise_guide_set required_guides(denoise_method m)
{
    using g = denoise_guide;
    switch (m)
    {
    case denoise_method::svgf:
        return g::normal | g::depth | g::motion;
    case denoise_method::dlss_rr:
        return g::albedo | g::specular_albedo | g::normal | g::roughness | g::depth | g::motion;
    case denoise_method::fsr_rr:
        return g::albedo | g::normal | g::roughness | g::depth | g::motion;
    case denoise_method::atrous:
    case denoise_method::oidn:
    case denoise_method::none:
    case denoise_method::automatic:
    case denoise_method::count_:
        return {};
    }
    return {};
}

denoise_guide_set optional_guides(denoise_method m)
{
    using g = denoise_guide;
    switch (m)
    {
    case denoise_method::atrous:
        return g::albedo | g::normal | g::depth;
    case denoise_method::svgf:
        return g::albedo;
    case denoise_method::oidn:
        return g::albedo | g::normal;
    case denoise_method::dlss_rr:
        return g::hit_distance;
    case denoise_method::fsr_rr:
        return g::specular_albedo | g::hit_distance;
    case denoise_method::none:
    case denoise_method::automatic:
    case denoise_method::count_:
        return {};
    }
    return {};
}

denoise_method resolve_denoise_method(sg::context const& ctx, denoise_settings const& settings)
{
    if (settings.method != denoise_method::automatic)
        return settings.method;

    auto const support = query_denoise_support(ctx);
    auto const preference = settings.fresh_samples ? cc::span<denoise_method const>(temporal_preference)
                                                   : cc::span<denoise_method const>(spatial_preference);
    for (auto const m : preference)
        if (support.supports(m))
            return m;
    return denoise_method::none;
}

tg::vec2i denoise_input_extent(sg::context const& ctx, denoise_settings const& settings, tg::vec2i output_extent)
{
    auto const m = resolve_denoise_method(ctx, settings);

    // Only the vendor members upscale; every native member and OIDN works at one ratio.
    if (m != denoise_method::dlss_rr && m != denoise_method::fsr_rr)
        return output_extent;

    // A named member this context cannot run will be refused, and a caller that traced smaller for it would then
    // composite a smaller image into its own output.
    if (!query_denoise_support(ctx).supports(m))
        return output_extent;

    auto const ratio = vendor_ratio(settings.scale);
    auto const scaled = [&](int v) { return cc::max(1, int(f32(v) / ratio + 0.5f)); };
    return tg::vec2i(scaled(output_extent[0]), scaled(output_extent[1]));
}

cc::shared_async<cc::unit> denoise_routine::init(sg::routine_init_scope scope)
{
    // The front holds nothing of its own.
    // Its init registers every supported member, so prewarming the front starts their compiles on the next tick
    // rather than on the first call.
    // Through prewarm rather than dependency tokens: a token would hold the front pending until every member is ready,
    // and one member this device cannot initialize would then hold every other member hostage.
    auto& ctx = scope.context();
    auto const support = query_denoise_support(ctx);
    if (support.atrous)
        atrous_denoise_routine::prewarm(ctx);
    if (support.svgf)
        svgf_denoise_routine::prewarm(ctx);
    co_return;
}

denoise_outcome denoise_routine::execute(sg::command_list& cmd,
                                         denoise_inputs const& in,
                                         denoise_history& history,
                                         denoise_settings const& settings)
{
    CC_ASSERT(settings.method != denoise_method::none, "a caller with denoising off does not call the denoiser");

    // Registers the front on first use, so its init prewarms the members; its own readiness gates nothing.
    (void)try_acquire(cmd);

    auto& ctx = cmd.context();
    auto const method = resolve_denoise_method(ctx, settings);
    if (method == denoise_method::none || !query_denoise_support(ctx).supports(method))
    {
        // The resolved method rather than what was asked for, so both refusal paths report a member rather than
        // `automatic`, which is not one.
        log_refusal_once(method, refusal_reason::unsupported, "not supported by this build or device");
        return {.status = denoise_status::unsupported, .method = method};
    }

    auto const missing = required_guides(method).without(in.present_guides());
    if (!missing.is_empty())
    {
        log_refusal_once(method, refusal_reason::missing_guide,
                         "the call is missing a guide buffer this member requires");
        return {.status = denoise_status::unsupported, .method = method};
    }

    switch (method)
    {
    case denoise_method::atrous:
        return atrous_denoise_routine::execute(cmd, in, history, atrous_denoise_routine::options_for(settings));
    case denoise_method::svgf:
        return svgf_denoise_routine::execute(cmd, in, history, svgf_denoise_routine::options_for(settings));
    case denoise_method::oidn:
    case denoise_method::dlss_rr:
    case denoise_method::fsr_rr:
    case denoise_method::none:
    case denoise_method::automatic:
    case denoise_method::count_:
        break;
    }
    // Reached only by a member query_denoise_support calls supported and this switch does not forward yet.
    CC_UNREACHABLE("a supported denoise member has no case in denoise_routine::execute");
}
} // namespace sr
