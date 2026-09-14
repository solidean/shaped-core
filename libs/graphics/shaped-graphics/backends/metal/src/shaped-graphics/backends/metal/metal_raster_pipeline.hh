#pragma once

#include <clean-core/container/pinned_data.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>

/// Metal implementation of sg::raster_pipeline.
///
/// **Three objects where dx12 and vulkan each have one.**
/// MTL4 splits what a D3D12 PSO folds together: the render pipeline state carries the shaders, the vertex layout, the
/// colour attachments and their blending; the depth and stencil test live in a separate `MTLDepthStencilState` bound on
/// the encoder; and the cull mode, fill mode, winding and depth bias are encoder calls rather than pipeline state at
/// all.
/// So this holds the first two and replays the third at bind time.
///
/// **Attachment formats are not pipeline state here.**
/// MTL4's render pipeline descriptor has no depth or stencil attachment format, and its colour formats are only what
/// the blend descriptor needs — the render pass establishes the rest at encode time.
/// sg's `depth_stencil_format` is therefore carried for validation rather than for building.
class sg::backend::metal::metal_raster_pipeline final : public sg::raster_pipeline
{
public:
    metal_raster_pipeline(metal_context& ctx,
                          MTL::RenderPipelineState* state,
                          MTL::DepthStencilState* depth_stencil,
                          sg::rasterization_state rasterization,
                          sg::primitive_topology topology,
                          sg::pipeline_layout_handle layout)
      : _ctx(ctx),
        _state(state),
        _depth_stencil(depth_stencil),
        _rasterization(rasterization),
        _topology(topology),
        _layout(cc::move(layout))
    {
    }

    ~metal_raster_pipeline() override { release_backend_objects(); }

    void release_backend_objects() override;

    [[nodiscard]] MTL::RenderPipelineState* state() const { return _state; }
    [[nodiscard]] MTL::DepthStencilState* depth_stencil_state() const { return _depth_stencil; }
    [[nodiscard]] sg::rasterization_state const& rasterization() const { return _rasterization; }
    [[nodiscard]] sg::primitive_topology topology() const { return _topology; }

    /// Metal has no serialized-PSO blob on this path; see metal_compute_pipeline for why.
    [[nodiscard]] cc::pinned_data<byte const> cached_pipeline_data() const override { return {}; }

private:
    metal_context& _ctx;
    MTL::RenderPipelineState* _state = nullptr;
    MTL::DepthStencilState* _depth_stencil = nullptr;
    sg::rasterization_state _rasterization;
    sg::primitive_topology _topology = sg::primitive_topology::triangle_list;
    sg::pipeline_layout_handle _layout;
};
