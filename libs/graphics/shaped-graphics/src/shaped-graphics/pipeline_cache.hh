#pragma once

#include <clean-core/common/hash128.hh>
#include <clean-core/container/key_value_cache.hh>
#include <clean-core/container/span.hh>
#include <shaped-graphics/fwd.hh>

#include <memory>

/// Backend-agnostic cache for binding layouts and compute pipelines, keyed by a cc::hash128 over the
/// logical creation arguments (so it is independent of any backend handle identity). A second acquire
/// with the same arguments returns the already-created handle / async node instead of rebuilding.
///
/// Layouts are cheap and are cached synchronously; compute pipelines are built asynchronously
/// (multi-ms PSO creation) as a cc::async routed to the installed default pool. Graphics / raytracing
/// pipelines are future work — sg has no such types yet.
///
/// A context owns one of these (reached via ctx.cached); the acquire_* methods take the owning context
/// so the cache stays a plain member.
///
/// Threading: the async pipeline build calls a backend create from a pool worker, which is only safe
/// where the backend permits concurrent pipeline creation (dx12 device creates are free-threaded). With
/// a single_threaded thread_model, install no pool and drive the node inline on the main thread via
/// cc::async_blocking_get.

namespace sg
{
class pipeline_cache
{
public:
    // provider configuration
public:
    /// Adds a tier to the binding-layout cache (front tiers consulted first; see cc::key_value_cache).
    void add_binding_layout_provider(std::shared_ptr<cc::key_value_provider<cc::hash128, binding_layout_handle>> provider);

    /// Adds a tier to the compute-pipeline cache.
    void add_compute_pipeline_provider(std::shared_ptr<cc::key_value_provider<cc::hash128, async_compute_pipeline>> provider);

    /// Convenience: give every cache a default in-memory tier (up to max_entries each).
    void add_default_in_memory_providers(cc::isize max_entries = 4096);

    // acquire (get-or-create)
public:
    /// The cached binding_layout for these bindings, created via ctx.persistent on a miss. Identical
    /// bindings map to one shared handle. Throws sg::pipeline_creation_exception on a creation failure.
    [[nodiscard]] binding_layout_handle acquire_binding_layout(context& ctx, cc::span<binding const> bindings);

    /// The async compute_pipeline for `desc`, built via ctx.persistent on a miss. The key combines the
    /// shader's content with the layout handle's identity, so acquire the layout THROUGH the cache to
    /// get full dedup (identical layouts then share one handle). Drive with cc::async_blocking_get, or
    /// poll .is_ready() / .try_value(); a build failure surfaces as an async error.
    [[nodiscard]] async_compute_pipeline acquire_compute_pipeline(context& ctx, compute_pipeline_description const& desc);

    // maintenance
public:
    /// Runs bookkeeping (e.g. in-memory eviction) on all caches.
    void apply_bookkeeping();

private:
    [[nodiscard]] cc::hash128 compute_binding_layout_key(cc::span<binding const> bindings) const;
    [[nodiscard]] cc::hash128 compute_compute_pipeline_key(compute_pipeline_description const& desc) const;

    cc::key_value_cache<cc::hash128, binding_layout_handle> _layout_cache;
    cc::key_value_cache<cc::hash128, async_compute_pipeline> _compute_cache;
    // TODO: _graphics_cache / _raytracing_cache once those pipeline types exist in sg.
};
} // namespace sg
