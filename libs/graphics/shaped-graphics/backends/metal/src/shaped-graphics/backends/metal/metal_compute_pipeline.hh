#pragma once

#include <clean-core/container/pinned_data.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/fwd.hh>

/// Metal implementation of sg::compute_pipeline.
///
/// Holds the pipeline state and the layout it was built against — the layout because Metal binds argument buffers
/// positionally, so the dispatch has to know which group goes at which argument-table index, and only the layout says.
class sg::backend::metal::metal_compute_pipeline final : public sg::compute_pipeline
{
public:
    metal_compute_pipeline(metal_context& ctx,
                           sg::compute_dimensions workgroup_size,
                           MTL::ComputePipelineState* state,
                           sg::pipeline_layout_handle layout)
      : sg::compute_pipeline(workgroup_size), _ctx(ctx), _state(state), _layout(cc::move(layout))
    {
    }

    ~metal_compute_pipeline() override { release_backend_objects(); }

    void release_backend_objects() override;

    [[nodiscard]] MTL::ComputePipelineState* state() const { return _state; }
    [[nodiscard]] sg::pipeline_layout_handle const& layout() const { return _layout; }

    /// Metal has no serialized-PSO blob on this path, so there is nothing to hand back.
    ///
    /// MTL4 does have binary archives (`MTL4Archive`), and they are a per-compiler store rather than sg's per-pipeline
    /// blob — so wiring them in is a design question about where the archive lives rather than a missing call.
    [[nodiscard]] cc::pinned_data<byte const> cached_pipeline_data() const override { return {}; }

private:
    metal_context& _ctx;
    MTL::ComputePipelineState* _state = nullptr;
    sg::pipeline_layout_handle _layout;
};
