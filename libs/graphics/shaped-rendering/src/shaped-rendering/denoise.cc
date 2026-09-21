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
#include <shaped-rendering/nrd_denoise_routine.hh>
#include <shaped-rendering/svgf_denoise_routine.hh>

namespace sr
{
namespace
{
[[nodiscard]] bool is_set(sg::texture_2d const& t)
{
    return t.raw() != nullptr;
}

/// The ratio of output to input each vendor preset stands for.
/// The vendors publish the same four, so one table serves both.
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
    denoise_method::dlss_rr, denoise_method::fsr_rr, denoise_method::nrd,
    denoise_method::svgf,    denoise_method::oidn,   denoise_method::atrous,
};
constexpr denoise_method spatial_preference[] = {denoise_method::oidn, denoise_method::atrous};

[[nodiscard]] cc::string_view name_of(denoise_method m)
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

/// One bit per method, set once its refusal has been logged.
/// Process-wide rather than per context: the reason is nearly always the build or the machine, which every context shares.
auto g_refusals_logged = cc::atomic<u32>(0);

/// Logs why `m` did not run, the first time it happens for `m` in this process.
/// A refusal repeats every frame, and one line is what a person needs to find out why the image is noisy.
void log_refusal_once(denoise_method m, cc::string_view why)
{
    auto const bit = u32(1) << u32(m);
    if ((g_refusals_logged.fetch_or(bit, cc::memory_order_relaxed) & bit) != 0)
        return;
    CC_LOG_WARNING("denoiser '{}' did not run: {}", name_of(m), why);
}
} // namespace

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

denoise_history::denoise_history(denoise_history&& other) noexcept
  : _method(other._method),
    _extent(other._extent),
    _reset_requested(other._reset_requested),
    _frame(other._frame),
    _vendor_state(other._vendor_state),
    _release_vendor_state(other._release_vendor_state)
{
    for (auto i = 0; i < 8; ++i)
        _state[i] = cc::move(other._state[i]);

    // Moved FROM rather than shared: two histories releasing one object is the double free this exists to prevent,
    // and the type is move-only precisely so there is one owner.
    other._vendor_state = nullptr;
    other._release_vendor_state = nullptr;
}

denoise_history& denoise_history::operator=(denoise_history&& other) noexcept
{
    if (this == &other)
        return *this;

    // Whatever this held is going away, so it owes its release before it is overwritten.
    _release_vendor();

    _method = other._method;
    _extent = other._extent;
    _reset_requested = other._reset_requested;
    _frame = other._frame;
    for (auto i = 0; i < 8; ++i)
        _state[i] = cc::move(other._state[i]);

    _vendor_state = other._vendor_state;
    _release_vendor_state = other._release_vendor_state;
    other._vendor_state = nullptr;
    other._release_vendor_state = nullptr;
    return *this;
}

denoise_history::~denoise_history()
{
    _release_vendor();
}

void denoise_history::_release_vendor()
{
    if (_vendor_state != nullptr && _release_vendor_state != nullptr)
        _release_vendor_state(_vendor_state);
    _vendor_state = nullptr;
    _release_vendor_state = nullptr;
}

bool denoise_history::_prepare(denoise_method method, tg::vec2i extent)
{
    auto const changed = _method != method || _extent != extent;
    auto const restarted = changed || _reset_requested;
    if (changed)
    {
        // The state is built for one extent and one member, so it goes with them.
        _release_vendor();

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
    case denoise_method::nrd:
        return nrd;
    case denoise_method::none:
    case denoise_method::automatic:
    case denoise_method::count_:
        return false;
    }
    return false;
}

denoise_support query_denoise_support(sg::context const& ctx)
{
    (void)ctx; // every member so far is native compute; the vendor members will read the adapter here

    // The native members are plain compute, which every backend has.
    // A member answers for itself: whether its SDK was compiled in, whether this is a backend it can record on,
    // and whether the adapter and driver carry the feature.
    // The ones still unimplemented stay false, which is what makes `automatic` skip them and a named request report
    // `unsupported` rather than silently running something else.
    return {.atrous = true, .svgf = true, .nrd = nrd_denoise_routine::is_available(ctx)};
}

bool is_temporal(denoise_method m)
{
    return m == denoise_method::svgf || m == denoise_method::dlss_rr || m == denoise_method::fsr_rr
        || m == denoise_method::nrd;
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
    case denoise_method::nrd:
        // The albedo pair is required rather than optional: NRD asks for radiance with no material information in it,
        // and the member divides both out rather than handing it texture to filter as noise.
        return g::albedo | g::specular_albedo | g::normal | g::roughness | g::depth | g::motion | g::hit_distance
             | g::split_diffuse_specular;
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
    case denoise_method::nrd:
        return {};
    case denoise_method::none:
    case denoise_method::automatic:
    case denoise_method::count_:
        return {};
    }
    return {};
}

denoise_method resolve_denoise_method(sg::context const& ctx, denoise_settings const& settings, bool temporal)
{
    if (settings.method != denoise_method::automatic)
        return settings.method;

    auto const support = query_denoise_support(ctx);
    auto const preference = temporal ? cc::span<denoise_method const>(temporal_preference)
                                     : cc::span<denoise_method const>(spatial_preference);
    for (auto const m : preference)
        if (support.supports(m))
            return m;
    return denoise_method::none;
}

tg::vec2i denoise_input_extent(sg::context const& ctx,
                               denoise_settings const& settings,
                               tg::vec2i output_extent,
                               bool temporal)
{
    auto const m = resolve_denoise_method(ctx, settings, temporal);

    // Only the vendor members upscale; every native member and OIDN works at one ratio.
    if (m != denoise_method::dlss_rr && m != denoise_method::fsr_rr)
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
    if (support.nrd)
        nrd_denoise_routine::prewarm(ctx);
    co_return;
}

denoise_outcome denoise_routine::execute(sg::command_list& cmd,
                                         denoise_inputs const& in,
                                         denoise_history& history,
                                         denoise_settings const& settings,
                                         bool temporal)
{
    CC_ASSERT(settings.method != denoise_method::none, "a caller with denoising off does not call the denoiser");

    // Registers the front on first use, so its init prewarms the members; its own readiness gates nothing.
    (void)try_acquire(cmd);

    auto& ctx = cmd.context();
    auto const method = resolve_denoise_method(ctx, settings, temporal);
    if (method == denoise_method::none || !query_denoise_support(ctx).supports(method))
    {
        log_refusal_once(settings.method, "not supported by this build or device");
        return {.status = denoise_status::unsupported, .method = settings.method};
    }

    auto const missing = required_guides(method).without(in.present_guides());
    if (!missing.is_empty())
    {
        log_refusal_once(method, "the call is missing a guide buffer this member requires");
        return {.status = denoise_status::unsupported, .method = method};
    }

    switch (method)
    {
    case denoise_method::atrous:
        return atrous_denoise_routine::execute(cmd, in, history, atrous_denoise_routine::options_for(settings));
    case denoise_method::svgf:
        return svgf_denoise_routine::execute(cmd, in, history, svgf_denoise_routine::options_for(settings));
    case denoise_method::nrd:
        return nrd_denoise_routine::execute(cmd, in, history, nrd_denoise_routine::options_for(settings));
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
