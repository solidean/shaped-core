#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/fwd.hh> // sg::context, sg::command_list

/// Base class for a reusable, self-contained unit of GPU work — a post-process pass, a LUT bake, a mipmap generate, a texture copy.
/// It owns its own lazy, hot-reload-aware initialization, so a call site only has to ask for it and use it.
/// Concrete routines derive from the CRTP sg::render_routine (render_routine.hh), which adds the by-type acquire(cmd) entry point;
/// this base carries the phase engine they share.
///
/// Initialization is three phases, kept apart so async pipeline compilation can start long before a command list exists:
///
///   init_once        persistent one-time work, independent of shader content.
///                    Never re-runs, not even on hot reload — a CPU-computed noise buffer uploaded once, say.
///   init_declare     acquire shaders, acquire async pipelines (kicking off background compiles), kick off uploads.
///                    Records no GPU work and opens no command list.
///                    Re-runs after a reload.
///   init_materialize record GPU init work: dispatches, clears, LUT bakes.
///                    Re-runs after a reload.
///
/// Most routines need only init_declare; the other two default to no-ops.
/// Re-init is driven by sg's process-global reload generation (sg::reload_generation):
/// when it moves, the next ensure_* re-runs declare + materialize, while init_once state is preserved.
/// A routine reads that counter directly — it needs no library reference.
/// Instances live per-context in ctx.routines, so their cached GPU state dies with the context that built it — no stale handles across contexts.
///
/// Threading: one lock per routine, held across the phase callbacks, so concurrent acquires are safe and each phase runs exactly once —
/// the losers of the race block until the winner is done, then see it initialized.
/// It is the same lock acquire_exclusive hands out, so it guards the derived routine's own state too; see sg::render_routine.
/// A phase callback therefore must not call back into acquire/acquire_exclusive/prewarm for the same routine.
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

    virtual void init_once(context& ctx) { (void)ctx; }
    virtual void init_declare(context& ctx) { (void)ctx; }
    virtual void init_materialize(command_list& cmd) { (void)cmd; }

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
        bool once_done = false;
        cc::optional<u64> declared_generation;
        cc::optional<u64> materialized_generation;
    };

    /// Whether every phase has run at the CURRENT reload generation, so a tick has nothing left to do here.
    /// Read under the routine's lock, so it is a snapshot rather than a promise: a reload can land right after it.
    [[nodiscard]] bool is_initialized();

    /// The same, given a lock already held — cc::mutex is not recursive, so the exclusive path cannot re-take it.
    [[nodiscard]] routine_readiness own_readiness_locked(init_state const& s);

    /// Where this routine stands, INCLUDING everything it depends on.
    /// This is what try_acquire reports, because a routine whose dependency is not up is not usable either.
    /// Identical to own_readiness until dependency tokens exist to fold in.
    [[nodiscard]] routine_readiness readiness();

    /// Where this routine stands, ignoring anything it depends on.
    ///
    /// `failed` is unreachable while the phases are synchronous and cannot report an error; it exists because the
    /// state a caller branches on should not change shape when the mechanism behind it does.
    [[nodiscard]] routine_readiness own_readiness();

    /// Runs init_once (first time only), then init_declare (first time + after each reload).
    /// The prewarm entry point: call it before opening a command list so async compiles start as early as possible.
    void ensure_initialized_no_materialize(context& ctx);

    /// The above, then init_materialize.
    /// The context is reached through cmd.context().
    /// Safe to call every frame — a no-op once initialized at the current generation.
    void ensure_initialized(command_list& cmd);

    // The bodies of the two above, minus the locking — so they may call each other, which the entry points cannot: cc::mutex is not recursive.
    // Only ever called with `_init` already held, which is also how acquire_exclusive runs the phases under the guard it hands out.

    void ensure_initialized_no_materialize_impl(init_state& s, context& ctx);
    void ensure_initialized_impl(init_state& s, command_list& cmd);

    /// The process-global reload generation to compare against (sg::reload_generation).
    [[nodiscard]] static u64 current_generation();

    cc::mutex<init_state> _init;
};
