#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/small_vector.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/barrier/command_list_slot.hh>
#include <shaped-graphics/barrier/resource_access_state.hh>

/// Per-command-list access tracking for one buffer, plus the canonical state carried between lists.
///
/// **This is where vulkan diverges most from dx12.**
/// A dx12 buffer has no between-lists state at all: D3D12 decays a buffer to COMMON at ExecuteCommandLists, so
/// cross-list ordering rides on that decay and only *intra-list* hazards ever produce a barrier — which is why
/// `dx12_buffer::finalize_slot` is a no-op that just clears the slot.
///
/// Vulkan has no such decay.
/// Submission order alone is execution order, not a memory dependency, so a write recorded in one list and a read
/// recorded in the next are unsynchronized unless something says otherwise.
/// The last writer therefore has to survive its own command list, which is what `canonical` below is for.
///
/// The model is the one dx12 already uses for *textures* — canonical state, per-slot state seeded from it, and a
/// promote when the last list using the resource finalizes — minus the subresource partition, since a buffer is one
/// undivided state.
/// Everything here is pure logic; the owning buffer wraps it in a mutex.

struct sg::backend::vulkan::vulkan_buffer_access
{
    struct slot_state
    {
        sg::resource_access_state state;
        bool active = false;          // this slot's list has touched the buffer since it started tracking
        bool recorded = false;        // this slot's list has added the buffer to its finalize set (dedup)
        bool pending_barrier = false; // declared for the current op, awaiting the pre-op flush (per-op dedup)
    };

    /// What previously-submitted lists left in flight, and what a fresh slot seeds from.
    sg::resource_access_state canonical;

    /// How many open lists are currently tracking this buffer.
    /// While it is non-zero no other list can promote, so canonical is stable for a tracking list's whole lifetime.
    int active_slot_count = 0;

    cc::small_vector<slot_state, 4> slots; // indexed by command_list_slot; SVO for a few concurrent lists

    /// The state for `slot`, seeded from canonical on first touch.
    [[nodiscard]] slot_state& slot_for(sg::command_list_slot slot)
    {
        auto const index = int(slot);
        CC_ASSERT(index >= 0, "an invalid command_list_slot cannot track access");
        while (isize(slots.size()) <= index)
            slots.push_back(slot_state{});

        auto& s = slots[index];
        if (!s.active)
        {
            s.state = canonical;
            s.active = true;
            ++active_slot_count;
        }
        return s;
    }

    /// Accumulate one declared access for the next op; emits nothing.
    /// Called once per binding, so a buffer bound several times to one op declares several times and `flush` merges
    /// them into a single barrier.
    void declare(sg::command_list_slot slot, sg::pipeline_stage_flags stages, sg::access_flags access)
    {
        slot_for(slot).state.declare(stages, access);
    }

    /// Test-and-set the per-op pending flag: true the first time since the last flush, false after.
    /// This is what makes a buffer bound several times to one op appear once in the barrier batch.
    [[nodiscard]] bool mark_pending_barrier(sg::command_list_slot slot)
    {
        auto& s = slot_for(slot);
        if (s.pending_barrier)
            return false;
        s.pending_barrier = true;
        return true;
    }

    /// Test-and-set the finalize-set flag: true the first time for this slot, false until the slot is cleared.
    [[nodiscard]] bool mark_recorded(sg::command_list_slot slot)
    {
        auto& s = slot_for(slot);
        if (s.recorded)
            return false;
        s.recorded = true;
        return true;
    }

    /// The single barrier satisfying everything declared for `slot` since the last flush.
    /// `needed == false` when the accesses are already ordered and nothing has to be emitted.
    [[nodiscard]] sg::access_barrier flush(sg::command_list_slot slot)
    {
        auto& s = slot_for(slot);
        s.pending_barrier = false;
        return s.state.flush();
    }

    /// The list holding `slot` was submitted: its recorded work will run, so what it leaves in flight becomes what
    /// the next list must synchronize against.
    ///
    /// Only the last list to finalize promotes, matching dx12's texture rule.
    /// While any list is still tracking, canonical must not move under it — it is what that list's slot was seeded
    /// from, and what its already-recorded barriers were computed against.
    void finalize(sg::command_list_slot slot)
    {
        auto& s = slot_for(slot);
        CC_ASSERT(!s.state.has_pending_declares(), "a declared buffer access was never flushed by a GPU op");

        --active_slot_count;
        if (active_slot_count == 0)
            canonical = s.state;
        s = slot_state{};
    }

    /// The list holding `slot` was dropped: its work never runs, so it leaves nothing behind and canonical is
    /// untouched — including when it was the last tracking list.
    void discard(sg::command_list_slot slot)
    {
        auto& s = slot_for(slot);
        --active_slot_count;
        s = slot_state{};
    }
};
