#include <clean-core/common/utility.hh> // cc::clamp
#include <shaped-graphics/all.hh>
#include <shaped-viewer/view/view_store.hh>
#include <shaped-viewer/viewer.hh>

namespace sv
{
namespace
{
/// Frames a view may go unseen before its identity is dropped.
constexpr i64 view_idle_frames = 240;

/// Frames a view may go unseen before its textures are released, its identity surviving.
/// Much shorter than `view_idle_frames`: a composite target and its accumulators are megabytes, a camera is not.
constexpr i64 view_payload_idle_frames = 60;

/// Ceiling on what every view's textures may hold together before the least recently used are released.
constexpr isize view_payload_budget = isize(256) << 20;

/// Ceiling on how many view identities are kept at all.
constexpr isize view_entry_budget = 256;
} // namespace

view_store::view_store()
{
    // Two tiers, because a view's textures are megabytes and its identity a handful of bytes, so they
    // must not expire on the same schedule.
    _entries.set_limits({.max_idle_frames_payload = view_payload_idle_frames,
                         .max_idle_frames_entry = view_idle_frames,
                         .max_payload_bytes = view_payload_budget,
                         .max_entries = view_entry_budget});
}

void view_store::begin_frame(u64 epoch)
{
    // Only the textures go.
    // What the caller and the controller drive — the camera, the orbit, the zoom, the placement — is the identity,
    // and survives its own payload by design.
    _entries.begin_frame(epoch,
                         [](view_id, impl::view_state& st)
                         {
                             // cc::map yields a proxy by value; its members are the live references
                             for (auto [temporal_id, slot] : st.temporal)
                                 if (slot.texture.raw() != nullptr)
                                     slot.texture.raw()->expire();
                             if (st.composite.raw() != nullptr)
                                 st.composite.raw()->expire();

                             st.temporal.clear();
                             st.composite = {};
                         });
}

impl::view_state& view_store::get_or_create(view_id id)
{
    return _entries.get_or_create(id);
}

impl::view_state* view_store::get_ptr(view_id id)
{
    return _entries.get_ptr(id);
}

impl::view_state& view_store::get(view_id id)
{
    return _entries.get(id);
}

impl::view_state const* view_store::peek_ptr(view_id id) const
{
    return _entries.peek_ptr(id);
}

impl::view_state const& view_store::peek(view_id id) const
{
    return _entries.peek(id);
}

void view_store::set_payload_bytes(view_id id, isize bytes)
{
    _entries.set_payload_bytes(id, bytes);
}

u32 view_store::accumulated_frames(view_id id, u64 temporal_id) const
{
    auto const* const st = _entries.peek_ptr(id); // a query must not keep a view alive
    if (st == nullptr)
        return 0;

    auto const* const slot = st->temporal.get_ptr(temporal_id);
    return slot == nullptr ? 0 : slot->accum_frame;
}

view_store::accumulation_history view_store::history_of(view_id id, u64 temporal_id) const
{
    auto const* const st = _entries.peek_ptr(id);
    auto const* const slot = st == nullptr ? nullptr : st->temporal.get_ptr(temporal_id);
    return slot == nullptr ? accumulation_history{}
                           : accumulation_history{.restarts = slot->restarts, .loading_frames = slot->loading_frames};
}

u32 view_store::min_accumulated_samples(view_id id) const
{
    auto const* const st = _entries.peek_ptr(id);
    return st == nullptr ? 0 : impl::min_accumulated_samples(*st);
}

u32 view_store::pixels_short_of_target(view_id id) const
{
    auto const* const st = _entries.peek_ptr(id);
    return st == nullptr ? 0 : impl::pixels_short_of_target(*st);
}

bool view_store::is_accumulation_converged(view_id id, cc::optional<u32> samples) const
{
    auto const* const st = _entries.peek_ptr(id);
    return st != nullptr && impl::is_accumulation_converged(*st, samples);
}

void view_store::restart_accumulation(view_id id)
{
    auto* const st = _entries.get_ptr(id);
    if (st == nullptr)
        return;

    // cc::map yields a proxy by value; its members are the live references
    for (auto [slot_id, slot] : st->temporal)
        if (temporal_id::is_accumulation(slot_id))
            impl::restart_accumulation(slot);

    // A cut is what throws the rest away on the view's next trace: the temporal histories and the reprojection camera.
    st->camera_cut_pending = true;
}

namespace impl
{
// Defined here rather than in a translation unit of their own: `view_state` is a header-only aggregate, and one fold
// over its temporal map does not earn a second .cc in the build.

void restart_accumulation(temporal_slot& slot)
{
    slot.accum_frame = 0;
    slot.samples = 0;
    slot.pixels_short = 0;
    slot.sample_floors.clear();
}

namespace
{
/// The samples `slot` holds as far as convergence is concerned: none while its mean restarts, whatever was counted before.
[[nodiscard]] u32 samples_of(temporal_slot const& slot)
{
    return slot.accum_frame == 0 ? 0 : slot.samples;
}
} // namespace

u32 min_accumulated_samples(view_state const& state)
{
    auto lowest = cc::optional<u32>();
    for (auto const& [id, slot] : state.temporal)
    {
        if (!temporal_id::is_accumulation(id))
            continue;
        if (!lowest.has_value() || samples_of(slot) < lowest.value())
            lowest = samples_of(slot);
    }
    return lowest.value_or(0);
}

u32 pixels_short_of_target(view_state const& state)
{
    auto total = u32(0);
    for (auto const& [id, slot] : state.temporal)
        if (temporal_id::is_accumulation(id) && slot.accum_frame != 0)
            total += slot.pixels_short;
    return total;
}

bool is_accumulation_converged(view_state const& state, cc::optional<u32> samples)
{
    auto any = false;
    for (auto const& [id, slot] : state.temporal)
    {
        if (!temporal_id::is_accumulation(id))
            continue;
        any = true;

        // Stopped counts as converged: the layer takes no more samples, so waiting longer buys nothing and a target
        // above its own would never be reached.
        auto const stopped = slot.is_at_target();
        auto const reached = samples.has_value() && samples_of(slot) >= samples.value();
        if (!stopped && !reached)
            return false;
    }
    return any;
}

f32 wavefront_budget_of(viewer_config const& config)
{
    return config.headless ? 0.0f : cc::max(0.0f, config.frame_budget_ms);
}

f32 wavefront_budget_share(f32 budget_ms, isize still_layers)
{
    return budget_ms / f32(cc::max(isize(1), still_layers));
}
} // namespace impl
} // namespace sv
