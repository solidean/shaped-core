#include <shaped-graphics/all.hh>
#include <shaped-viewer/view/view_store.hh>

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
    // The two tiers are the point: a view's textures are megabytes and its identity is a handful of bytes, so they
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
} // namespace sv
