#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/binding/binding_group.hh>   // sg::declared_binding_set, and the sampler merge below
#include <shaped-graphics/binding/pipeline_layout.hh> // acquire_pipeline_layout<Ts...> fills a description
#include <shaped-graphics/fwd.hh>

#include <concepts>

namespace sg
{
/// Edits a raster pipeline's description last, after everything its source stated.
using raster_pipeline_customize = cc::unique_function<void(raster_pipeline_description&)>;

/// Something that describes a raster pipeline for a context, given the parts it leaves to the caller as `S::open`.
/// A generated SGL `pipeline` is one, which is what lets `ctx.cached.acquire_raster_pipeline(shaders::cube.pipeline, {.color = f})` build it.
/// sg knows nothing else about it: the description is what is acquired, so the cache key is the description's.
template <class S>
concept raster_pipeline_source = requires(S const& source, context& ctx, typename S::open const& parts) {
    {
        source.description(ctx, parts, raster_pipeline_customize())
    } -> std::same_as<cc::shared_async<raster_pipeline_description>>;
};
} // namespace sg

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
    template <declared_binding_set G>
    [[nodiscard]] binding_group_layout_handle acquire_binding_group_layout()
    {
        return acquire_binding_group_layout(G::declared_bindings(), G::declared_samplers());
    }

    /// The same, plus static samplers for the ones `G` left undeclared — what a runtime-generated permutation
    /// supplies.
    /// A declared sampler wins; see sg::impl::merge_declared_samplers for why supplying one is a mistake.
    template <declared_binding_set G>
    [[nodiscard]] binding_group_layout_handle acquire_binding_group_layout(cc::span<named_sampler const> samplers)
    {
        return acquire_binding_group_layout(G::declared_bindings(),
                                            impl::merge_declared_samplers(G::declared_samplers(), samplers));
    }

    /// The cached pipeline_layout for these ordered group layouts, created on a miss.
    /// The key is the group layouts' structural identity, so two separately created but identical ones still dedup.
    /// Throws sg::pipeline_creation_exception on a creation failure, or sg::device_lost_exception if the device was lost.
    [[nodiscard]] pipeline_layout_handle acquire_pipeline_layout(pipeline_layout_description const& desc);

    /// The pipeline layout a list of generated types states, with no reflected binding in it.
    /// Each binding set is the group at its position among the sets, and at most one inline-constants block, wherever
    /// it stands, is the layout's `inline_constants` — which is an SGL entry point's binding list, spelled in C++.
    /// `static_samplers` are the ones a pipeline needs beyond what its groups declare.
    template <class... Ts>
        requires((declared_binding_set<Ts> || declared_inline_constants<Ts>) && ...)
    [[nodiscard]] pipeline_layout_handle acquire_pipeline_layout(cc::span<bound_sampler const> static_samplers = {})
    {
        static_assert((int(declared_inline_constants<Ts>) + ... + 0) <= 1, "a pipeline layout has one inline block");
        auto desc = pipeline_layout_description();
        (add_to_layout<Ts>(desc), ...);
        desc.static_samplers.push_back_range(static_samplers);
        return acquire_pipeline_layout(desc);
    }

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

    /// The same, once `desc` is known: what a pipeline source's description is acquired through.
    /// A failed description is the returned async's failure.
    [[nodiscard]] async_raster_pipeline acquire_raster_pipeline(cc::shared_async<raster_pipeline_description> desc);

    /// The pipeline `source` describes, with `parts` stated and `customize` applied to the description last.
    /// `parts` is not deduced, so `{.color = f}` names the source's own open parts.
    template <raster_pipeline_source S>
    [[nodiscard]] async_raster_pipeline acquire_raster_pipeline(S const& source,
                                                                typename S::open const& parts = {},
                                                                raster_pipeline_customize customize = {})
    {
        return acquire_raster_pipeline(source.description(_ctx, parts, cc::move(customize)));
    }

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
    template <class T>
    void add_to_layout(pipeline_layout_description& desc)
    {
        if constexpr (declared_inline_constants<T>)
            desc.inline_constants = T::inline_binding();
        else
            desc.groups.push_back(acquire_binding_group_layout<T>());
    }

    friend class context;
    explicit context_cached_scope(context& ctx) : _ctx(ctx) {}

    context& _ctx;
};
