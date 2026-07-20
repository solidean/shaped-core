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
/// `auto const& self = acquire(cmd);` and reads the routine's members through `self`:
///
///   class my_routine : public sg::render_routine<my_routine>
///   {
///   public:
///       static void execute(sg::command_list& cmd, /* args */)
///       {
///           auto const& self = acquire(cmd);
///           // ... bind self._pipeline, dispatch ...
///       }
///   protected:
///       void init_declare(sg::context& ctx) override { /* acquire shaders + pipelines */ }
///   };
template <class Derived>
class render_routine : public render_routine_base
{
public:
    /// The per-context instance for Derived, fully initialized (declare + materialize) at the current
    /// reload generation — the reference a static execute() reads from.
    [[nodiscard]] static Derived const& acquire(command_list& cmd)
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
