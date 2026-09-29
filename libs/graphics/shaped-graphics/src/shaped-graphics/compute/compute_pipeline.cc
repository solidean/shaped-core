#include <shaped-graphics/compute/compute_pipeline.hh>

namespace sg
{
compute_pipeline::~compute_pipeline() = default;

void impl::set_footprint(compute_pipeline const& pipeline, impl::pipeline_footprint footprint)
{
    // Only ever called on a pipeline nobody else holds yet, and never on one created const.
    const_cast<compute_pipeline&>(pipeline)._footprint = cc::move(footprint);
}
} // namespace sg
