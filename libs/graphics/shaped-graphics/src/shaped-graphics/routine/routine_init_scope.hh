#pragma once

#include <clean-core/common/utility.hh> // cc::unit, cc::move
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/fwd.hh>

/// What an initializing routine is given, and the only way it reaches a command list.
///
/// A phase is a coroutine, so everything here is shaped around two facts about them:
/// a coroutine's REFERENCE parameter dangles across its first suspend, so this is a value type passed by value; and a
/// coroutine may resume on any worker at any time, so the command list is reached through a callback that cannot
/// itself await.

namespace sg::impl
{
/// One run of one routine's initialization, shared between the scope the phase holds and the tick driving it.
struct routine_run
{
    sg::context* ctx = nullptr;
    routine_registry* registry = nullptr;

    /// The reload generation this run belongs to.
    u64 generation = 0;

    /// Set by the tick when a reload makes this run obsolete.
    /// Cooperative: it is observed at `yield` and at `with_cmd`, so a phase with neither is abandoned rather than
    /// cancelled — its result is dropped, it just does not stop early.
    bool cancelled = false;
};
} // namespace sg::impl

/// The scope a routine's init phase runs in.
///
/// Copyable and small, because it is a coroutine parameter and has to be taken by value.
class sg::routine_init_scope
{
public:
    [[nodiscard]] context& context() const;

    /// The reload generation this initialization belongs to.
    /// A routine that folds it into a hash (to invalidate accumulated work on reload) reads it here.
    [[nodiscard]] u64 generation() const { return _run->generation; }

    /// Whether a reload has made this run obsolete.
    /// Checked automatically by `yield` and `with_cmd`; a long phase that does neither may test it itself.
    [[nodiscard]] bool is_cancelled() const { return _run->cancelled; }

    /// Record GPU init work into the routine system's shared command list.
    ///
    /// **Awaited, not merely called.** The list only exists inside a tick's window, and a phase may resume on a worker
    /// long after the tick that started it returned — so this parks until a window is open rather than reaching for a
    /// list that is not there, which would leave one open across an advance_epoch.
    ///
    /// `f` takes a `sg::command_list&` and is NOT a coroutine, which is what keeps a list from being held across an
    /// await: there is nowhere inside it to await.
    /// Every routine initialized in one tick records into the same list, so their init work batches into one submit.
    template <class F>
    [[nodiscard]] cc::shared_async<cc::unit> with_cmd(F f) const;

    /// A budget boundary inside one phase, and a cancellation check.
    ///
    /// The tick's budget is checked between routines, so a single long initialization overruns it by however long it takes.
    /// A phase that knows its work is chunky awaits this between chunks and becomes pacable.
    [[nodiscard]] cc::shared_async<cc::unit> yield() const;

private:
    friend class routine_registry;
    friend class render_routine_base;
    explicit routine_init_scope(std::shared_ptr<impl::routine_run> run) : _run(cc::move(run)) {}

    std::shared_ptr<impl::routine_run> _run;
};
