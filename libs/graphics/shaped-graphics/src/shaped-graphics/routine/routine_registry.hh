#pragma once

#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/fwd.hh> // sg::context
#include <shaped-graphics/routine/render_routine_base.hh>
#include <shaped-graphics/routine/routine_params.hh>

#include <type_traits>

namespace sg
{
namespace impl
{
/// Unique, RTTI-free key per routine type: each instantiation owns a distinct static address.
template <class T>
[[nodiscard]] void const* routine_type_key()
{
    static char const key = 0;
    return &key;
}

/// What identifies one routine INSTANCE: its type, plus the hash of the parameter it was acquired with.
/// An unparametrized routine hashes to 0, so it keys exactly as it did when the type alone was the key.
struct routine_key
{
    void const* type = nullptr;
    u64 params_hash = 0;

    [[nodiscard]] bool operator==(routine_key const&) const = default;

    [[nodiscard]] friend u64 hash(routine_key const& k) { return cc::make_hash(u64(k.type), k.params_hash); }
};
} // namespace impl

} // namespace sg

/// Per-context storage of render-routine instances, reached as `ctx.routines`.
/// A routine is a per-context singleton:
/// the first acquire of a given type creates and registers it here (lazy self-registration — no explicit registration, no by-name lookup),
/// and it lives until the context is shut down or it is explicitly evicted.
///
/// Everything type-keyed is private and driven through sg::render_routine's statics (`R::acquire(cmd)` / `R::acquire_exclusive(cmd)` / `R::prewarm(ctx)` / `R::evict(ctx)`);
/// the only public operation here is clear().
/// A thin per-context sub-object like ctx.cached, created and destroyed with its context.
///
/// Map access is guarded, so acquiring is safe from parallel command-list recording.
/// What a routine holds itself is guarded by the routine's own lock instead, which acquire_exclusive hands to its caller (see sg::render_routine).
/// Do not clear()/evict() a registry while another thread is still recording against the same context.
/// What a tick is allowed to do.
struct sg::routine_tick_options
{
    /// How long the tick may spend, in seconds.
    ///
    /// **Advisory pacing, not a deadline.** It is checked BETWEEN routines, so a tick overruns it by however long the
    /// longest single initialization takes, and a caller that treats it as a frame deadline will eventually miss one
    /// and blame the wrong thing.
    /// Absent means "do everything pending", which is what a test or a loading screen wants.
    cc::optional<f64> budget_secs;
};

/// What a tick did.
struct sg::routine_tick_result
{
    int initialized = 0;           ///< routines this tick brought up
    int pending = 0;               ///< routines still waiting when it returned
    bool budget_exhausted = false; ///< it stopped early, so another tick has work to do

    /// Whether anything at all is left for a later tick.
    [[nodiscard]] bool is_idle() const { return pending == 0; }
};

class sg::routine_registry
{
public:
    /// Drive pending routine initialization, within an optional budget.
    ///
    /// **A frame-boundary call**: it opens and submits a command list of its own, so it must not run inside one, and
    /// it belongs after advance_epoch and before the frame's first acquire.
    /// One list is shared by every routine initialized in the same tick, so their GPU init work batches.
    ///
    /// Routines register themselves on first acquire or prewarm; this is what actually brings them up.
    routine_tick_result tick(routine_tick_options const& options = {});

    /// tick() until nothing is pending — the spelling a test, a tool or a loading screen wants.
    /// Unbounded by construction, so never on a frame path.
    routine_tick_result tick_until_idle();

    /// Drop every instance, releasing their cached GPU resources.
    /// Run on context shutdown, and callable early under VRAM pressure or before switching to another live context.
    void clear();

    // Pinned to its owning context: neither copyable nor movable.
    routine_registry(routine_registry const&) = delete;
    routine_registry(routine_registry&&) = delete;
    routine_registry& operator=(routine_registry const&) = delete;
    routine_registry& operator=(routine_registry&&) = delete;

private:
    friend class context;
    template <class, class>
    friend class render_routine;

    explicit routine_registry(context& ctx) : _ctx(ctx) {}

    /// Every registered instance, as shared owners, so the tick can drive them without holding the map lock.
    /// Taking a snapshot matters: a routine's initialization may register another one, which would otherwise
    /// invalidate the iteration underneath it.
    [[nodiscard]] cc::vector<std::shared_ptr<render_routine_base>> snapshot();

    context& _ctx;

    using routine_map = cc::map<impl::routine_key, std::shared_ptr<render_routine_base>>;

    /// Shared owner of the R instance for `params_hash`, created + registered on first call.
    /// The routine is heap-held, so the pointer stays valid while it is registered (the caller's weak_ptr sees eviction).
    ///
    /// `make` builds one, and is called only on a miss: it carries the parameter value, which this does not know.
    template <class R, class MakeF>
    [[nodiscard]] std::shared_ptr<R> get_or_create(u64 params_hash, MakeF&& make)
    {
        static_assert(std::is_base_of_v<render_routine_base, R>, "R must derive from render_routine_base");
        auto const key = impl::routine_key{.type = impl::routine_type_key<R>(), .params_hash = params_hash};
        // cc::mutex::lock returns by value, so hand back a (copyable) shared owner, not a reference.
        auto base = _entries.lock(
            [&](routine_map& entries) -> std::shared_ptr<render_routine_base>
            {
                auto e = entries.entry(key);
                // Deliberately not get_or_emplace: its argument would be evaluated on the hit path too,
                // allocating a routine per call only to throw it away.
                return e.exists() ? e.value() : e.emplace(make());
            });
        return std::static_pointer_cast<R>(cc::move(base));
    }

    /// Drop one instance of R, releasing its cached GPU resources (if nothing else holds them).
    /// A no-op if R was never acquired with that parameter.
    template <class R>
    void evict(u64 params_hash)
    {
        auto const key = impl::routine_key{.type = impl::routine_type_key<R>(), .params_hash = params_hash};
        _entries.lock([key](routine_map& entries) { entries.erase(key); });
    }

    /// Drop EVERY parametrization of R.
    /// The unparametrized case has exactly one, so this and evict(0) are the same thing there.
    template <class R>
    void evict_all()
    {
        void const* const type = impl::routine_type_key<R>();
        _entries.lock(
            [type](routine_map& entries)
            {
                auto doomed = cc::vector<impl::routine_key>();
                for (auto const& [key, value] : entries)
                    if (key.type == type)
                        doomed.push_back(key);
                for (auto const& key : doomed)
                    entries.erase(key);
            });
    }

    cc::mutex<routine_map> _entries;
};
