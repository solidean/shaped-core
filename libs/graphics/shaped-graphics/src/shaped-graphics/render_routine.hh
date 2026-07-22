#pragma once

#include <shaped-graphics/command_list.hh> // cmd.context()
#include <shaped-graphics/context.hh>      // ctx.routines
#include <shaped-graphics/render_routine_base.hh>
#include <shaped-graphics/routine_registry.hh>

#include <memory>

namespace sg
{
/// CRTP base for a concrete render routine. Derive as
///
///   class my_routine : public sg::render_routine<my_routine> { ... };
///
/// and the routine gets a by-type entry point — no handle, no registration call, no by-name lookup.
/// acquire(cmd) finds (or lazily creates) this routine's per-context instance in cmd.context().routines,
/// initializes it, and returns it. The customary shape is a static execute() that opens with
/// `auto& self = acquire(cmd);` and works through `self`:
///
///   class my_routine : public sg::render_routine<my_routine>
///   {
///   public:
///       static void execute(sg::command_list& cmd, /* args */)
///       {
///           auto& self = acquire(cmd);
///           // ... bind self's pipelines, dispatch ...
///       }
///   protected:
///       void init_declare(sg::context& ctx) override { /* acquire shaders + pipelines */ }
///   };
///
/// A routine is expected to *hold state* — pipelines keyed by target format, a resource registry, a
/// scratch buffer it grows. That is why acquire hands out a mutable reference rather than a const one.
///
/// Threading, in three parts:
///
///   - the registry is guarded, so acquiring from parallel command-list recording is safe;
///   - the phase engine is guarded, so each init phase runs exactly once even under a concurrent
///     acquire (see render_routine_base);
///   - **a routine's own mutable state is the routine's job to guard.** acquire() hands the same
///     instance to every caller on the context, so anything execute() writes must sit behind a
///     cc::mutex the routine owns. Putting all of it in one `cc::mutex<state>` and locking once per
///     entry point is the shape to reach for first — it keeps the rule checkable by inspection.
///
/// State written in init_declare and merely *read* later is no exception: a reload on another thread
/// re-runs init_declare while this thread is recording, so that state belongs behind the same mutex.
///
/// Do not clear()/evict() a registry while another thread is still recording against the same context.
template <class Derived>
class render_routine : public render_routine_base
{
public:
    /// The per-context instance for Derived, fully initialized (declare + materialize) at the current
    /// reload generation — the reference a static execute() works through.
    [[nodiscard]] static Derived& acquire(command_list& cmd)
    {
        Derived& self = instance(cmd.context());
        self.ensure_initialized(cmd);
        return self;
    }

    /// Create the instance and run init_once + init_declare (kicking off async compiles) before a command
    /// list exists. Materialize happens later, on the first acquire(cmd).
    static void prewarm(context& ctx) { instance(ctx).ensure_initialized_no_materialize(ctx); }

    /// Drop this routine's instance on ctx, releasing its cached GPU resources. A no-op if it was never
    /// acquired there.
    static void evict(context& ctx) { ctx.routines.template evict<Derived>(); }

private:
    /// Per-thread memo of the last instance handed out, so the steady state costs a pointer compare
    /// instead of a locked map lookup. Weak on purpose: a cached slot must never keep a routine alive
    /// past evict/clear/context shutdown — expiry is exactly what invalidates it.
    struct cache_entry
    {
        context* ctx = nullptr;
        Derived* routine = nullptr;
        std::weak_ptr<Derived> alive;
    };

    /// The per-context instance for Derived, created on first use.
    [[nodiscard]] static Derived& instance(context& ctx)
    {
        static thread_local cache_entry cache;
        // A live weak_ptr means the registry still owns it; together with the context compare that also
        // rules out a new context reusing a dead one's address.
        if (cache.ctx == &ctx && !cache.alive.expired())
            return *cache.routine;

        auto const held = ctx.routines.template get_or_create<Derived>();
        cache = {&ctx, held.get(), held};
        return *held;
    }
};
} // namespace sg
