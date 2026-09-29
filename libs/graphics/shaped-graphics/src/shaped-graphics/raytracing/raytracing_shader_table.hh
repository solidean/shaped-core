#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/fwd.hh>

/// Selects which of a raytracing_pipeline's registered shaders go into a shader table, and in what order.
/// The `add_*` helpers take a pipeline handle, in registration order, and return the table index, in dispatch order.
/// That index is what HLSL TraceRay's shader-record offsets address.
struct sg::raytracing_shader_table_description
{
    raytracing_pipeline_handle pipeline;

    cc::vector<raygen_shader_handle> raygen;
    cc::vector<miss_shader_handle> miss;
    cc::vector<hit_shader_handle> hit;
    cc::vector<callable_shader_handle> callable;

    /// How many ray types trace through this table, which is the length of every hit_row.
    /// A trace of ray type r uses r as its ray contribution and `ray_count` as its geometry multiplier, so geometry g of an instance reads record `hit_group_offset + g * ray_count + r`.
    /// Must be >= 1.
    /// Metal builds one intersection function table per ray type from it; dx12 and vulkan take both terms per trace and need nothing here.
    int ray_count = 1;

    /// Appends a raygen record; returns its table index.
    [[nodiscard]] raygen_index add_raygen_shader(raygen_shader_handle handle);
    /// Appends a miss record; returns its table index.
    [[nodiscard]] miss_index add_miss_shader(miss_shader_handle handle);
    /// Appends a hit-group record; returns its table index.
    [[nodiscard]] hit_index add_hit_shader(hit_shader_handle handle);
    /// Appends one hit record per ray type, consecutively, and returns the row, whose value is its first record's index.
    /// `per_ray_type.size()` must equal `ray_count`.
    /// A row may name one hit group several times, and one hit group may appear in several rows.
    /// Rows and add_hit_shader both append to the same records, so they mix freely.
    [[nodiscard]] hit_row add_hit_row(cc::span<hit_shader_handle const> per_ray_type);
    /// Appends a callable record; returns its table index.
    [[nodiscard]] callable_index add_callable_shader(callable_shader_handle handle);
};

/// A shader table (not "SBT"): the GPU-resident table of shader records dispatch_rays reads to pick the raygen / miss / hit-group / callable shaders.
/// Each record holds only a 32-byte shader identifier, with no local root arguments, so resources bind through the pipeline's global root signature instead.
/// Persistent and tied to one raytracing_pipeline; held via raytracing_shader_table_handle.
class sg::raytracing_shader_table : public std::enable_shared_from_this<raytracing_shader_table>
{
public:
    virtual ~raytracing_shader_table();

    [[nodiscard]] raytracing_pipeline_handle const& pipeline() const { return _pipeline; }

    /// The ray count the description gave, which every hit_row of this table spans.
    [[nodiscard]] int ray_count() const { return _ray_count; }

    /// The hit group behind each hit record, by record index.
    [[nodiscard]] cc::span<hit_shader_handle const> hit_records() const { return _hit_records; }

    /// The value an instance tracing through `row` takes as its tlas_instance::hit_group_offset.
    /// `row` must come from this table's description.
    [[nodiscard]] u32 offset_of(hit_row row) const;

protected:
    /// Keeps what every backend's table shares: the pipeline, the hit record's groups and the ray count, which must be >= 1.
    explicit raytracing_shader_table(raytracing_shader_table_description const& desc);

    raytracing_pipeline_handle _pipeline;

private:
    cc::vector<hit_shader_handle> _hit_records;
    int _ray_count = 1;
};
