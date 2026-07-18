#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/error/optional.hh>
#include <shaped-graphics/fwd.hh> // sg::context, sg::command_list
#include <shaped-rendering/fwd.hh>

#include <memory>

namespace sr
{
/// Base class for a reusable, self-contained unit of GPU work — a post-process pass, a LUT bake, a
/// mipmap generate, a texture copy. It owns its own lazy, hot-reload-aware initialization, so a call
/// site only has to ask for it and use it.
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
/// Most routines need only init_declare; the other two default to no-ops. Re-init is driven by slib's
/// process-global shader-reload generation: when a shader reloads it moves, and the next ensure_*
/// re-runs declare + materialize, while init_once state is preserved. A routine needs no library
/// reference for this — it reads the global counter directly.
///
/// The customary call shape is a static execute() on the subclass taking a routine_handle: the method
/// order then mirrors the run order, and execute() reads the routine's private members through the
/// initialized reference the handle returns. See libs/graphics/shaped-rendering/docs/render-routines.md.
class render_routine
{
public:
    /// Runs init_once (first time only), then init_declare (first time + after each reload). The prewarm
    /// entry point: call it before opening a command list so async compiles start as early as possible.
    void ensure_initialized_no_materialize(sg::context& ctx);

    /// The above, then init_materialize. The context is reached through cmd.context(). Safe to call every
    /// frame — a no-op once initialized at the current generation.
    void ensure_initialized(sg::command_list& cmd);

    virtual ~render_routine() = default;

    render_routine(render_routine const&) = delete;
    render_routine& operator=(render_routine const&) = delete;

protected:
    render_routine() = default;

    virtual void init_once(sg::context& ctx) { (void)ctx; }
    virtual void init_declare(sg::context& ctx) { (void)ctx; }
    virtual void init_materialize(sg::command_list& cmd) { (void)cmd; }

private:
    /// The process-global shader-reload generation to compare against (slib::current_reload_generation).
    [[nodiscard]] static u64 current_generation();

    bool _init_once_done = false;
    cc::optional<u64> _declared_generation;
    cc::optional<u64> _materialized_generation;
};

/// A cheap, copyable handle to a routine — a shared_ptr wrapper. A package creates one via
/// register_routine and exposes it as a member; call sites hold it and reach the routine through it.
template <class RoutineT>
class routine_handle
{
public:
    routine_handle() = default;
    explicit routine_handle(std::shared_ptr<RoutineT> routine) : _routine(cc::move(routine)) {}

    [[nodiscard]] bool is_valid() const { return _routine != nullptr; }

    void ensure_initialized_no_materialize(sg::context& ctx) const
    {
        CC_ASSERT(_routine != nullptr, "null routine_handle — create it via register_routine");
        _routine->ensure_initialized_no_materialize(ctx);
    }

    void ensure_initialized(sg::command_list& cmd) const
    {
        CC_ASSERT(_routine != nullptr, "null routine_handle — create it via register_routine");
        _routine->ensure_initialized(cmd);
    }

    /// Ensures the routine is initialized, then returns it — the reference a static execute() reads from.
    [[nodiscard]] RoutineT const& acquire(sg::command_list& cmd) const
    {
        CC_ASSERT(_routine != nullptr, "null routine_handle — create it via register_routine");
        _routine->ensure_initialized(cmd);
        return *_routine;
    }

    /// The underlying routine, without initializing it. Escape hatch.
    [[nodiscard]] std::shared_ptr<RoutineT> const& shared() const { return _routine; }

private:
    std::shared_ptr<RoutineT> _routine;
};
} // namespace sr
