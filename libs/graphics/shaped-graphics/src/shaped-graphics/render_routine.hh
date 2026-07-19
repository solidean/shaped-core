#pragma once

#include <shaped-graphics/command_list.hh> // cmd.context()
#include <shaped-graphics/context.hh>      // ctx.routines
#include <shaped-graphics/render_routine_base.hh>
#include <shaped-graphics/routine_registry.hh>

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
        Derived& self = cmd.context().routines.template get_or_create<Derived>();
        self.ensure_initialized(cmd);
        return self;
    }

    /// Prewarm form: runs init_once + init_declare (kicking off async compiles) before a command list
    /// exists. Materialize happens later, on the first acquire(cmd).
    static Derived const& acquire_no_materialize(context& ctx)
    {
        Derived& self = ctx.routines.template get_or_create<Derived>();
        self.ensure_initialized_no_materialize(ctx);
        return self;
    }
};
} // namespace sg
