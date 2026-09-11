#pragma once

#include <clean-core/common/assert.hh>
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
    [[nodiscard]] bool is_ready() const { return _readiness == routine_readiness::ready; }
    [[nodiscard]] bool is_pending() const { return _readiness == routine_readiness::pending; }
    [[nodiscard]] bool is_failed() const { return _readiness == routine_readiness::failed; }
    [[nodiscard]] routine_readiness readiness() const { return _readiness; }

    /// The routine itself.
    /// Only valid while is_ready().
    [[nodiscard]] Derived& operator*() const
    {
        CC_ASSERT(is_ready(), "a routine was used while it was not ready — test is_ready() first");
        return *_routine;
    }
    [[nodiscard]] Derived* operator->() const { return &**this; }

    /// Redeem a dependency token minted by THIS routine's init.
    /// See routine_scope::acquire.
    template <class Other, class P>
    [[nodiscard]] Other const& acquire(routine_dependency<Other, P> const& token) const
    {
        CC_ASSERT(is_ready(), "a dependency was redeemed through a routine that is not ready");
        CC_ASSERT(token.is_valid(), "a dependency token was never minted — declare it with depend_on during init");
        return *token._target;
    }

    routine_guard(routine_guard&&) = default;
    routine_guard& operator=(routine_guard&&) = default;

    routine_guard(routine_guard const&) = delete;
    routine_guard& operator=(routine_guard const&) = delete;

private:
    template <class, class>
    friend class render_routine;

    explicit routine_guard(Derived& routine,
                           cc::mutex_guard<render_routine_base::init_state> lock,
                           routine_readiness readiness)
      : _routine(&routine), _lock(cc::move(lock)), _readiness(readiness)
    {
    }

    Derived* _routine;
    routine_readiness _readiness;
    // The phase engine's lock is the routine's lock; what it guards is the whole of *_routine, not just the phase flags.
    cc::mutex_guard<render_routine_base::init_state> _lock;
};

/// A promise, minted during one routine's init, that another routine is ready whenever the holder is.
///
/// The framework refuses to hand out the holder until its whole token subtree is ready, so redeeming a token during
/// execution cannot fail — which is what keeps the branch count at one per entry into the routine system rather than
/// one per routine.
///
/// It holds a STRONG reference: a depended-on routine cannot be evicted while a dependent exists, so redemption needs
/// no liveness check at all.
/// It is redeemed through a scope or a guard rather than off the routine, so a token used somewhere its holder is not
/// acquired does not compile.
template <class Other, class Params>
class sg::routine_dependency
{
public:
    routine_dependency() = default;

    /// False until an init minted it — a default-constructed token names nothing.
    [[nodiscard]] bool is_valid() const { return _target != nullptr; }

    /// The parameter the dependency was minted for.
    [[nodiscard]] Params const& params() const { return _params; }

private:
    template <class, class>
    friend class render_routine;
    template <class>
    friend class routine_scope;
    template <class>
    friend class routine_guard;

    explicit routine_dependency(std::shared_ptr<Other> target, Params params)
      : _target(cc::move(target)), _params(cc::move(params))
    {
    }

    std::shared_ptr<Other> _target;
    Params _params = {};
};

/// Read-only access to a routine's per-context instance, plus where it stands — what try_acquire hands back.
///
/// It is a SCOPE rather than a bare reference for two reasons that arrive together: a caller has to be able to ask
/// whether the routine is usable before using it, and a dependency token is redeemed THROUGH this rather than off the
/// routine, so "valid only while the holder is acquired" is structural instead of an assert that release compiles out.
///
/// No operator bool: the states are named, so a call site says which one it is testing.
template <class Derived>
class sg::routine_scope
{
public:
    [[nodiscard]] bool is_ready() const { return _readiness == routine_readiness::ready; }
    [[nodiscard]] bool is_pending() const { return _readiness == routine_readiness::pending; }
    [[nodiscard]] bool is_failed() const { return _readiness == routine_readiness::failed; }
    [[nodiscard]] routine_readiness readiness() const { return _readiness; }

    /// The routine itself.
    /// Only valid while is_ready().
    [[nodiscard]] Derived const& operator*() const
    {
        CC_ASSERT(is_ready(), "a routine was used while it was not ready — test is_ready() first");
        return *_routine;
    }
    [[nodiscard]] Derived const* operator->() const { return &**this; }

    /// Redeem a dependency token minted by THIS routine's init.
    ///
    /// Infallible: the framework refused to hand *this* out until the whole token subtree was ready.
    /// Only reachable through a scope, which is what makes "valid while the holder is acquired" structural.
    template <class Other, class P>
    [[nodiscard]] Other const& acquire(routine_dependency<Other, P> const& token) const
    {
        CC_ASSERT(is_ready(), "a dependency was redeemed through a routine that is not ready");
        CC_ASSERT(token.is_valid(), "a dependency token was never minted — declare it with depend_on during init");
        return *token._target;
    }

    routine_scope(routine_scope&&) = default;
    routine_scope& operator=(routine_scope&&) = default;

    routine_scope(routine_scope const&) = delete;
    routine_scope& operator=(routine_scope const&) = delete;

private:
    template <class, class>
    friend class render_routine;

    explicit routine_scope(Derived& routine, routine_readiness readiness) : _routine(&routine), _readiness(readiness) {}

    Derived* _routine;
    routine_readiness _readiness;
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
        return routine_guard<Derived>(self, cc::move(lock), self.own_readiness_locked(*lock));
    }

    /// Where this routine stands, without initializing it — the fallible entry point.
    ///
    /// It reports; it never brings a routine up.
    /// That is the tick's job, so a routine nothing has ticked reads as pending here rather than quietly initializing
    /// on the frame path.
    /// Registering it is not initializing it: asking is enough to make the next tick bring it up.
    [[nodiscard]] static routine_scope<Derived> try_acquire(command_list& cmd, Params const& params = {})
    {
        return try_acquire(cmd.context(), params);
    }

    /// The same, reachable before a command list exists.
    [[nodiscard]] static routine_scope<Derived> try_acquire(context& ctx, Params const& params = {})
    {
        Derived& self = instance(ctx, params);
        return routine_scope<Derived>(self, self.readiness());
    }

    /// The same, mutable, holding the routine's lock — for a routine that writes anything.
    /// Unlike acquire_exclusive it does not initialize, so the returned guard may report pending.
    [[nodiscard]] static routine_guard<Derived> try_acquire_exclusive(command_list& cmd, Params const& params = {})
    {
        return try_acquire_exclusive(cmd.context(), params);
    }

    [[nodiscard]] static routine_guard<Derived> try_acquire_exclusive(context& ctx, Params const& params = {})
    {
        Derived& self = instance(ctx, params);
        auto lock = self._init.lock_scoped();
        auto const readiness = self.own_readiness_locked(*lock);
        return routine_guard<Derived>(self, cc::move(lock), readiness);
    }

    /// Register this routine so the next `ctx.routines.tick()` brings it up, before anything needs it.
    ///
    /// It no longer runs the phases itself: initialization is the tick's job, and a routine that is merely prewarmed
    /// is *registered*, not ready.
    /// Prewarming the top of a renderer is what lets a whole frame's worth of compiles start in one tick rather than
    /// being discovered one acquire at a time.
    static void prewarm(context& ctx, Params const& params = {}) { (void)instance(ctx, params); }

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

protected:
    /// Declare that this routine needs another one, and get the token that reaches it during execution.
    ///
    /// Call it from init.
    /// The edge is recorded, so this routine is not handed out until `Other` is ready too, and a cycle is refused
    /// where the edge is declared rather than discovered as a hang.
    /// Minting every token before awaiting anything is what lets a whole subtree start together.
    template <class Other, class P = typename Other::params_t>
    [[nodiscard]] routine_dependency<Other, P> depend_on(context& ctx, P const& params = {})
    {
        auto target = Other::shared_instance(ctx, params);
        ctx.routines.add_dependency(this, target);
        return routine_dependency<Other, P>(cc::move(target), params);
    }

private:
    // depend_on reaches another routine's shared_instance, so every instantiation is a friend of every other.
    // The alternative is making that public, which would hand any caller a way to keep a routine alive behind the
    // registry's back.
    template <class, class>
    friend class render_routine;

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

    /// The per-context instance for Derived at `params` as a shared owner, created on first use.
    /// A dependency token holds one of these, which is what pins a depended-on routine against eviction.
    [[nodiscard]] static std::shared_ptr<Derived> shared_instance(context& ctx, Params const& params)
    {
        auto held = ctx.routines.template get_or_create<Derived>(impl::routine_params_hash(params),
                                                                 [&]
                                                                 {
                                                                     auto r = std::make_shared<Derived>();
                                                                     r->_params = params;
                                                                     return r;
                                                                 });
        // Two distinct parameters that hash alike would otherwise silently share one instance, which is a wrong
        // pipeline rather than a slow one.
        CC_ASSERT(held->params() == params, "routine parameter hash collision");
        return held;
    }

    /// The per-context instance for Derived at `params`, created on first use.
    [[nodiscard]] static Derived& instance(context& ctx, Params const& params)
    {
        auto const params_hash = impl::routine_params_hash(params);

        static thread_local cache_entry cache;
        // A live weak_ptr means the registry still owns it;
        // together with the context compare that also rules out a new context reusing a dead one's address.
        if (cache.ctx == &ctx && cache.params_hash == params_hash && !cache.alive.expired())
            return *cache.routine;

        auto const held = shared_instance(ctx, params);
        cache = {&ctx, params_hash, held.get(), held};
        return *held;
    }

    Params _params = {};
};
