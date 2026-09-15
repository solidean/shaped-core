#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/thread/async.hh>

// cc::async_backlog — the work a component started and does not await itself, kept so someone can wait for it to settle.
// The model is "Detached work" in libs/base/clean-core/docs/systems/async.md.

namespace cc::impl
{
struct async_backlog_state;
}

/// Work a component detached, tracked so a caller can wait for all of it to settle.
///
/// A cache that starts a compile on a miss, or an actor whose request promise nobody reads, detaches that work: it runs to a result whether or not anyone still holds it.
/// Tracking it is what makes "is everything this component started done?" answerable — a test awaits that before it ends, and a teardown before it destroys what the work references.
///
/// Entries are weak, so a backlog never keeps work alive, and a node that is gone or settled counts for nothing.
/// A node still cold is not waited for, since nothing may ever start it; it counts once something does.
///
/// Track the outermost node of what was detached: a node depending on a tracked one can still be finishing when settled() resolves.
/// A tracked node must be driven to a result by whoever owns it, since a settled() waiting on a manual node its producer abandoned stays parked.
///
/// Thread-safe: any thread may track while another waits.
struct cc::async_backlog
{
    async_backlog();
    ~async_backlog();

    async_backlog(async_backlog const&) = delete;
    async_backlog(async_backlog&&) = delete;
    async_backlog& operator=(async_backlog const&) = delete;
    async_backlog& operator=(async_backlog&&) = delete;

    /// Start `node` and track it, handing the same handle back — cc::async_start for work nobody will await.
    /// Tracked before it starts, so work that settles at once is never missed.
    template <class T, class E>
    shared_async<T, E> start(shared_async<T, E> node)
    {
        track(node);
        if (node != nullptr && node->is_cold() && impl::async_can_schedule_here())
            node->schedule();
        return node;
    }

    /// Track `node` without starting it: work already scheduled, or a manual node something else will push.
    template <class T, class E>
    void track(shared_async<T, E> const& node)
    {
        if (node != nullptr)
            track_node(async_node_weak(node));
    }

    /// An async resolving once every tracked node that has started has settled, including work tracked while it waits.
    ///
    /// Lazy like every other spelling: awaiting it is what starts the wait.
    /// It never fails, because a tracked node that failed is settled work rather than a failure of the backlog.
    /// Work that keeps re-arming itself never lets it resolve, so bound the wait where that cannot be ruled out.
    /// The node shares the backlog's state, so it may outlive the backlog itself.
    [[nodiscard]] shared_async<unit> settled() const;

    /// settled() over several backlogs, resolving once one pass over all of them finds nothing pending.
    /// List them upstream first: work a node tracks into a later backlog before it resolves is then never missed.
    [[nodiscard]] static shared_async<unit> settled(cc::span<async_backlog const* const> backlogs);

    /// How many tracked nodes have started and not yet settled.
    /// Racy on live work, for diagnostics and tests rather than control flow.
    [[nodiscard]] isize outstanding_count() const;

private:
    void track_node(async_node_weak node);

    cc::shared_ptr<impl::async_backlog_state> _state;
};
