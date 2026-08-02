#pragma once

#include <clean-core/container/span.hh>
#include <shaped-graphics/fwd.hh>

namespace sg
{
/// Cache facade for a context's built-in pipeline_cache, reached as `ctx.cached`. `acquire` is the
/// get-or-create verb: identical arguments return the already-built handle / async node instead of
/// rebuilding. Layouts are cached synchronously; compute pipelines build asynchronously.
///
/// A thin facade over its owning context — it forwards to the context's built-in pipeline_cache. Use
/// cache() to install extra tiers (e.g. a disk-backed provider) or run bookkeeping.
class context_cached_scope
{
public:
    /// The cached binding_group_layout for these bindings + static samplers, created on a miss. The static
    /// samplers are part of the cache key (they are baked into the group layout). Throws
    /// sg::pipeline_creation_exception on a creation failure.
    [[nodiscard]] binding_group_layout_handle acquire_binding_group_layout(cc::span<binding const> bindings,
                                                                           cc::span<named_sampler const> static_samplers
                                                                           = {});

    /// The cached pipeline_layout for these ordered group layouts, created on a miss. Acquire the group
    /// layouts through this scope too for full dedup (the key is their handle identity). Throws
    /// sg::pipeline_creation_exception on a creation failure.
    [[nodiscard]] pipeline_layout_handle acquire_pipeline_layout(pipeline_layout_description const& desc);

    /// The async compute_pipeline for `desc`, built on a miss. Drive with cc::async_blocking_get_singlethreaded, or
    /// poll .is_ready() / .try_value(); a build failure surfaces as an async error. Acquire the pipeline
    /// layout through this scope too for full dedup (see pipeline_cache).
    [[nodiscard]] async_compute_pipeline acquire_compute_pipeline(compute_pipeline_description const& desc);

    /// The async raytracing_pipeline for `desc`, built on a miss. Drive with cc::async_blocking_get_singlethreaded, or
    /// poll .is_ready() / .try_value(); a build failure surfaces as an async error. Acquire the pipeline
    /// layout through this scope too for full dedup (see pipeline_cache).
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
} // namespace sg
