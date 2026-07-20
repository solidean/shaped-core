#pragma once

#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/map.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/fwd.hh> // sg::context
#include <shaped-graphics/render_routine_base.hh>

#include <memory>
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
} // namespace impl

/// Per-context storage of render-routine instances, reached as `ctx.routines`. A routine is a
/// per-context singleton: the first acquire of a given type creates and registers it here (lazy
/// self-registration — no explicit registration, no by-name lookup), and it lives until the context is
/// shut down or it is explicitly evicted.
///
/// Everything type-keyed is private and driven through sg::render_routine's statics
/// (`R::acquire(cmd)` / `R::prewarm(ctx)` / `R::evict(ctx)`); the only public operation here is
/// clear(). A thin per-context sub-object like ctx.cached, created and destroyed with its context.
///
/// Map access is guarded, so acquire is safe from parallel command-list recording; initializing a
/// *single* routine concurrently from two threads is not yet synchronized (a follow-up for when
/// parallel init lands). Do not clear()/evict() a registry while another thread is still recording
/// against the same context.
class routine_registry
{
public:
    /// Drop every instance, releasing their cached GPU resources. Run on context shutdown, and callable
    /// early under VRAM pressure or before switching to another live context.
    void clear();

    // Pinned to its owning context: neither copyable nor movable.
    routine_registry(routine_registry const&) = delete;
    routine_registry(routine_registry&&) = delete;
    routine_registry& operator=(routine_registry const&) = delete;
    routine_registry& operator=(routine_registry&&) = delete;

private:
    friend class context;
    template <class>
    friend class render_routine;

    routine_registry() = default;

    using routine_map = cc::map<void const*, std::shared_ptr<render_routine_base>>;

    /// Shared owner of R's instance, created + registered on first call. The routine is heap-held, so
    /// the pointer stays valid while it is registered (the caller's weak_ptr sees eviction).
    template <class R>
    [[nodiscard]] std::shared_ptr<R> get_or_create()
    {
        static_assert(std::is_base_of_v<render_routine_base, R>, "R must derive from render_routine_base");
        void const* const key = impl::routine_type_key<R>();
        // cc::mutex::lock returns by value, so hand back a (copyable) shared owner, not a reference.
        auto base = _entries.lock(
            [key](routine_map& entries) -> std::shared_ptr<render_routine_base>
            {
                auto e = entries.entry(key);
                // Deliberately not get_or_emplace: its argument would be evaluated on the hit path too,
                // allocating a routine per call only to throw it away.
                return e.exists() ? e.value() : e.emplace(std::make_shared<R>());
            });
        return std::static_pointer_cast<R>(cc::move(base));
    }

    /// Drop the instance for R, releasing its cached GPU resources (if nothing else holds them). A no-op
    /// if R was never acquired.
    template <class R>
    void evict()
    {
        void const* const key = impl::routine_type_key<R>();
        _entries.lock([key](routine_map& entries) { entries.erase(key); });
    }

    cc::mutex<routine_map> _entries;
};
} // namespace sg
