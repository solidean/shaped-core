#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/fwd.hh>

namespace sg
{
/// The raw, **uncached** factory for pipeline schemas — group layouts, pipeline layouts, and compute
/// pipelines — reached as `ctx.uncached`. Unlike ctx.persistent / ctx.transient these are not lifetime-
/// scoped GPU resources; they are immutable schemas / PSOs, and each create here builds a fresh backend
/// object every call.
///
/// Prefer `ctx.cached`: a layout or compute_pipeline is almost always worth deduplicating, and a pipeline
/// build is multi-ms — `ctx.cached` memoizes and builds pipelines asynchronously. This uncached layer is
/// the escape hatch for owning the object and its (synchronous) build directly.
///
/// Each create comes in a throwing default (`create_*`, raises a typed sg exception on failure) and a
/// fallible core (`try_create_*`, returns cc::result); see docs/error-handling.md. A thin facade
/// forwarding to the owning context's backend impl.
class context_uncached_scope
{
public:
    /// Builds a binding_group_layout (one group's schema) from a shader's reflected bindings. Sampler
    /// bindings named in `static_samplers` are baked into the group layout; the rest are dynamic (supplied
    /// per binding_group). Throws sg::pipeline_creation_exception on failure.
    [[nodiscard]] binding_group_layout_handle create_binding_group_layout(cc::span<binding const> bindings,
                                                                          cc::span<named_sampler const> static_samplers
                                                                          = {});

    /// Fallible core of create_binding_group_layout — returns an error instead of throwing.
    [[nodiscard]] cc::result<binding_group_layout_handle> try_create_binding_group_layout(
        cc::span<binding const> bindings,
        cc::span<named_sampler const> static_samplers = {});

    /// Builds a pipeline_layout (the binding interface) from an ordered list of group layouts. Throws
    /// sg::pipeline_creation_exception on failure.
    [[nodiscard]] pipeline_layout_handle create_pipeline_layout(pipeline_layout_description const& desc);

    /// Fallible core of create_pipeline_layout — returns an error instead of throwing.
    [[nodiscard]] cc::result<pipeline_layout_handle> try_create_pipeline_layout(pipeline_layout_description const& desc);

    /// Builds a compute_pipeline from a description (compute shader + pipeline layout). Blocking backend PSO
    /// creation — prefer ctx.cached.acquire_compute_pipeline for an async, deduplicated build. Throws
    /// sg::pipeline_creation_exception on failure.
    [[nodiscard]] compute_pipeline_handle create_compute_pipeline(compute_pipeline_description const& desc);

    /// Fallible core of create_compute_pipeline — returns an error instead of throwing.
    [[nodiscard]] cc::result<compute_pipeline_handle> try_create_compute_pipeline(compute_pipeline_description const& desc);

    /// Builds a raster_pipeline from a description (vertex/fragment shaders + pipeline layout + fixed-
    /// function state). Blocking backend PSO creation. Throws sg::pipeline_creation_exception on failure.
    [[nodiscard]] raster_pipeline_handle create_raster_pipeline(raster_pipeline_description const& desc);

    /// Fallible core of create_raster_pipeline — returns an error instead of throwing.
    [[nodiscard]] cc::result<raster_pipeline_handle> try_create_raster_pipeline(raster_pipeline_description const& desc);

    /// Builds a raytracing_pipeline (a DXR state object) from a description. Blocking backend state-object
    /// creation — prefer ctx.cached.acquire_raytracing_pipeline for an async, deduplicated build. Throws
    /// sg::pipeline_creation_exception on failure.
    [[nodiscard]] raytracing_pipeline_handle create_raytracing_pipeline(raytracing_pipeline_description const& desc);

    /// Fallible core of create_raytracing_pipeline — returns an error instead of throwing.
    [[nodiscard]] cc::result<raytracing_pipeline_handle> try_create_raytracing_pipeline(
        raytracing_pipeline_description const& desc);

    /// Builds a raytracing_shader_table over a pipeline (referencing its shader identifiers). Persistent and
    /// uncached — it is tied to one pipeline. Throws sg::pipeline_creation_exception on failure.
    [[nodiscard]] raytracing_shader_table_handle create_raytracing_shader_table(
        raytracing_shader_table_description const& desc);

    /// Fallible core of create_raytracing_shader_table — returns an error instead of throwing.
    [[nodiscard]] cc::result<raytracing_shader_table_handle> try_create_raytracing_shader_table(
        raytracing_shader_table_description const& desc);

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
