#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/fwd.hh>

/// The concurrency substrate for access tracking. Every command list allocates a dense slot index when it
/// is created and releases it on submit/drop. That slot keys the command list's private access-state entry
/// inside each resource it touches, so several command lists can record against the same resource at once
/// without sharing state. See libs/graphics/shaped-graphics/docs/concepts/barriers.md.

namespace sg
{
/// Per-command-list index into a resource's concurrent access-state slots. Compact (lowest free index
/// first), so a resource can index its slot storage directly.
enum class command_list_slot : int
{
    invalid = -1,
};

/// Hands out `command_list_slot`s: a mutex-guarded 64-bit free bitmask (index = lowest clear bit) with a
/// heap free-list overflow past 64 concurrently-open command lists. Crossing 64 emits a one-time warning
/// — that many concurrent recorders almost always means a command list was never submitted or dropped.
class command_list_slot_allocator
{
public:
    /// Acquire the lowest free slot.
    [[nodiscard]] command_list_slot acquire();

    /// Release a previously-acquired slot. Returns true iff no slots remain live afterwards — the
    /// "returns to zero" signal that lets the releasing command list promote its final layouts to
    /// canonical instead of reverting them.
    bool release(command_list_slot slot);

    /// Number of currently-live slots (open command lists).
    [[nodiscard]] int live_count();

private:
    struct state
    {
        u64 bits = 0;              // bit i set => slot i in [0, 64) is live
        cc::vector<bool> overflow; // overflow[j] set => slot 64+j is live
        int live = 0;
        bool overflow_warned = false;
    };
    cc::mutex<state> _state;
};
} // namespace sg
