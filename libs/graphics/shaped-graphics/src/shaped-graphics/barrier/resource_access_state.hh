#pragma once

#include <shaped-graphics/barrier/resource_access.hh>
#include <shaped-graphics/fwd.hh>

/// The per-resource (or per-subresource) access-state machine that turns a stream of declared accesses into a minimal set of barriers.
/// A reusable, backend-neutral building block: a backend accumulates declared accesses with `declare`, then calls `flush` before each GPU op.
/// `flush` returns the barrier to emit, if any, and rolls the state forward.
/// Buffers keep one of these; textures keep one per covering box.
/// See libs/graphics/shaped-graphics/docs/concepts/barriers.md for why the machine is shaped this way.
///
/// The three timelines that keep barriers minimal:
///   curr_*           — what the *next* op will do (accumulated by declare, consumed by flush)
///   inflight_*       — everything issued since the last write / command-list start
///   barriered_read_* — the reads already synced against the last write (so read-after-read is free)

/// The barrier `flush` asks the backend to emit.
/// `needed == false` means the access was a freebie and no barrier is required.
/// For buffers the layouts are always `general`; they matter for textures.
struct sg::access_barrier
{
    bool needed = false;
    pipeline_stage_flags src_stages = {};
    pipeline_stage_flags dst_stages = {};
    access_flags src_access = {};
    access_flags dst_access = {};
    texture_layout src_layout = texture_layout::general;
    texture_layout dst_layout = texture_layout::general;
};

struct sg::resource_access_state
{
    // curr — accumulated for the next op
    pipeline_stage_flags curr_read_stages = {};
    access_flags curr_read_access = {};
    pipeline_stage_flags curr_write_stages = {};
    access_flags curr_write_access = {};

    // reads already barriered against the last write (subset of inflight reads)
    pipeline_stage_flags barriered_read_stages = {};
    access_flags barriered_read_access = {};

    // in-flight since the last write / command-list start
    pipeline_stage_flags inflight_read_stages = {};
    access_flags inflight_read_access = {};
    pipeline_stage_flags inflight_write_stages = {};
    access_flags inflight_write_access = {};

    // buffers are always `general`; textures transition between layouts
    texture_layout curr_layout = texture_layout::general;
    texture_layout prev_layout = texture_layout::general;

    // What this command list's FIRST op needs of the resource, recorded at that op's flush.
    // A list's private state starts empty and enters at this requirement rather than at whatever the resource
    // happened to be in while it recorded, so nothing it records is trusted about where the resource starts.
    // The submit resolves it against the state the resource is really in, and prepends the barrier that gets there.
    bool entry_begun = false;           // this list has declared against the resource at least once
    bool has_entry_requirement = false; // ...and its first op has flushed, so the requirement below is final
    pipeline_stage_flags entry_stages = {};
    access_flags entry_access = {};
    texture_layout entry_layout = texture_layout::general;

    // queries
    [[nodiscard]] bool has_inflight_writes() const { return !inflight_write_access.is_empty(); }
    [[nodiscard]] bool has_any_inflight_access() const
    {
        return !inflight_read_access.is_empty() || !inflight_write_access.is_empty();
    }
    [[nodiscard]] bool has_curr_writes() const { return !curr_write_access.is_empty(); }
    [[nodiscard]] bool has_pending_layout_change() const { return curr_layout != prev_layout; }
    [[nodiscard]] pipeline_stage_flags all_inflight_stages() const
    {
        return inflight_read_stages | inflight_write_stages;
    }
    [[nodiscard]] access_flags all_inflight_access() const { return inflight_read_access | inflight_write_access; }
    [[nodiscard]] pipeline_stage_flags all_curr_stages() const { return curr_read_stages | curr_write_stages; }
    [[nodiscard]] access_flags all_curr_access() const { return curr_read_access | curr_write_access; }

    /// Accumulate one declared access into `curr`.
    /// `layout` is the layout the access needs (`general` for buffers).
    /// An unordered write, or a layout change, routes into the write bucket — a layout transition is an implicit read+write; everything else is a read.
    void declare(pipeline_stage_flags stages, access_flags access, texture_layout layout = texture_layout::general)
    {
        if (is_unordered_write(access) || layout != prev_layout)
        {
            curr_write_stages |= stages;
            curr_write_access |= access;
            curr_layout = layout;
        }
        else
        {
            curr_read_stages |= stages;
            curr_read_access |= access;
        }
    }

    /// Compute the barrier that satisfies the accumulated `curr` access against in-flight work, roll the three timelines forward, and clear `curr`.
    /// Returns `{needed=false}` for a freebie, with nothing to emit.
    [[nodiscard]] access_barrier flush()
    {
        access_barrier b;
        bool const layout_change = curr_layout != prev_layout;

        if (has_curr_writes() || layout_change)
        {
            // The freebie below rests on the backend inferring the access itself at first use (D3D12 promotes a buffer out of COMMON on its first op).
            // It can only infer ONE, so an op that both reads and writes the same resource — a copy whose source and destination are the same buffer — must be spelled out.
            // Skipping the barrier there leaves D3D12 assuming COPY_DEST and rejecting the source read.
            if (!has_any_inflight_access() && !layout_change && !has_read_access(all_curr_access()))
            {
                // First write with no prior access: take ownership, no barrier.
                inflight_read_stages = curr_read_stages;
                inflight_read_access = curr_read_access;
                inflight_write_stages = curr_write_stages;
                inflight_write_access = curr_write_access;
                barriered_read_stages = {};
                barriered_read_access = {};
            }
            else
            {
                // Fully serialize: prevents WAW / WAR / RAW against everything in flight.
                b.needed = true;
                b.src_stages = all_inflight_stages();
                b.dst_stages = all_curr_stages();
                b.src_access = all_inflight_access();
                b.dst_access = all_curr_access();
                b.src_layout = prev_layout;
                b.dst_layout = curr_layout;

                inflight_read_stages = curr_read_stages;
                inflight_read_access = curr_read_access;
                inflight_write_stages = curr_write_stages;
                inflight_write_access = curr_write_access;
                // A real write leaves no reads barriered against the *new* write; a pure layout change
                // keeps the current reads (they were synced by this transition).
                barriered_read_stages = curr_read_stages;
                barriered_read_access = curr_read_access;
            }
            prev_layout = curr_layout;
        }
        else if (!curr_read_access.is_empty())
        {
            if (!has_inflight_writes())
            {
                // No writer in flight: reads run free, just widen the in-flight read set.
                inflight_read_stages |= curr_read_stages;
                inflight_read_access |= curr_read_access;
            }
            else
            {
                // Sync only the *new* reads (stages/access not already barriered) against the last write.
                pipeline_stage_flags const new_stages = curr_read_stages.without(barriered_read_stages);
                access_flags const new_access = curr_read_access.without(barriered_read_access);
                if (!new_stages.is_empty() || !new_access.is_empty())
                {
                    b.needed = true;
                    b.src_stages = inflight_write_stages;
                    b.dst_stages = new_stages;
                    b.src_access = inflight_write_access;
                    b.dst_access = new_access;
                    b.src_layout = prev_layout;
                    b.dst_layout = curr_layout;
                }
                inflight_read_stages |= curr_read_stages;
                inflight_read_access |= curr_read_access;
                barriered_read_stages |= curr_read_stages;
                barriered_read_access |= curr_read_access;
            }
        }

        curr_read_stages = {};
        curr_read_access = {};
        curr_write_stages = {};
        curr_write_access = {};
        return b;
    }

    /// True if any access has been declared for the next op but not yet flushed.
    [[nodiscard]] bool has_pending_declares() const
    {
        return !curr_read_access.is_empty() || !curr_write_access.is_empty();
    }

    /// Begin a command list's private view of the resource, entering at `layout`.
    /// Called on the first declare, before it, so that declare is classified by what it does rather than as a layout
    /// change out of whatever the resource happened to be in.
    void begin_entry(texture_layout layout)
    {
        entry_begun = true;
        curr_layout = layout;
        prev_layout = layout;
    }

    /// Record what the first op of a command list needs, and take that as the state the list enters in.
    /// Called once per state, immediately before that op's flush, so it sees the layout every declare of the op
    /// settled on rather than the first one's.
    /// Levelling `prev_layout` is what keeps the entry transition out of the list's own body: the prepended barrier
    /// puts the resource here before the list runs, so the body has nothing to transition from.
    void capture_entry_requirement()
    {
        has_entry_requirement = true;
        entry_stages = all_curr_stages();
        entry_access = all_curr_access();
        entry_layout = curr_layout;
        prev_layout = curr_layout;
    }

    /// The barrier taking this — the resource's real state between lists — to what `list_state`'s command list needs
    /// on entry, or `{needed=false}` when it is already there.
    /// Read at submit, in submission order, and emitted into the small command buffer prepended to that submit.
    [[nodiscard]] access_barrier entry_barrier_for(resource_access_state const& list_state) const
    {
        if (!list_state.has_entry_requirement)
            return {};

        auto probe = *this;
        probe.declare(list_state.entry_stages, list_state.entry_access, list_state.entry_layout);
        return probe.flush();
    }

    /// Drop the entry bookkeeping, which belongs to one command list and means nothing once it has been resolved.
    void clear_entry_requirement()
    {
        entry_begun = false;
        has_entry_requirement = false;
        entry_stages = {};
        entry_access = {};
        entry_layout = texture_layout::general;
    }

    /// Reset the timelines to a fresh state, preserving the achieved layout so the committed layout carries into the next command list.
    /// Used at command-list release.
    void reset_keep_layout()
    {
        auto const layout = curr_layout;
        *this = resource_access_state{};
        curr_layout = layout;
        prev_layout = layout;
    }
};
