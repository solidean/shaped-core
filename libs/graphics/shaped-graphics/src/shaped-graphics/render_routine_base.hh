#pragma once

#include <clean-core/error/optional.hh>
#include <shaped-graphics/fwd.hh> // sg::context, sg::command_list

namespace sg
{
/// Base class for a reusable, self-contained unit of GPU work — a post-process pass, a LUT bake, a
/// mipmap generate, a texture copy. It owns its own lazy, hot-reload-aware initialization, so a call
/// site only has to ask for it and use it. Concrete routines derive from the CRTP sg::render_routine
/// (render_routine.hh), which adds the by-type acquire(cmd) entry point; this base carries the phase
/// engine they share.
///
/// Initialization is three phases, kept apart so async pipeline compilation can start long before a
/// command list exists:
///
///   init_once        persistent one-time work, independent of shader content. Never re-runs — not
///                    even on hot reload (e.g. a CPU-computed noise buffer, uploaded once).
///   init_declare     acquire shaders, acquire async pipelines (kicking off background compiles), kick
///                    off uploads. Records no GPU work and opens no command list. Re-runs after a reload.
///   init_materialize record GPU init work (dispatches, clears, LUT bakes). Re-runs after a reload.
///
/// Most routines need only init_declare; the other two default to no-ops. Re-init is driven by sg's
/// process-global reload generation (sg::reload_generation): when it moves, the next ensure_* re-runs
/// declare + materialize, while init_once state is preserved. A routine reads that counter directly —
/// it needs no library reference. Instances live per-context in ctx.routines, so their cached GPU state
/// dies with the context that built it — no stale handles across contexts.
class render_routine_base
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
    template <class>
    friend class render_routine;

    /// Runs init_once (first time only), then init_declare (first time + after each reload). The prewarm
    /// entry point: call it before opening a command list so async compiles start as early as possible.
    void ensure_initialized_no_materialize(context& ctx);

    /// The above, then init_materialize. The context is reached through cmd.context(). Safe to call every
    /// frame — a no-op once initialized at the current generation.
    void ensure_initialized(command_list& cmd);

    /// The process-global reload generation to compare against (sg::reload_generation).
    [[nodiscard]] static u64 current_generation();

    bool _init_once_done = false;
    cc::optional<u64> _declared_generation;
    cc::optional<u64> _materialized_generation;
};
} // namespace sg
