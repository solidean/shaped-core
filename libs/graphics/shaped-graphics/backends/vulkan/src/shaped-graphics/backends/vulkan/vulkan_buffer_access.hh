#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/small_vector.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/barrier/command_list_slot.hh>
#include <shaped-graphics/barrier/resource_access_state.hh>

/// Per-command-list access tracking for one buffer, plus the state carried between lists.
///
/// **This is where vulkan diverges most from dx12.**
/// A dx12 buffer has no between-lists state at all: D3D12 decays a buffer to COMMON at ExecuteCommandLists, so
/// cross-list ordering rides on that decay and only *intra-list* hazards ever produce a barrier — which is why
/// `dx12_buffer::finalize_slot` is a no-op that just clears the slot.
///
/// Vulkan has no such decay.
/// Submission order alone is execution order, not a memory dependency, so a write recorded in one list and a read
/// recorded in the next are unsynchronized unless something says otherwise.
/// The last writer therefore has to survive its own command list, which is what `current` below is for.
///
/// The model is the one textures use — a current state between lists, a private per-slot state, and an entry
/// requirement resolved at submit — minus the subresource partition, since a buffer is one undivided state.
///
/// **A slot starts empty rather than seeded from `current`.**
/// Seeding meant a list computed its barriers against whatever the buffer was in *while it recorded*, and two lists
/// recording concurrently therefore never saw each other's declares: A's write and B's read both took the no-barrier
/// freebie, and submitting A then B left them unsynchronized.
/// Vulkan gives no implicit ordering between two `vkQueueSubmit` batches, so that was a real hazard, and
/// synchronization validation reports it as a READ_AFTER_WRITE.
/// (dx12 is safe by construction there: ExecuteCommandLists orders the batches, and a buffer decays to COMMON.)
/// Instead the list records what its first op needs, and the submit prepends the barrier that satisfies it against
/// the state the buffer is really in by then.
///
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

    /// What previously-submitted lists left in flight, updated by every finalize in submission order.
    sg::resource_access_state current;

    /// How many open lists are currently tracking this buffer.
    /// Diagnostic only: no decision hangs off it, since every finalize writes `current` and none reverts.
    int active_slot_count = 0;

    cc::small_vector<slot_state, 4> slots; // indexed by command_list_slot; SVO for a few concurrent lists

    /// The state for `slot`, started empty on first touch.
    [[nodiscard]] slot_state& slot_for(sg::command_list_slot slot)
    {
        auto const index = int(slot);
        CC_ASSERT(index >= 0, "an invalid command_list_slot cannot track access");
        while (isize(slots.size()) <= index)
            slots.push_back(slot_state{});

        auto& s = slots[index];
        if (!s.active)
        {
            s.state = sg::resource_access_state{};
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
        auto& s = slot_for(slot);
        if (!s.state.entry_begun)
            s.state.begin_entry(sg::texture_layout::general);
        s.state.declare(stages, access);
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
        if (s.state.entry_begun && !s.state.has_entry_requirement)
            s.state.capture_entry_requirement(); // this list's first op: its barrier is the submit's to prepend
        return s.state.flush();
    }

    /// The list holding `slot` was submitted: its recorded work will run, so what it leaves in flight becomes what
    /// the next list must synchronize against.
    ///
    /// Returns the barrier taking the buffer from what it is really in now to what this list's first op needs.
    /// It belongs ahead of the list rather than inside it — the caller prepends it to the same submit — because a
    /// list that recorded second may submit first, and a barrier recorded against a guess would name the wrong source.
    ///
    /// Called in submission order, which is what makes `current` mean "after everything submitted so far".
    [[nodiscard]] sg::access_barrier finalize(sg::command_list_slot slot)
    {
        auto& s = slot_for(slot);
        CC_ASSERT(!s.state.has_pending_declares(), "a declared buffer access was never flushed by a GPU op");

        auto const entry = current.entry_barrier_for(s.state);
        if (s.state.entry_begun) // a slot can be marked without ever declaring, and then it leaves nothing behind
        {
            current = s.state;
            current.clear_entry_requirement();
        }

        --active_slot_count;
        s = slot_state{};
        return entry;
    }

    /// The list holding `slot` was dropped: its work never runs, so it leaves nothing behind and `current` is
    /// untouched.
    void discard(sg::command_list_slot slot)
    {
        auto& s = slot_for(slot);
        --active_slot_count;
        s = slot_state{};
    }
};
