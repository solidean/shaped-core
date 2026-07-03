#pragma once

#include <shaped-graphics/fwd.hh>

namespace sg
{
/// A ready-to-run compute pipeline: a compute shader compiled against a binding_layout. Bound to a
/// command list and dispatched. Held via compute_pipeline_handle.
///
/// Abstract: a backend subclasses it and owns the native object (dx12 pipeline state + root signature,
/// vulkan VkPipeline + VkPipelineLayout). See libs/graphics/shaped-graphics/docs/concepts/bindings.md.
class compute_pipeline
{
public:
    virtual ~compute_pipeline();

protected:
    compute_pipeline() = default;
};
} // namespace sg
