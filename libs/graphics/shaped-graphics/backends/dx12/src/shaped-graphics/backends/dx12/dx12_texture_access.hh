#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/container/small_vector.hh>
#include <clean-core/string/print.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/barrier/command_list_slot.hh>
#include <shaped-graphics/barrier/resource_access.hh>
#include <shaped-graphics/barrier/resource_access_state.hh>
#include <shaped-graphics/barrier/subresource_state.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/resource/subresource.hh>

namespace sg::backend::dx12
{
struct combined_layout;
struct dx12_subresource_barrier;
class dx12_texture_access;
} // namespace sg::backend::dx12

/// A barrier the tracker asks the command list to emit, scoped to a subresource range.
/// dx12-internal: SG core never produces barriers, and each backend owns its own tracking and emission.
struct sg::backend::dx12::dx12_subresource_barrier
{
    sg::subresource_range range;
    sg::access_barrier barrier;
};

namespace sg::backend::dx12
{

/// The subresource grid a texture's access state partitions: mip × array-slice × aspect-plane.
/// A cube is 6 array slices per cube; a depth+stencil format has two aspect planes.
[[nodiscard]] inline sg::subresource_extent subresource_extent_of(sg::texture_description const& d)
{
    int const layers = d.array_layers.value_or(1) * (d.is_cube ? 6 : 1);
    return sg::subresource_extent{
        .mip_count = d.mip_levels,
        .array_count = layers,
        .aspect_count = sg::format_aspect_count(d.format),
    };
}

} // namespace sg::backend::dx12

/// How two required layouts for one subresource-in-one-op combined: cleanly, into a slower fallback, or not at all — a hazard.
enum class sg::backend::dx12::layout_combine
{
    ok,       ///< a single layout serves both (or they were equal) — no cost
    degraded, ///< no specialized layout serves both, so COMMON (`general`) is used — correct but slower
    conflict, ///< the two accesses can't coexist in one op (e.g. copy-dest + sampled) — caller error
};

struct sg::backend::dx12::combined_layout
{
    sg::texture_layout layout;
    layout_combine result;
};

namespace sg::backend::dx12
{

/// Combine the two layouts a subresource is required to be in within a single operation, i.e. a texture bound as more than one view.
/// The policy is D3D12-specific.
/// The only mismatch a compute binding group can legitimately produce is a texture bound as both a sampled (`shader_readonly`/SRV) and a storage (`shader_readwrite`/UAV) view.
/// No specialized layout serves both an SRV and a UAV, so it falls back to COMMON (`general`) and reports `degraded`; sampling in COMMON is slower.
/// `general` already serves any access, so combining with it is free.
/// Anything else — a copy/render-target/depth layout mixed with a different one — is a real hazard and reports `conflict`.
[[nodiscard]] inline combined_layout combine_layouts(sg::texture_layout a, sg::texture_layout b)
{
    if (a == b)
        return {a, layout_combine::ok};

    auto const is_shader = [](sg::texture_layout l)
    { return l == sg::texture_layout::shader_readonly || l == sg::texture_layout::shader_readwrite; };
    if (is_shader(a) && is_shader(b))
        return {sg::texture_layout::general, layout_combine::degraded};

    if (a == sg::texture_layout::general || b == sg::texture_layout::general)
        return {sg::texture_layout::general, layout_combine::ok};

    return {sg::texture_layout::general, layout_combine::conflict};
}

} // namespace sg::backend::dx12

/// Per-texture, per-command-list access tracking — the dx12 realization of the covering-partition + slot model.
/// Pure logic, with no D3D12 objects, so it is unit-testable without a device.
/// dx12_texture holds one under a mutex; the command list drives declare/finalize/discard and emits the returned barriers.
///
/// Each open command list keys its private covering partition by its command_list_slot, and that partition starts **empty**:
/// a box enters at the layout its first declare asks for, not at whatever the texture happened to be in while the list recorded.
/// What the list needs on entry is recorded as a requirement per box, and the submit resolves it against `_current`, the between-lists state,
/// prepending the barrier that gets there — into a small command list executed ahead of this one in the same ExecuteCommandLists call.
///
/// That is what makes a recorded barrier independent of when the list was recorded.
/// The model it replaces seeded a slot from the between-lists state and had every non-last finalize *revert* the texture back to it,
/// which held only as long as command lists were the sole things moving a layout — an async transfer's fixup moving it between one list's
/// submit and another's left the second list's barriers naming a layout the image had left.
///
/// Every finalize writes `_current`, in submission order.
class sg::backend::dx12::dx12_texture_access
{
public:
    explicit dx12_texture_access(sg::subresource_extent extent) : _current(extent) {}

    /// The layout `range`'s first subresource is in as of the last submitted list.
    /// For the async transfer path, which records on the copy queue and so cannot go through declare/flush at all.
    [[nodiscard]] sg::texture_layout current_layout_of(sg::subresource_range range)
    {
        sg::texture_layout layout = sg::texture_layout::undefined;
        bool first = true;
        _current.for_each_in(range,
                             [&](sg::resource_access_state const& state)
                             {
                                 if (first)
                                 {
                                     layout = state.curr_layout;
                                     first = false;
                                 }
                             });
        return layout;
    }

    /// Accumulate one declared `stages`/`access`/`layout` over `range` for `slot` into the next-op state, entering an untouched box at `layout`, without emitting anything.
    /// Call once per binding — a texture bound several times to one op declares several times, and `flush` then merges them per box.
    /// Thread-safe via the owning dx12_texture's mutex.
    ///
    /// If a box is already declared for this op with a *different* layout — the texture bound as more than one view — the two are combined via `combine_layouts`.
    /// They may fall back to COMMON (`general`) with a one-time perf warning, and a genuine conflict such as copy-dest plus sampled asserts.
    void declare(sg::command_list_slot slot,
                 sg::subresource_range range,
                 sg::pipeline_stage_flags stages,
                 sg::access_flags access,
                 sg::texture_layout layout)
    {
        auto& s = slot_for(slot);
        bool warned = false;
        s.partition.for_each_box_in(
            range,
            [&](sg::subresource_range const&, sg::resource_access_state& state)
            {
                if (!state.entry_begun)
                    state.begin_entry(layout);

                sg::texture_layout target = layout;
                // Already declared for this op with a different layout? One layout must serve both accesses.
                bool const touched = state.has_pending_declares() || state.has_pending_layout_change();
                if (touched && state.curr_layout != layout)
                {
                    combined_layout const c = combine_layouts(state.curr_layout, layout);
                    CC_ASSERT(c.result != layout_combine::conflict, "a texture subresource is bound with conflicting "
                                                                    "layouts in one operation");
                    if (c.result == layout_combine::degraded && !warned)
                    {
                        CC_LOG_WARNING("a texture is bound as both a sampled and a storage view in one "
                                       "operation — using the COMMON layout, which is slower to sample. Prefer "
                                       "a single view class.");
                        warned = true;
                    }
                    target = c.layout;
                }
                state.declare(stages, access, target);
            });
    }

    /// Test-and-set `slot`'s pending-barrier flag: true the first time it is called for `slot` since the last flush, false after.
    /// Only valid on a slot that was declared (active), which is why it is only ever called right after declare.
    /// `flush` clears it.
    [[nodiscard]] bool mark_pending_barrier(sg::command_list_slot slot)
    {
        // Only ever called right after declare, so the slot exists and is active.
        int const i = int(slot);
        CC_ASSERT(i < _slots.size() && _slots[i].active, "mark_pending_barrier before declare");
        auto& s = _slots[i];
        if (s.pending_barrier)
            return false;
        s.pending_barrier = true;
        return true;
    }

    /// Flush the accesses declared for `slot` since the last flush: for every subresource box with pending work, roll it forward and collect the per-box barrier; empty means all freebies.
    /// Merges multiple declares of the same box — a texture bound several times to one op — into one barrier.
    /// Call once per op, before it, after all its bindings are declared.
    /// Only valid on a slot that was declared (active).
    [[nodiscard]] cc::small_vector<dx12_subresource_barrier, 4> flush(sg::command_list_slot slot)
    {
        int const i = int(slot);
        CC_ASSERT(i < _slots.size() && _slots[i].active, "flush of a texture this list never declared");
        auto& s = _slots[i];
        s.pending_barrier = false; // this op's declares are being flushed
        cc::small_vector<dx12_subresource_barrier, 4> out;
        s.partition.for_each_box_in(sg::subresource_range::whole(s.partition.extent()),
                                    [&](sg::subresource_range const& box_range, sg::resource_access_state& state)
                                    {
                                        if (!state.entry_begun)
                                            return; // this list never named the box
                                        if (!state.has_entry_requirement)
                                            state.capture_entry_requirement(); // the submit prepends this one
                                        else if (!state.has_pending_declares() && !state.has_pending_layout_change())
                                            return;
                                        auto const b = state.flush();
                                        if (b.needed)
                                            out.push_back({box_range, b});
                                    });
        s.partition.try_merge();
        return out;
    }

    /// Finalize `slot` when its command list is submitted: its layouts become the current ones, in submission order.
    ///
    /// Returns the barriers taking the texture from what it is really in now to what this list's first use of each box needs.
    /// They belong ahead of the list rather than inside it — the caller executes them from a command list prepended to the same submit —
    /// because a list that recorded second may submit first, and a barrier recorded against a guess would name the wrong LayoutBefore.
    /// Clears the slot; only valid on a slot this list declared, so it is active.
    /// Submit runs finalize + execute under one lock, so finalize order equals execute order.
    [[nodiscard]] cc::small_vector<dx12_subresource_barrier, 4> finalize(sg::command_list_slot slot)
    {
        // Only ever called for a texture this list actually touched: declare seeded the slot active and grew _slots.
        // So the slot exists and is active — see the command list's submit/reclaim paths.
        int const i = int(slot);
        CC_ASSERT(i < _slots.size() && _slots[i].active, "finalize of a texture this list never touched");
        auto& s = _slots[i];
        CC_ASSERT(!has_pending_declares(s.partition), "a declared texture access was never flushed by a GPU op");
        CC_ASSERT(_active_slot_count > 0, "finalize of a texture with no active slots");

        auto const out = entry_barriers(s);

        // Only the boxes this list actually used are committed.
        // A slot's partition starts empty, so its untouched boxes say nothing about the texture — assigning the whole
        // partition would reset the layout of every subresource the list never named.
        for (auto const& sbox : s.partition.boxes())
        {
            if (!sbox.state.has_entry_requirement)
                continue;

            auto committed = sbox.state;
            committed.clear_entry_requirement();
            _current.for_each_box_in(
                sbox.range, [&](sg::subresource_range const&, sg::resource_access_state& state) { state = committed; });
        }
        _current.try_merge();

        --_active_slot_count;
        s = slot_state{};
        return out;
    }

    /// Discard `slot` when its command list is dropped: the recorded work never runs, so just drop this texture's `active_slot_count` and clear the slot.
    /// No layout change — the current state is unchanged.
    void discard(sg::command_list_slot slot)
    {
        // Like finalize, only ever called for a texture this list touched (its slot is active).
        int const i = int(slot);
        CC_ASSERT(i < _slots.size() && _slots[i].active, "discard of a texture this list never touched");
        CC_ASSERT(_active_slot_count > 0, "discard of a texture with no active slots");
        --_active_slot_count;
        _slots[i] = slot_state{};
    }

    /// Test-and-set `slot`'s finalize-recorded flag: true the first time it is called for `slot`, false after, until the slot is cleared by finalize/discard.
    /// The command list uses it to add the texture to its touched set exactly once, in O(1), replacing a linear scan.
    [[nodiscard]] bool mark_recorded(sg::command_list_slot slot)
    {
        int const i = int(slot);
        CC_ASSERT(i >= 0, "mark_recorded with an invalid command_list_slot");
        while (_slots.size() <= i)
            _slots.push_back(slot_state{});
        auto& s = _slots[i];
        if (s.recorded)
            return false;
        s.recorded = true;
        return true;
    }

private:
    struct slot_state
    {
        // subresource_partition's default ctor is explicit, so the initializer names the type; `= {}` is copy-list-init and would not pick it.
        // Extent is set when the slot is seeded.
        sg::subresource_partition partition = sg::subresource_partition();
        bool active = false;
        bool recorded = false;        // this slot's command list has added the texture to its finalize set (dedup)
        bool pending_barrier = false; // declared for the current op, awaiting the pre-op flush (per-op dedup)
    };

    slot_state& slot_for(sg::command_list_slot slot)
    {
        int const i = int(slot);
        CC_ASSERT(i >= 0, "declare with an invalid command_list_slot");
        while (_slots.size() <= i)
            _slots.push_back(slot_state{});
        auto& s = _slots[i];
        if (!s.active)
        {
            s.active = true;
            s.partition = sg::subresource_partition(_current.extent()); // empty: a box enters at its own first declare
            ++_active_slot_count; // one more open command list is now using this texture
        }
        return s;
    }

    // The barriers satisfying each box's recorded entry requirement against the state the texture is really in.
    // Boxes the list never touched carry no requirement and are left where they are.
    cc::small_vector<dx12_subresource_barrier, 4> entry_barriers(slot_state const& s)
    {
        cc::small_vector<dx12_subresource_barrier, 4> out;
        for (auto const& sbox : s.partition.boxes())
        {
            if (!sbox.state.has_entry_requirement)
                continue;

            // Splits _current so its boxes align to this one, which is what makes each emitted range exact.
            auto const requirement = sbox.state;
            _current.for_each_box_in(sbox.range,
                                     [&](sg::subresource_range const& box_range, sg::resource_access_state& state)
                                     {
                                         if (auto const b = state.entry_barrier_for(requirement); b.needed)
                                             out.push_back({box_range, b});
                                     });
        }
        return out;
    }

    static bool has_pending_declares(sg::subresource_partition const& p)
    {
        for (auto const& box : p.boxes())
            if (box.state.has_pending_declares())
                return true;
        return false;
    }

    cc::small_vector<slot_state, 4> _slots; // indexed by command_list_slot
    sg::subresource_partition _current;     // between-lists state, as of the last submitted list (initially general)
    int _active_slot_count = 0;             // open command lists currently using this texture (active slots)
};
