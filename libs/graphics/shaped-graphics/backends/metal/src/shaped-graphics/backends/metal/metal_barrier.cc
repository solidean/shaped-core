#include "metal_barrier.hh"

namespace sg::backend::metal
{
MTL::Stages stages_of(sg::pipeline_stage_flag stage)
{
    switch (stage)
    {
    case sg::pipeline_stage_flag::vertex:
        return MTL::StageVertex;

    case sg::pipeline_stage_flag::draw_indirect:
        // Metal has no indirect-argument stage: an indirect draw's arguments are fetched by the vertex stage, and an
        // indirect dispatch's by the dispatch one.
        // Both, because the flag alone does not say which kind of indirect this was.
        return MTL::StageVertex | MTL::StageDispatch;

    case sg::pipeline_stage_flag::fragment:
    case sg::pipeline_stage_flag::render_target:
    case sg::pipeline_stage_flag::depth_stencil_target:
        // Colour and depth output are both the fragment stage here, where D3D12 and Vulkan each name them separately.
        return MTL::StageFragment;

    case sg::pipeline_stage_flag::compute:
        return MTL::StageDispatch;

    case sg::pipeline_stage_flag::copy:
        return MTL::StageBlit;

    case sg::pipeline_stage_flag::raytracing:
        // Ray tracing runs as a dispatch in Metal — there is no ray-tracing stage of its own.
        return MTL::StageDispatch;

    case sg::pipeline_stage_flag::accel_build:
        return MTL::StageAccelerationStructure;
    }

    return MTL::StageAll;
}

MTL::Stages stages_of(sg::pipeline_stage_flags stages)
{
    // An empty set means "not known", not "no stage" — see sg::binding's visibility for the same convention.
    // Ordering against nothing would be a barrier that orders nothing, so the conservative answer is every stage.
    if (stages.is_empty())
        return MTL::StageAll;

    // cc::flags carries no iteration, so the set is tested one flag at a time.
    // Listed exhaustively rather than looped, which is what makes a new sg stage a compile error here — the switch in
    // stages_of(pipeline_stage_flag) above would stop covering its enum.
    constexpr sg::pipeline_stage_flag k_all[] = {
        sg::pipeline_stage_flag::draw_indirect,
        sg::pipeline_stage_flag::vertex,
        sg::pipeline_stage_flag::fragment,
        sg::pipeline_stage_flag::compute,
        sg::pipeline_stage_flag::copy,
        sg::pipeline_stage_flag::render_target,
        sg::pipeline_stage_flag::depth_stencil_target,
        sg::pipeline_stage_flag::raytracing,
        sg::pipeline_stage_flag::accel_build,
    };

    MTL::Stages out = 0;
    for (auto const stage : k_all)
        if (stages.has(stage))
            out |= stages_of(stage);
    return out;
}

MTL4::VisibilityOptions visibility_for(sg::access_flags src_access, sg::access_flags dst_access)
{
    // Nothing was written, so nothing has to become visible and the barrier only has to order execution.
    // `MTL4VisibilityOptionNone` is that cheaper form, and on a tiler the difference is a real flush rather than a
    // bookkeeping detail.
    auto const writes = sg::is_unordered_write(src_access) || src_access.has(sg::access_flag::color_write)
                     || src_access.has(sg::access_flag::depth_write);
    if (!writes)
        return MTL4::VisibilityOptionNone;

    (void)dst_access;
    return MTL4::VisibilityOptionDevice;
}

metal_barrier translate_barrier(sg::access_barrier const& barrier)
{
    if (!barrier.needed)
        return {};

    // A layout transition is the one part with no Metal counterpart, and a barrier carrying only that is a barrier
    // with nothing left to emit once the layouts are dropped.
    auto const has_access = !barrier.src_access.is_empty() || !barrier.dst_access.is_empty();
    if (!has_access)
        return {};

    return {
        .needed = true,
        .after_stages = stages_of(barrier.src_stages),
        .before_stages = stages_of(barrier.dst_stages),
        .visibility = visibility_for(barrier.src_access, barrier.dst_access),
    };
}
} // namespace sg::backend::metal
