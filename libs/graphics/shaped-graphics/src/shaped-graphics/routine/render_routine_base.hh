#pragma once

#include <clean-core/common/utility.hh> // cc::unit
#include <clean-core/error/optional.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/fwd.hh> // sg::context, sg::command_list
#include <shaped-graphics/routine/routine_init_scope.hh>

/// Base class for a reusable, self-contained unit of GPU work — a post-process pass, a LUT bake, a mipmap generate, a texture copy.
/// Concrete routines derive from the CRTP sg::render_routine (render_routine.hh), which adds the acquire entry points;
/// this base carries the phase engine they share.
///
/// Initialization is two phases, both coroutines returning `cc::shared_async<cc::unit>`:
///
///   init_once  persistent one-time work, independent of shader content.
///              Never re-runs, not even on hot reload — a CPU-computed noise buffer uploaded once, say.
///   init       everything that depends on shader content: shaders, pipelines, dependency tokens, GPU init work.
///              Re-runs at each reload generation, and an in-flight one is cancelled when the generation moves.
///
/// Both default to a no-op, so a routine overrides only what it needs.
/// Re-init is driven by sg's process-global reload generation (sg::reload_generation), which the tick reads once per
/// tick — so a reload is observed at a frame boundary and never lands mid-frame.
/// Instances live per-context in ctx.routines, so their cached GPU state dies with the context that built it.
///
/// **Nothing here runs on the frame path.** `ctx.routines.tick()` is the only thing that drives a phase, and a caller
/// that asks for a routine either gets one that is already up or is told to come back.
///
/// Threading: `_init` guards the phase bookkeeping ONLY.
/// A coroutine cannot hold a cc::mutex across a suspend, so a routine's own members are written by `init` without the
/// lock, and the pending -> ready transition the tick publishes is the barrier a reader synchronizes with.

/// Where a routine stands, as three states rather than two.
///
/// "Still compiling" and "will never compile" produce the same answer to a caller that only draws, and collapsing them
/// is how a broken shader becomes a black rectangle that reports nothing.
/// The common branch is the same either way -- a caller that only draws tests is_ready() -- so the third state costs
/// nothing at the sites that ignore it and is the difference between a failure a test can assert on and one a person
/// has to notice.
enum class sg::routine_readiness
{
    pending, ///< initialization has not finished; try again after another tick
    ready,   ///< every phase ran at the current reload generation
    failed,  ///< initialization failed and will not succeed until something changes (a reload, an eviction)
};

/// What a FALLIBLE routine's execute reports back.
///
/// Most routines are not fallible: one whose dependency set is entirely static tokens is handed out only when its
/// whole subtree is ready, so when it reports ready, execute goes through and returns void.
/// A routine that acquires something dynamically during execution can still decline, and it has to SAY so — a caller
/// that cannot tell "declined" from "nothing to draw" is how a headless capture writes a blank image and reports
/// success.
enum class sg::routine_outcome
{
    executed, ///< the work was recorded
    declined, ///< something it needed was not ready; nothing was recorded
};

class sg::render_routine_base
{
public:
    virtual ~render_routine_base() = default;

    render_routine_base(render_routine_base const&) = delete;
    render_routine_base& operator=(render_routine_base const&) = delete;

protected:
    render_routine_base() = default;

    /// Declare from inside a phase that initialization cannot succeed at this reload generation.
    ///
    /// Building a shader is what fails today; an asset a routine cannot load is the same shape and will want it too.
    /// Either way the routine is not "still working on it": it stays failed until something changes, and collapsing
    /// that into pending is how a routine nobody can use becomes one nobody hears about.
    ///
    /// **A failed shader RELOAD does not reach here.** slib promotes a recompile only when it produced a value, so a
    /// bad edit leaves the last good shader in place and does not even bump the reload generation, so routines keep
    /// running on what they already built.
    /// What reaches here is a shader that was never good: the first compile failing, a missing package, or a context
    /// accepting no format any registered compiler produces.
    ///
    /// Cleared when the phases re-run at a new generation, so a genuine reload is a fresh verdict.
    /// The spelling changes once init is a coroutine — a failed await will resolve the init async on its error
    /// channel — but the state it reports does not.
    void fail_init();

    /// Persistent one-time work, independent of shader content.
    /// NEVER re-runs, not even on reload, and is never cancelled by one.
    virtual cc::shared_async<cc::unit> init_once(routine_init_scope scope);

    /// Everything that depends on shader content: acquire shaders, build pipelines, declare dependencies, record GPU
    /// init work through `scope.with_cmd`.
    ///
    /// Re-runs at each reload generation, and an in-flight one is cancelled when the generation moves — so it must be
    /// safe to abandon between `with_cmd` bodies.
    virtual cc::shared_async<cc::unit> init(routine_init_scope scope);

private:
    // The phase engine is driven by the CRTP's static entry points (acquire / prewarm), not by user code.
    template <class, class>
    friend class render_routine;
    // It hands the same lock on to its caller, so it needs to name what _init guards.
    template <class>
    friend class routine_guard;
    // The tick drives initialization, so it needs to ask whether a routine still wants driving and to run its phases.
    friend class routine_registry;

    /// Which phases have run, and at which reload generation.
    /// It shares _init with the derived routine's own state, so a phase runs only once even under a concurrent acquire.
    struct init_state
    {
        /// init_once has completed.
        /// It never runs again, whatever the generation does.
        bool once_done = false;

        /// The generation `init` last COMPLETED at.
        /// Readiness is this equalling the current one.
        cc::optional<u64> ready_generation;

        /// The phase currently running, if any.
        /// Phases are sequential, so there is at most one.
        cc::shared_async<cc::unit> in_flight;

        /// Whether `in_flight` is init_once rather than init.
        bool in_flight_is_once = false;

        /// The run the in-flight phase holds, so a reload can cancel it.
        std::shared_ptr<impl::routine_run> run;
    };

    /// Whether every phase has run at the CURRENT reload generation, so a tick has nothing left to do here.
    /// Read under the routine's lock, so it is a snapshot rather than a promise: a reload can land right after it.
    [[nodiscard]] bool is_initialized();

    /// The same, given a lock already held — cc::mutex is not recursive, so the exclusive path cannot re-take it.
    [[nodiscard]] routine_readiness own_readiness_locked(init_state const& s);

    /// Where this routine stands, ignoring anything it depends on.
    ///
    /// `failed` is unreachable while the phases are synchronous and cannot report an error; it exists because the
    /// state a caller branches on should not change shape when the mechanism behind it does.
    [[nodiscard]] routine_readiness own_readiness();

    /// One step of this routine's initialization, driven by the tick and never by anything else.
    ///
    /// Starts the next phase, collects one that has settled, or does nothing while one is running.
    /// Returns true when it brought the routine up, which is what the tick counts.
    bool advance_init(routine_registry& registry, context& ctx, u64 generation);

    /// Abandon an in-flight `init` because the generation moved.
    /// init_once is never cancelled.
    void cancel_init_for_reload(init_state& s);

    /// The tick's entry into the above: cancel an in-flight `init` that belongs to an older generation.
    /// Called once per tick, before anything is advanced, so one reload retires every stale run together.
    void cancel_init_for_reload_if_stale(u64 generation);

    /// Whether the tick still has something to do here at `generation`: a phase is running, or one has yet to start.
    ///
    /// Asked against the tick's OWN generation rather than the global one, which is what keeps a reload landing
    /// mid-tick from leaving the tick chasing a generation it is not driving.
    [[nodiscard]] bool needs_init(u64 generation);

    /// The process-global reload generation to compare against (sg::reload_generation).
    [[nodiscard]] static u64 current_generation();

    /// Set by fail_init from inside a phase, cleared when the phases re-run at a new generation.
    ///
    /// Atomic and beside _init rather than inside it: a phase runs on whichever worker the scheduler gave it, so this
    /// is written off the lock while a reader may be asking for readiness.
    cc::atomic<bool> _failed = false;

    cc::mutex<init_state> _init;
};
