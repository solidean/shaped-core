#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/fwd.hh>

namespace sg
{
/// The raw, **uncached** factory for pipeline schemas — binding layouts and compute pipelines — reached
/// as `ctx.uncached`. Unlike ctx.persistent / ctx.transient these are not lifetime-scoped GPU resources;
/// they are immutable schemas / PSOs, and each create here builds a fresh backend object every call.
///
/// Prefer `ctx.cached`: a binding_layout or compute_pipeline is almost always worth deduplicating, and a
/// pipeline build is multi-ms — `ctx.cached` memoizes and builds pipelines asynchronously. This uncached
/// layer is the escape hatch for owning the object and its (synchronous) build directly.
///
/// Each create comes in a throwing default (`create_*`, raises a typed sg exception on failure) and a
/// fallible core (`try_create_*`, returns cc::result); see docs/error-handling.md. A thin facade
/// forwarding to the owning context's backend impl.
class context_uncached_scope
{
public:
    /// Builds a binding_layout (the bindable-set schema) from a shader's reflected bindings. Sampler
    /// bindings named in `static_samplers` are baked into the layout; the rest are dynamic (supplied per
    /// binding_group). Throws sg::pipeline_creation_exception on failure.
    [[nodiscard]] binding_layout_handle create_binding_layout(cc::span<binding const> bindings,
                                                              cc::span<named_sampler const> static_samplers = {});

    /// Fallible core of create_binding_layout — returns an error instead of throwing.
    [[nodiscard]] cc::result<binding_layout_handle> try_create_binding_layout(cc::span<binding const> bindings,
                                                                              cc::span<named_sampler const> static_samplers
                                                                              = {});

    /// Builds a compute_pipeline from a description (compute shader + layout). Blocking backend PSO
    /// creation — prefer ctx.cached.acquire_compute_pipeline for an async, deduplicated build. Throws
    /// sg::pipeline_creation_exception on failure.
    [[nodiscard]] compute_pipeline_handle create_compute_pipeline(compute_pipeline_description const& desc);

    /// Fallible core of create_compute_pipeline — returns an error instead of throwing.
    [[nodiscard]] cc::result<compute_pipeline_handle> try_create_compute_pipeline(compute_pipeline_description const& desc);

    // Pinned to its owning context: neither copyable nor movable.
    context_uncached_scope(context_uncached_scope const&) = delete;
    context_uncached_scope(context_uncached_scope&&) = delete;
    context_uncached_scope& operator=(context_uncached_scope const&) = delete;
    context_uncached_scope& operator=(context_uncached_scope&&) = delete;

private:
    friend class context;
    explicit context_uncached_scope(context& ctx) : _ctx(ctx) {}

    context& _ctx;
};
} // namespace sg
