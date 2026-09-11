#pragma once

#include <clean-core/common/utility.hh>                 // cc::move
#include <clean-core/thread/mutex.hh>                   // cc::mutex_guard
#include <shaped-graphics/command_list/command_list.hh> // cmd.context()
#include <shaped-graphics/context/context.hh>           // ctx.routines
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/routine/render_routine_base.hh>
#include <shaped-graphics/routine/routine_params.hh>
#include <shaped-graphics/routine/routine_registry.hh>

/// Exclusive, mutable access to a render routine's per-context instance — what acquire_exclusive returns.
/// The routine is reached through -> and *.
///
/// It holds the routine's lock for its whole lifetime, which is why a routine needs no mutex of its own.
/// Keep it to the scope that actually mutates.
/// The lock is not recursive, so the *same* routine must never be acquired again while a guard on it is alive.
/// Acquiring a *different* routine under it is fine, as long as every path takes the two in the same order.
template <class Derived>
class sg::routine_guard
{
public:
    [[nodiscard]] Derived& operator*() const { return *_routine; }
    [[nodiscard]] Derived* operator->() const { return _routine; }

    routine_guard(routine_guard&&) = default;
    routine_guard& operator=(routine_guard&&) = default;

    routine_guard(routine_guard const&) = delete;
    routine_guard& operator=(routine_guard const&) = delete;

private:
    template <class, class>
    friend class render_routine;

    explicit routine_guard(Derived& routine, cc::mutex_guard<render_routine_base::init_state> lock)
      : _routine(&routine), _lock(cc::move(lock))
    {
    }

    Derived* _routine;
    // The phase engine's lock is the routine's lock; what it guards is the whole of *_routine, not just the phase flags.
    cc::mutex_guard<render_routine_base::init_state> _lock;
};

/// CRTP base for a concrete render routine.
/// Derive as
///
///   class my_routine : public sg::render_routine<my_routine> { ... };
///
/// and the routine gets a by-type entry point — no handle, no registration call, no by-name lookup.
/// Both entry points find (or lazily create) this routine's per-context instance in cmd.context().routines and initialize it;
/// they differ in what they hand back, and that is how a routine says whether it mutates.
///
///   acquire(cmd)            -> Derived const&           no lock held; only const members are reachable
///   acquire_exclusive(cmd)  -> routine_guard<Derived>   holds the routine's lock; the routine is fully mutable through it
///
/// A routine is expected to *hold state* — pipelines keyed by target format, a resource registry, a scratch buffer it grows.
/// So acquire_exclusive is the usual one, and the customary shape is a static execute() that opens with it:
///
///   class my_routine : public sg::render_routine<my_routine>
///   {
///   public:
///       static void execute(sg::command_list& cmd, /* args */)
///       {
///           auto self = acquire_exclusive(cmd);
///           // ... bind self's pipelines, dispatch ...
///       }
///   protected:
///       void init_declare(sg::context& ctx) override { /* acquire shaders + pipelines; already under the lock */ }
///   };
///
/// Threading, in three parts:
///
///   - the registry is guarded, so acquiring from parallel command-list recording is safe;
///   - one lock per routine covers both the init phases and everything the routine owns;
///     each phase therefore runs exactly once, and two threads recording the same routine serialize (see render_routine_base);
///   - **the const path is not locked.**
///     Whatever a routine exposes to acquire() must be immutable after init, or self-guarded (as sr::keyed_pipeline_cache is):
///     a reload on another thread re-runs init_declare while this thread reads.
///     A routine whose execute() touches anything init_declare writes belongs on acquire_exclusive — which is nearly all of them.
///
/// TODO(sg): the lock this wants is a shared/exclusive one, which clean-core does not have yet.
/// The model to reach: the init phases exclude every execute, while executes that only *read* run in parallel with each other.
/// Today acquire() takes no lock where it wants a shared one, and acquire_exclusive() serializes executes that could overlap.
/// It needs a cc::shared_mutex<T> (lock_shared(f) / lock_shared_scoped()) — see the sg TODO.
///
/// Do not clear()/evict() a registry while another thread is still recording against the same context.
template <class Derived, class Params>
class sg::render_routine : public render_routine_base
{
    static_assert(routine_params<Params>, "a routine parameter must be copyable and equality-comparable");

public:
    /// The parameter type this routine is keyed on, so a caller can name it without repeating the declaration.
    /// sg::routine_no_params for an unparametrized routine, which is every routine that does not say otherwise.
    using params_t = Params;

    /// The per-context instance for Derived, fully initialized (declare + materialize) at the current reload generation.
    /// No lock is held, so this reaches only const members — see the threading note above for what that requires of them.
    [[nodiscard]] static Derived const& acquire(command_list& cmd, Params const& params = {})
    {
        Derived& self = instance(cmd.context(), params);
        self.ensure_initialized(cmd);
        return self;
    }

    /// The same instance, mutable, with the routine's lock held for as long as the returned guard lives.
    /// This is the entry point for a routine that writes anything.
    [[nodiscard]] static routine_guard<Derived> acquire_exclusive(command_list& cmd, Params const& params = {})
    {
        Derived& self = instance(cmd.context(), params);
        auto lock = self._init.lock_scoped();
        // The phases run under the very lock the caller is about to hold, so a reload can never land mid-execute.
        self.ensure_initialized_impl(*lock, cmd);
        return routine_guard<Derived>(self, cc::move(lock));
    }

    /// Create the instance and run init_once + init_declare (kicking off async compiles) before a command list exists.
    /// Materialize happens later, on the first acquire(cmd).
    static void prewarm(context& ctx, Params const& params = {})
    {
        instance(ctx, params).ensure_initialized_no_materialize(ctx);
    }

    /// Drop ONE parametrization's instance on ctx, releasing its cached GPU resources.
    /// A no-op if it was never acquired there.
    static void evict(context& ctx, Params const& params = {})
    {
        ctx.routines.template evict<Derived>(impl::routine_params_hash(params));
    }

    /// Drop every parametrization of this routine on ctx.
    /// The same thing as evict() for an unparametrized routine, which has exactly one.
    static void evict_all(context& ctx) { ctx.routines.template evict_all<Derived>(); }

    /// The parameter this instance was created for.
    [[nodiscard]] Params const& params() const { return _params; }

private:
    /// Per-thread memo of the last instance handed out, so the steady state costs a pointer compare instead of a locked map lookup.
    /// Weak on purpose: a cached slot must never keep a routine alive past evict/clear/context shutdown — expiry is exactly what invalidates it.
    ///
    /// The parameter hash is part of what the memo matches on: a parametrized routine alternating between two values
    /// would otherwise be served the wrong instance.
    struct cache_entry
    {
        context* ctx = nullptr;
        u64 params_hash = 0;
        Derived* routine = nullptr;
        std::weak_ptr<Derived> alive;
    };

    /// The per-context instance for Derived at `params`, created on first use.
    [[nodiscard]] static Derived& instance(context& ctx, Params const& params)
    {
        auto const params_hash = impl::routine_params_hash(params);

        static thread_local cache_entry cache;
        // A live weak_ptr means the registry still owns it;
        // together with the context compare that also rules out a new context reusing a dead one's address.
        if (cache.ctx == &ctx && cache.params_hash == params_hash && !cache.alive.expired())
            return *cache.routine;

        auto const held = ctx.routines.template get_or_create<Derived>(params_hash,
                                                                       [&]
                                                                       {
                                                                           auto r = std::make_shared<Derived>();
                                                                           r->_params = params;
                                                                           return r;
                                                                       });
        // Two distinct parameters that hash alike would otherwise silently share one instance, which is a wrong
        // pipeline rather than a slow one.
        CC_ASSERT(held->params() == params, "routine parameter hash collision");
        cache = {&ctx, params_hash, held.get(), held};
        return *held;
    }

    Params _params = {};
};
