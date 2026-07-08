#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/small_vector.hh>
#include <clean-core/string/print.hh>
#include <shaped-graphics/backend/command_list_slot.hh>
#include <shaped-graphics/backend/resource_access.hh>
#include <shaped-graphics/backend/resource_access_state.hh>
#include <shaped-graphics/backend/subresource.hh>
#include <shaped-graphics/pixel_format.hh>
#include <shaped-graphics/raw_texture.hh>

namespace sg::backend::dx12
{
/// A barrier the tracker asks the command list to emit, scoped to a subresource range. dx12-internal —
/// SG core never produces barriers; each backend owns its own tracking + emission.
struct dx12_subresource_barrier
{
    sg::subresource_range range;
    sg::access_barrier barrier;
};

/// The subresource grid a texture's access state partitions: mip × array-slice × aspect-plane. A cube is
/// 6 array slices per cube; a depth+stencil format has two aspect planes.
[[nodiscard]] inline sg::subresource_extent subresource_extent_of(sg::texture_description const& d)
{
    int const layers = d.array_layers.value_or(1) * (d.is_cube ? 6 : 1);
    return sg::subresource_extent{
        .mip_count = d.mip_levels,
        .array_count = layers,
        .aspect_count = sg::format_aspect_count(d.format),
    };
}

/// Per-texture, per-command-list access tracking — the dx12 realization of the covering-partition + slot
/// model. Pure logic (no D3D12 objects) so it is unit-testable without a device; dx12_texture holds one
/// under a mutex, the command list drives declare/finalize/discard and emits the returned barriers.
///
/// Each open command list keys its private covering partition by its command_list_slot, seeded on first
/// touch from `canonical` (the between-lists state). A per-texture `active_slot_count` tracks how many open
/// lists are using it; the finalize that drops it to zero — the *last* such list — promotes that list's
/// partition into `canonical` (the one case that may leave the texture in a new layout), and every earlier
/// finalize restores the texture to the canonical layout for the lists still using it.
class dx12_texture_access
{
public:
    explicit dx12_texture_access(sg::subresource_extent extent) : _canonical(extent) {}

    /// Accumulate one declared `stages`/`access`/`layout` over `range` for `slot` (seeding from canonical on
    /// first touch) into the next-op state, without emitting anything. Call once per binding — a texture
    /// bound several times to one op declares several times; `flush` then merges them per box. Thread-safe
    /// via the owning dx12_texture's mutex.
    ///
    /// If a box is declared twice for one op with two *different* target layouts, they must combine to a
    /// layout that satisfies both — for D3D12 that is COMMON (`general`), with a sampling-perf penalty worth
    /// a warning. That layout combining lands with the texture-views branch (where a binding group can bind
    /// one texture as several views); until then the conflicting case is caught below.
    void declare(sg::command_list_slot slot,
                 sg::subresource_range range,
                 sg::pipeline_stage_flags stages,
                 sg::access_flags access,
                 sg::texture_layout layout)
    {
        auto& s = slot_for(slot);
        s.partition.for_each_box_in(range,
                                    [&](sg::subresource_range const&, sg::resource_access_state& state)
                                    {
                                        CC_ASSERT(!state.has_pending_layout_change() || state.curr_layout == layout,
                                                  "a texture subresource was bound with two different layouts in "
                                                  "one operation (layout combining is not implemented yet)");
                                        state.declare(stages, access, layout);
                                    });
    }

    /// Test-and-set `slot`'s pending-barrier flag: true the first time it is called for `slot` since the last
    /// flush, false after. The command list uses it to enqueue the texture for the pre-op barrier flush
    /// exactly once, no matter how many times it is bound. `flush` clears it.
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

    /// Flush the accesses declared for `slot` since the last flush: for every subresource box with pending
    /// work, roll it forward and collect the per-box barrier (empty = all freebies). Merges multiple declares
    /// of the same box (a texture bound several times to one op) into one barrier. Call once per op, before
    /// it, after all its bindings are declared. Only valid on a slot that was declared (active).
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
                                        if (!state.has_pending_declares() && !state.has_pending_layout_change())
                                            return;
                                        auto const b = state.flush();
                                        if (b.needed)
                                            out.push_back({box_range, b});
                                    });
        s.partition.try_merge();
        return out;
    }

    /// Finalize `slot` when its command list is submitted. Decrements this texture's `active_slot_count`; if
    /// this was the **last** command list using the texture, its slot partition becomes the new canonical
    /// (between-lists) state — the only case that may leave the texture in a new layout. Otherwise each
    /// subresource whose layout diverged is transitioned back to the canonical layout (returning those
    /// barriers), so the texture is handed back unchanged for the lists still using it. Clears the slot.
    /// No-op if the list never touched this texture. Submit runs finalize + execute under one lock so
    /// finalize order = execute order; the decision itself is per-texture, under this object's mutex.
    [[nodiscard]] cc::small_vector<dx12_subresource_barrier, 4> finalize(sg::command_list_slot slot)
    {
        // Only ever called for a texture this list actually touched (declare seeded the slot active and
        // grew _slots), so the slot exists and is active — see the command list's submit/reclaim paths.
        int const i = int(slot);
        CC_ASSERT(i < _slots.size() && _slots[i].active, "finalize of a texture this list never touched");
        auto& s = _slots[i];
        CC_ASSERT(!has_pending_declares(s.partition), "a declared texture access was never flushed by a GPU op");
        CC_ASSERT(_active_slot_count > 0, "finalize of a texture with no active slots");
        cc::small_vector<dx12_subresource_barrier, 4> out;
        bool const was_last = --_active_slot_count == 0;
        if (was_last)
            _canonical = s.partition; // last one out commits its layout as the new canonical
        else
            out = revert_to_canonical(s); // others still using it — hand it back in the canonical layout
        s = slot_state{};
        return out;
    }

    /// Discard `slot` when its command list is dropped: the recorded work never runs, so just drop this
    /// texture's `active_slot_count` and clear the slot. No layout change — `canonical` is unchanged.
    void discard(sg::command_list_slot slot)
    {
        // Like finalize, only ever called for a texture this list touched (its slot is active).
        int const i = int(slot);
        CC_ASSERT(i < _slots.size() && _slots[i].active, "discard of a texture this list never touched");
        CC_ASSERT(_active_slot_count > 0, "discard of a texture with no active slots");
        --_active_slot_count;
        _slots[i] = slot_state{};
    }

    /// Test-and-set `slot`'s finalize-recorded flag: true the first time it is called for `slot`, false
    /// after (until the slot is cleared by finalize/discard). The command list uses it to add the texture to
    /// its touched set exactly once, in O(1) — replacing a linear scan.
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
        // Default-init braces are load-bearing: subresource_partition's default ctor is explicit, so
        // aggregate `slot_state{}` needs them.
        sg::subresource_partition partition{}; // this list's private state (extent set when seeded)
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
            s.partition = _canonical; // seed the private state from the between-lists (canonical) state
            ++_active_slot_count;     // one more open command list is now using this texture
        }
        return s;
    }

    // Restore each subresource box to the canonical layout: for every box in the canonical
    // partition, transition the (possibly diverged) current layout back to it. While a list uses the
    // texture its active_slot_count is >= 1, so no other list can promote it — canonical is stable across
    // that list's lifetime, hence "the layout it entered with" and "the canonical layout" are the same.
    cc::small_vector<dx12_subresource_barrier, 4> revert_to_canonical(slot_state& s)
    {
        cc::small_vector<dx12_subresource_barrier, 4> out;
        bool warned = false;
        for (auto const& cbox : _canonical.boxes())
        {
            sg::texture_layout const canonical_layout = cbox.state.prev_layout;
            s.partition.for_each_box_in(
                cbox.range,
                [&](sg::subresource_range const& box_range, sg::resource_access_state& state)
                {
                    if (state.prev_layout == canonical_layout)
                        return; // already in the canonical layout
                    state.declare(sg::pipeline_stage_flags::none, sg::access_flags::none, canonical_layout);
                    auto const b = state.flush();
                    if (b.needed)
                    {
                        out.push_back({box_range, b});
                        if (!warned)
                        {
                            cc::eprintln("[sg] reverting a texture to its canonical layout at submit "
                                         "because other command lists are still open (a hidden "
                                         "cost of concurrent recording)");
                            warned = true;
                        }
                    }
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
    sg::subresource_partition _canonical;   // between-lists state (initial layout general)
    int _active_slot_count = 0;             // open command lists currently using this texture (active slots)
};
} // namespace sg::backend::dx12
