#pragma once

#include <shaped-graphics/backend/resource_access.hh>
#include <shaped-graphics/fwd.hh>

/// The per-resource (or per-subresource) access-state machine that turns a stream of declared accesses
/// into a minimal set of barriers. A reusable, backend-neutral building block: a backend accumulates
/// declared accesses with `declare`, then calls `flush` before each GPU op to get the barrier to emit
/// (if any) and roll the state forward. Buffers keep one of these; textures keep one per covering box.
///
/// Three timelines keep barriers minimal:
///   curr_*           — what the *next* op will do (accumulated by declare, consumed by flush)
///   inflight_*       — everything issued since the last write / command-list start
///   barriered_read_* — the reads already synced against the last write (so read-after-read is free)

namespace sg
{
/// The barrier `flush` asks the backend to emit. `needed == false` means the access was a freebie and no
/// barrier is required. For buffers the layouts are always `general`; they matter for textures.
struct access_barrier
{
    bool needed = false;
    pipeline_stage_flags src_stages = pipeline_stage_flags::none;
    pipeline_stage_flags dst_stages = pipeline_stage_flags::none;
    access_flags src_access = access_flags::none;
    access_flags dst_access = access_flags::none;
    texture_layout src_layout = texture_layout::general;
    texture_layout dst_layout = texture_layout::general;
};

struct resource_access_state
{
    // curr — accumulated for the next op
    pipeline_stage_flags curr_read_stages = pipeline_stage_flags::none;
    access_flags curr_read_access = access_flags::none;
    pipeline_stage_flags curr_write_stages = pipeline_stage_flags::none;
    access_flags curr_write_access = access_flags::none;

    // reads already barriered against the last write (subset of inflight reads)
    pipeline_stage_flags barriered_read_stages = pipeline_stage_flags::none;
    access_flags barriered_read_access = access_flags::none;

    // in-flight since the last write / command-list start
    pipeline_stage_flags inflight_read_stages = pipeline_stage_flags::none;
    access_flags inflight_read_access = access_flags::none;
    pipeline_stage_flags inflight_write_stages = pipeline_stage_flags::none;
    access_flags inflight_write_access = access_flags::none;

    // buffers are always `general`; textures transition between layouts
    texture_layout curr_layout = texture_layout::general;
    texture_layout prev_layout = texture_layout::general;

    // queries
    [[nodiscard]] bool has_inflight_writes() const { return has_any(inflight_write_access); }
    [[nodiscard]] bool has_any_inflight_access() const
    {
        return has_any(inflight_read_access) || has_any(inflight_write_access);
    }
    [[nodiscard]] bool has_curr_writes() const { return has_any(curr_write_access); }
    [[nodiscard]] bool has_pending_layout_change() const { return curr_layout != prev_layout; }
    [[nodiscard]] pipeline_stage_flags all_inflight_stages() const
    {
        return inflight_read_stages | inflight_write_stages;
    }
    [[nodiscard]] access_flags all_inflight_access() const { return inflight_read_access | inflight_write_access; }
    [[nodiscard]] pipeline_stage_flags all_curr_stages() const { return curr_read_stages | curr_write_stages; }
    [[nodiscard]] access_flags all_curr_access() const { return curr_read_access | curr_write_access; }

    /// Accumulate one declared access into `curr`. `layout` is the layout the access needs (`general` for
    /// buffers). An unordered write, or a layout change, routes into the write bucket (a layout transition
    /// is an implicit read+write); everything else is a read.
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

    /// Compute the barrier that satisfies the accumulated `curr` access against in-flight work, roll the
    /// three timelines forward, and clear `curr`. Returns `{needed=false}` for a freebie (nothing to emit).
    [[nodiscard]] access_barrier flush()
    {
        access_barrier b;
        bool const layout_change = curr_layout != prev_layout;

        if (has_curr_writes() || layout_change)
        {
            if (!has_any_inflight_access() && !layout_change)
            {
                // First write with no prior access: take ownership, no barrier.
                inflight_read_stages = curr_read_stages;
                inflight_read_access = curr_read_access;
                inflight_write_stages = curr_write_stages;
                inflight_write_access = curr_write_access;
                barriered_read_stages = pipeline_stage_flags::none;
                barriered_read_access = access_flags::none;
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
        else if (has_any(curr_read_access))
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
                pipeline_stage_flags const new_stages = without(curr_read_stages, barriered_read_stages);
                access_flags const new_access = without(curr_read_access, barriered_read_access);
                if (has_any(new_stages) || has_any(new_access))
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

        curr_read_stages = pipeline_stage_flags::none;
        curr_read_access = access_flags::none;
        curr_write_stages = pipeline_stage_flags::none;
        curr_write_access = access_flags::none;
        return b;
    }

    /// True if any access has been declared for the next op but not yet flushed.
    [[nodiscard]] bool has_pending_declares() const { return has_any(curr_read_access) || has_any(curr_write_access); }

    /// Reset the timelines to a fresh state while preserving the achieved layout (so the committed layout
    /// carries into the next command list). Used at command-list release.
    void reset_keep_layout()
    {
        auto const layout = curr_layout;
        *this = resource_access_state{};
        curr_layout = layout;
        prev_layout = layout;
    }
};
} // namespace sg
