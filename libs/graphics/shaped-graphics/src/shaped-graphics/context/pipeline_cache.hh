#pragma once

#include <blob-cache/keys.hh> // bcache::cache_key, returned by value below
#include <clean-core/common/hash128.hh>
#include <clean-core/container/key_value_cache.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/fwd.hh>

/// Backend-agnostic cache for group layouts, pipeline layouts, and compute + raytracing pipelines.
/// Keyed by a cc::hash128 over the logical creation arguments, so it is independent of any backend handle identity.
/// A second acquire with the same arguments returns the already-created handle / async node instead of rebuilding.
///
/// Layouts are cheap and cached synchronously.
/// Pipelines are built asynchronously as a cc::async routed to the installed default pool, since PSO creation is multi-ms.
/// Raster pipelines are not cached yet.
///
/// A context owns one of these, reached via ctx.cached; the acquire_* methods take the owning context so the cache stays a plain member.
///
/// Threading: the async pipeline build calls a backend create from a pool worker, which is only safe where the backend permits concurrent pipeline creation (dx12 device creates are free-threaded).
/// With a single_threaded thread_model, install no pool and drive the node inline on the main thread via cc::async_blocking_get.

class sg::pipeline_cache
{
public:
    // provider configuration
public:
    /// Adds a tier to the binding-group-layout cache (front tiers consulted first; see cc::key_value_cache).
    void add_binding_group_layout_provider(
        std::shared_ptr<cc::key_value_provider<cc::hash128, binding_group_layout_handle>> provider);

    /// Adds a tier to the pipeline-layout cache.
    void add_pipeline_layout_provider(std::shared_ptr<cc::key_value_provider<cc::hash128, pipeline_layout_handle>> provider);

    /// Adds a tier to the compute-pipeline cache.
    void add_compute_pipeline_provider(std::shared_ptr<cc::key_value_provider<cc::hash128, async_compute_pipeline>> provider);

    /// Adds a tier to the raytracing-pipeline cache.
    void add_raytracing_pipeline_provider(
        std::shared_ptr<cc::key_value_provider<cc::hash128, async_raytracing_pipeline>> provider);

    /// Convenience: give every cache a default in-memory tier (up to max_entries each).
    void add_default_in_memory_providers(isize max_entries = 4096);

    /// The persistent tier a pipeline build consults: serialized PSO blobs surviving across runs.
    /// Defaults to bcache::default_cache(), opened the first time a pipeline misses in memory; nullptr turns it off.
    ///
    /// The build parks on the store, so it needs somewhere to resume: with no pool installed and no worker scope
    /// active, the tier is skipped and the pipeline is built the plain way.
    /// Without threads, drive it with ctx.pump() — the store advances only when pumped.
    void set_blob_cache(bcache::blob_cache* cache);

    // acquire (get-or-create)
public:
    /// The cached binding_group_layout for these bindings + static samplers, created via ctx.uncached on a miss.
    /// Identical (bindings, static_samplers) map to one shared handle — the static samplers are part of the key, since they are baked into the group layout.
    /// Throws sg::pipeline_creation_exception on failure, or sg::device_lost_exception if the device was lost.
    [[nodiscard]] binding_group_layout_handle acquire_binding_group_layout(context& ctx,
                                                                           cc::span<binding const> bindings,
                                                                           cc::span<named_sampler const> static_samplers
                                                                           = {});

    /// The cached pipeline_layout for these ordered group layouts, created via ctx.uncached on a miss.
    /// The key is the group layouts' structural identity, so two separately created but identical group layouts still dedup.
    /// Throws sg::pipeline_creation_exception on failure, or sg::device_lost_exception if the device was lost.
    [[nodiscard]] pipeline_layout_handle acquire_pipeline_layout(context& ctx, pipeline_layout_description const& desc);

    /// The async compute_pipeline for `desc`, built via ctx.uncached on a miss.
    /// The key combines the shader's content with the pipeline_layout's structural identity.
    /// Drive with cc::async_blocking_get, or poll .is_ready() / .try_value(); a build failure surfaces as an async error.
    [[nodiscard]] async_compute_pipeline acquire_compute_pipeline(context& ctx, compute_pipeline_description const& desc);

    /// The async raytracing_pipeline for `desc`, built via ctx.uncached on a miss.
    /// The key combines every shader's content with the pipeline_layout's structural identity and the pipeline limits.
    /// Drive with cc::async_blocking_get, or poll .is_ready() / .try_value(); a build failure surfaces as an async error.

    [[nodiscard]] async_raytracing_pipeline acquire_raytracing_pipeline(context& ctx,
                                                                        raytracing_pipeline_description const& desc);

    // maintenance
public:
    /// Runs bookkeeping (e.g. in-memory eviction) on all caches.
    void apply_bookkeeping();

    /// Advances the persistent tier where it has no thread of its own; true if there may be more work.
    /// Reached through ctx.pump(); never opens a store that was not already in use.
    bool pump();

private:
    [[nodiscard]] cc::hash128 compute_binding_group_layout_key(cc::span<binding const> bindings,
                                                               cc::span<named_sampler const> static_samplers) const;
    [[nodiscard]] cc::hash128 compute_pipeline_layout_key(pipeline_layout_description const& desc) const;
    [[nodiscard]] cc::hash128 compute_compute_pipeline_key(compute_pipeline_description const& desc) const;
    [[nodiscard]] cc::hash128 compute_raytracing_pipeline_key(raytracing_pipeline_description const& desc) const;

    /// The persistent-cache key for a pipeline whose in-memory key is `pipeline_key`.
    ///
    /// Folds in the adapter and driver on top, because a serialized PSO blob is only valid for the pair that wrote it.
    /// Under-keying is cheap by construction — a rejected blob costs one failed create and nothing else — so this
    /// deliberately does not try to capture the whole driver state.
    [[nodiscard]] bcache::cache_key persistent_key(context& ctx, cc::hash128 pipeline_key, cc::string_view kind) const;

    /// The persistent tier, resolved on first use: nullopt means "not chosen yet", a null pointer means OFF.
    /// Lazy so that merely creating a context never opens a cache file.
    [[nodiscard]] bcache::blob_cache* resolve_blob_cache();

    cc::optional<bcache::blob_cache*> _blob_cache;

    cc::key_value_cache<cc::hash128, binding_group_layout_handle> _binding_group_layout_cache;
    cc::key_value_cache<cc::hash128, pipeline_layout_handle> _pipeline_layout_cache;
    cc::key_value_cache<cc::hash128, async_compute_pipeline> _compute_cache;
    cc::key_value_cache<cc::hash128, async_raytracing_pipeline> _raytracing_cache;
    // TODO: a raster_pipeline cache tier.
};
