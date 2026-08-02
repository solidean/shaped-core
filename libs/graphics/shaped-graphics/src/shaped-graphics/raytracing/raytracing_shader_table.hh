#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/fwd.hh>

namespace sg
{
/// Selects which of a raytracing_pipeline's registered shaders go into a shader table, and in what order.
/// The `add_*` helpers take a pipeline handle (registration order) and return the table index (dispatch
/// order) — what HLSL TraceRay's shader-record offsets address.
struct raytracing_shader_table_description
{
    raytracing_pipeline_handle pipeline;

    cc::vector<raygen_shader_handle> raygen;
    cc::vector<miss_shader_handle> miss;
    cc::vector<hit_shader_handle> hit;
    cc::vector<callable_shader_handle> callable;

    /// Appends a raygen record; returns its table index.
    [[nodiscard]] raygen_index add_raygen_shader(raygen_shader_handle handle);
    /// Appends a miss record; returns its table index.
    [[nodiscard]] miss_index add_miss_shader(miss_shader_handle handle);
    /// Appends a hit-group record; returns its table index.
    [[nodiscard]] hit_index add_hit_shader(hit_shader_handle handle);
    /// Appends a callable record; returns its table index.
    [[nodiscard]] callable_index add_callable_shader(callable_shader_handle handle);
};

/// A shader table (not "SBT"): the GPU-resident table of shader records dispatch_rays reads to pick the
/// raygen / miss / hit-group / callable shaders. Each record holds only a 32-byte shader identifier (no
/// local root arguments) — bind resources through the pipeline's global root signature instead. Persistent
/// and tied to one raytracing_pipeline; held via raytracing_shader_table_handle.
class raytracing_shader_table
{
public:
    virtual ~raytracing_shader_table();

    [[nodiscard]] raytracing_pipeline_handle const& pipeline() const { return _pipeline; }

protected:
    explicit raytracing_shader_table(raytracing_pipeline_handle pipeline) : _pipeline(cc::move(pipeline)) {}

    raytracing_pipeline_handle _pipeline;
};
} // namespace sg
