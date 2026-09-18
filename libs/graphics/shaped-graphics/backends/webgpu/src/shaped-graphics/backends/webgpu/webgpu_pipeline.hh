#pragma once

#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/webgpu/fwd.hh>
#include <shaped-graphics/backends/webgpu/webgpu_common.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>

/// WebGPU implementation of sg::compute_pipeline.
///
/// WebGPU compiles WGSL itself, so a pipeline is built from the shader's source text.
/// There is no pipeline blob to persist: cached_pipeline_data is always empty, and a supplied blob is ignored.
class sg::backend::webgpu::webgpu_compute_pipeline final : public sg::compute_pipeline
{
public:
    webgpu_compute_pipeline(sg::compute_dimensions workgroup_size,
                            webgpu_pipeline_layout_handle layout,
                            wgpu_compute_pipeline pipeline)
      : sg::compute_pipeline(workgroup_size), layout(cc::move(layout)), pipeline(cc::move(pipeline))
    {
    }

    [[nodiscard]] cc::pinned_data<byte const> cached_pipeline_data() const override { return {}; }

    webgpu_pipeline_layout_handle layout;
    wgpu_compute_pipeline pipeline;
};

/// WebGPU implementation of sg::raster_pipeline.
///
/// Geometry and tessellation stages are refused, since WebGPU has neither, and so is a wireframe fill.
/// A vertex attribute's WGSL `@location` is its index in the layout's attribute list, the same rule the vulkan backend follows.
class sg::backend::webgpu::webgpu_raster_pipeline final : public sg::raster_pipeline
{
public:
    webgpu_raster_pipeline(webgpu_pipeline_layout_handle layout, wgpu_render_pipeline pipeline)
      : layout(cc::move(layout)), pipeline(cc::move(pipeline))
    {
    }

    [[nodiscard]] cc::pinned_data<byte const> cached_pipeline_data() const override { return {}; }

    webgpu_pipeline_layout_handle layout;
    wgpu_render_pipeline pipeline;
};

namespace sg::backend::webgpu
{
/// The descriptors a raster pipeline is built from, owning everything the WebGPU descriptor points into.
/// Shared by the synchronous create and the asynchronous one, which must keep it alive until its callback.
struct raster_pipeline_build;

/// Validates `desc` and fills a build, or reports what WebGPU cannot express.
[[nodiscard]] cc::result<std::unique_ptr<raster_pipeline_build>> prepare_raster_pipeline(
    webgpu_context& ctx,
    sg::raster_pipeline_description const& desc);

/// The descriptor a prepared build describes; valid while the build lives.
[[nodiscard]] WGPURenderPipelineDescriptor const& descriptor_of(raster_pipeline_build const& build);

/// The pipeline object a finished build becomes.
[[nodiscard]] webgpu_raster_pipeline_handle finish_raster_pipeline(raster_pipeline_build& build,
                                                                   wgpu_render_pipeline pipeline);

/// A compute build's inputs, the same way.
struct compute_pipeline_build;

[[nodiscard]] cc::result<std::unique_ptr<compute_pipeline_build>> prepare_compute_pipeline(
    webgpu_context& ctx,
    sg::compute_pipeline_description const& desc);

[[nodiscard]] WGPUComputePipelineDescriptor const& descriptor_of(compute_pipeline_build const& build);

[[nodiscard]] webgpu_compute_pipeline_handle finish_compute_pipeline(compute_pipeline_build& build,
                                                                     wgpu_compute_pipeline pipeline);
} // namespace sg::backend::webgpu
