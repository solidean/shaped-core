#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/fwd.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/async.hh>

// cc::async_backlog — the work a component started and does not await itself, kept so someone can wait for it to settle.
// The model is "Detached work" in libs/base/clean-core/docs/systems/async.md.

namespace cc::impl
{
struct async_backlog_state;
}

namespace cc
{
/// Calls `f` for every live backlog, so a diagnostic can say what work is still outstanding and whose it is.
///
/// **This is the in-flight picture, and it covers only what was deliberately detached.**
/// A node awaited in the ordinary way is in nobody's list, and making every node enumerable would mean an atomic
/// pair on a path measured in nanoseconds — so this reports the work a component chose to track and says nothing
/// about the rest, rather than pretending to a completeness it does not have.
///
/// Never waits for the registry: a report must not hang on the thing it is reporting about.
/// False means `f` never ran.
/// A backlog's own counts, read through `outstanding_count()`, still take that backlog's lock.
[[nodiscard]] bool try_for_each_async_backlog(cc::function_ref<void(async_backlog const&)> f);

/// Writes every live backlog and what it still owes to stderr.
/// The rendering half, beside the query, so a crash and a hang print the same thing.
void report_async_backlogs(char const* reason) noexcept;
} // namespace cc

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
    /// `name` is what a diagnostic calls this backlog, and it is worth giving one.
    ///
    /// A hang report lists every live backlog and what each still owes; an unnamed one reports a count with nothing
    /// to attribute it to, which names the symptom and not the component.
    /// The view must outlive the backlog — a literal, or something the owner keeps.
    explicit async_backlog(cc::string_view name = {});
    ~async_backlog();

    /// What this backlog is called, or empty.
    [[nodiscard]] cc::string_view name() const { return _name; }

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
    /// The first round is taken when this is called, not when it is awaited: nothing pending hands back a resolved node,
    /// and work started after the call is waited for only if something it pinned tracks it.
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

    /// How many entries the backlog still holds, live or not.
    /// Tracking compacts it, so this stays bounded by the live entries whether or not anyone settles it.
    /// For diagnostics and tests.
    [[nodiscard]] isize tracked_count() const;

private:
    void track_node(async_node_weak node);

    cc::string_view _name;
    cc::shared_ptr<impl::async_backlog_state> _state;

    /// The process-wide list every live backlog is on, so a report can find them all.
    /// Threaded through the objects themselves rather than a container, since a backlog is not copyable or movable
    /// and a list needs no allocation.
    async_backlog* _registry_next = nullptr;
    async_backlog* _registry_prev = nullptr;

    friend bool cc::try_for_each_async_backlog(cc::function_ref<void(async_backlog const&)>);
};
