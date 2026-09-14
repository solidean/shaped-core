#pragma once

#include <clean-core/container/span.hh>
#include <shaped-graphics/binding/binding_group.hh> // sg::declared_binding_group, and the sampler merge below
#include <shaped-graphics/fwd.hh>

/// Cache facade for a context's built-in pipeline_cache, reached as `ctx.cached`.
/// `acquire` is the get-or-create verb: identical arguments return the already-built handle / async node instead of rebuilding.
/// Layouts are cached synchronously; compute, raster and raytracing pipelines build asynchronously.
///
/// Use cache() to install extra tiers (e.g. a disk-backed provider) or run bookkeeping.
/// A context installs default in-memory tiers at construction, so dedup works without any setup.
class sg::context_cached_scope
{
public:
    /// The cached binding_group_layout for these bindings + static samplers, created on a miss.
    /// The static samplers are part of the cache key — they are baked into the group layout.
    /// Throws sg::pipeline_creation_exception on a creation failure, or sg::device_lost_exception if the device was lost.
    [[nodiscard]] binding_group_layout_handle acquire_binding_group_layout(cc::span<binding const> bindings,
                                                                           cc::span<named_sampler const> static_samplers
                                                                           = {});

    /// The cached layout the generated group `G` declares — its bindings and the samplers it marked `static`.
    /// Constant rather than reflected: the same parse that wrote the shader's addresses produced this table.
    template <declared_binding_group G>
    [[nodiscard]] binding_group_layout_handle acquire_binding_group_layout()
    {
        return acquire_binding_group_layout(G::declared_bindings(), G::declared_samplers());
    }

    /// The same, plus static samplers for the ones `G` left undeclared — what a runtime-generated permutation
    /// supplies.
    /// A declared sampler wins; see sg::impl::merge_declared_samplers for why supplying one is a mistake.
    template <declared_binding_group G>
    [[nodiscard]] binding_group_layout_handle acquire_binding_group_layout(cc::span<named_sampler const> samplers)
    {
        return acquire_binding_group_layout(G::declared_bindings(),
                                            impl::merge_declared_samplers(G::declared_samplers(), samplers));
    }

    /// The cached pipeline_layout for these ordered group layouts, created on a miss.
    /// The key is the group layouts' structural identity, so two separately created but identical ones still dedup.
    /// Throws sg::pipeline_creation_exception on a creation failure, or sg::device_lost_exception if the device was lost.
    [[nodiscard]] pipeline_layout_handle acquire_pipeline_layout(pipeline_layout_description const& desc);

    /// The async compute_pipeline for `desc`, built on a miss.
    /// Drive with cc::async_blocking_get, or poll .is_ready() / .try_value(); a build failure surfaces as an async error.
    /// Acquire the pipeline layout through this scope too for full dedup (see pipeline_cache).
    [[nodiscard]] async_compute_pipeline acquire_compute_pipeline(compute_pipeline_description const& desc);

    /// The async raster_pipeline for `desc`, built on a miss.
    /// The key covers the shaders, the vertex-input layout and every fixed-function state.
    /// NOT `desc.cached_pipeline` though — that blob is a best-effort build accelerator, not part of the pipeline's identity.
    /// Drive with cc::async_blocking_get, or poll .is_ready() / .try_value(); a build failure surfaces as an async error.
    /// Acquire the pipeline layout through this scope too for full dedup (see pipeline_cache).
    [[nodiscard]] async_raster_pipeline acquire_raster_pipeline(raster_pipeline_description const& desc);

    /// The async raytracing_pipeline for `desc`, built on a miss.
    /// Drive with cc::async_blocking_get, or poll .is_ready() / .try_value(); a build failure surfaces as an async error.
    /// Acquire the pipeline layout through this scope too for full dedup (see pipeline_cache).
    [[nodiscard]] async_raytracing_pipeline acquire_raytracing_pipeline(raytracing_pipeline_description const& desc);

    /// The underlying cache — install providers (add_*_provider) or run apply_bookkeeping through it.
    [[nodiscard]] pipeline_cache& cache();

    // Pinned to its owning context: neither copyable nor movable.
    context_cached_scope(context_cached_scope const&) = delete;
    context_cached_scope(context_cached_scope&&) = delete;
    context_cached_scope& operator=(context_cached_scope const&) = delete;
    context_cached_scope& operator=(context_cached_scope&&) = delete;

private:
    friend class context;
    explicit context_cached_scope(context& ctx) : _ctx(ctx) {}

    context& _ctx;
};
