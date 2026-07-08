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
/// touch from `committed` (the between-lists state) plus an `entry` snapshot of it. Only the submit that
/// brings the live list count to zero promotes its partition into `committed` (the one case that may leave
/// the texture in a new layout); any other submit restores the texture to that list's entry layout.
class dx12_texture_access
{
public:
    explicit dx12_texture_access(sg::subresource_extent extent) : _committed(extent) {}

    /// Declare `stages`/`access`/`layout` over `range` for `slot` (seeding from committed on first touch),
    /// roll the covered boxes forward, and return the per-box barriers to emit before the op (empty = all
    /// freebies).
    [[nodiscard]] cc::small_vector<dx12_subresource_barrier, 4> declare(sg::command_list_slot slot,
                                                                        sg::subresource_range range,
                                                                        sg::pipeline_stage_flags stages,
                                                                        sg::access_flags access,
                                                                        sg::texture_layout layout)
    {
        auto& s = slot_for(slot);
        cc::small_vector<dx12_subresource_barrier, 4> out;
        s.partition.for_each_box_in(range,
                                    [&](sg::subresource_range const& box_range, sg::resource_access_state& state)
                                    {
                                        state.declare(stages, access, layout);
                                        auto const b = state.flush();
                                        if (b.needed)
                                            out.push_back({box_range, b});
                                    });
        s.partition.try_merge();
        return out;
    }

    /// Finalize `slot` when its command list is submitted. `promote` (this was the last open list) commits
    /// the slot's partition as the new between-lists state — the only case that may leave the texture in a
    /// new layout. Otherwise restore the texture to this list's entry layout (returning the transitions to
    /// emit) and warn. Clears the slot. No-op if the list never touched this texture.
    [[nodiscard]] cc::small_vector<dx12_subresource_barrier, 4> finalize(sg::command_list_slot slot, bool promote)
    {
        int const i = int(slot);
        cc::small_vector<dx12_subresource_barrier, 4> out;
        if (i >= _slots.size() || !_slots[i].active)
            return out;
        auto& s = _slots[i];
        CC_ASSERT(!has_pending_declares(s.partition), "a declared texture access was never flushed by a GPU op");
        if (promote)
            _committed = s.partition;
        else
            out = revert_to_entry(s);
        s = slot_state{};
        return out;
    }

    /// Discard `slot` when its command list is dropped: the recorded work never runs, so just clear the
    /// slot; `committed` is unchanged.
    void discard(sg::command_list_slot slot)
    {
        int const i = int(slot);
        if (i < _slots.size())
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
        sg::subresource_partition entry{};     // committed snapshot the list seeded from (revert target)
        bool active = false;
        bool recorded = false; // this slot's command list has added the texture to its finalize set (dedup)
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
            s.partition = _committed; // seed the private state from the between-lists state
            s.entry = _committed;     // and remember it as the revert target
        }
        return s;
    }

    // Restore each subresource box to the layout it had at this list's entry: for every box in the entry
    // snapshot, transition the (possibly diverged) current layout back to the entry layout.
    cc::small_vector<dx12_subresource_barrier, 4> revert_to_entry(slot_state& s)
    {
        cc::small_vector<dx12_subresource_barrier, 4> out;
        bool warned = false;
        for (auto const& ebox : s.entry.boxes())
        {
            sg::texture_layout const entry_layout = ebox.state.prev_layout;
            s.partition.for_each_box_in(
                ebox.range,
                [&](sg::subresource_range const& box_range, sg::resource_access_state& state)
                {
                    if (state.prev_layout == entry_layout)
                        return; // already in the entry layout
                    state.declare(sg::pipeline_stage_flags::none, sg::access_flags::none, entry_layout);
                    auto const b = state.flush();
                    if (b.needed)
                    {
                        out.push_back({box_range, b});
                        if (!warned)
                        {
                            cc::eprintln("[sg] reverting a texture to its entry layout at submit "
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
    sg::subresource_partition _committed;   // between-lists state (initial layout general)
};
} // namespace sg::backend::dx12
