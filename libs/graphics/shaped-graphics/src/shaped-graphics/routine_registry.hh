#pragma once

#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/vector.hh>
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

/// Per-context registry of render-routine instances, reached as `ctx.routines`. A routine is a
/// per-context singleton: the first acquire of a given type creates and registers it here (lazy
/// self-registration — no explicit registration, no by-name lookup), and it lives until the context is
/// shut down or it is explicitly evicted. Routines are normally reached through sg::render_routine's
/// static acquire(cmd), which calls get_or_create under the hood; you touch this directly only to
/// prewarm, evict, or clear.
///
/// A thin per-context sub-object like ctx.cached: it holds a back-reference to its context and is
/// created and destroyed with it. Map access is guarded, so acquire is safe from parallel command-list
/// recording; initializing a *single* routine concurrently from two threads is not yet synchronized (a
/// follow-up for when parallel init lands). Do not clear()/evict() a registry while another thread is
/// still recording against the same context.
class routine_registry
{
public:
    /// The instance for R (created + registered on first call), keyed by type. Returns a stable
    /// reference — the routine is heap-held and does not move when the registry grows.
    template <class R>
    [[nodiscard]] R& get_or_create()
    {
        static_assert(std::is_base_of_v<render_routine_base, R>, "R must derive from render_routine_base");
        void const* const key = impl::routine_type_key<R>();
        // cc::mutex::lock returns by value, so hand back a (copyable) pointer into the stable heap object.
        R* const routine = _entries.lock(
            [key](cc::vector<entry>& entries) -> R*
            {
                for (auto const& e : entries)
                    if (e.key == key)
                        return static_cast<R*>(e.routine.get());
                auto created = std::make_shared<R>();
                R* const raw = created.get();
                entries.push_back({key, cc::move(created)});
                return raw;
            });
        return *routine;
    }

    /// Prewarm the given routine types: create each + run init_once + init_declare, so all their async
    /// pipeline compiles are in flight at once (they build in parallel on the installed async pool).
    /// Materialize still happens lazily on the first acquire(cmd). The declares run sequentially for now;
    /// the async *compiles* are what parallelize.
    template <class... R>
    void prewarm()
    {
        (get_or_create<R>().ensure_initialized_no_materialize(_ctx), ...);
    }

    /// Drop the instance for R, releasing its cached GPU resources (if nothing else holds them). A no-op
    /// if R was never acquired.
    template <class R>
    void evict()
    {
        void const* const key = impl::routine_type_key<R>();
        _entries.lock([key](cc::vector<entry>& entries)
                      { entries.remove_first_where([key](entry const& e) { return e.key == key; }); });
    }

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
    explicit routine_registry(context& ctx) : _ctx(ctx) {}

    struct entry
    {
        void const* key;
        std::shared_ptr<render_routine_base> routine;
    };

    context& _ctx;
    cc::mutex<cc::vector<entry>> _entries;
};
} // namespace sg
