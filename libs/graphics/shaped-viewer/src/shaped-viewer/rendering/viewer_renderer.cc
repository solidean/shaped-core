#include <clean-core/common/asserts.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/rendering/layout_routine.hh>
#include <shaped-viewer/rendering/view_renderer.hh>
#include <shaped-viewer/rendering/viewer_renderer.hh>
#include <shaped-viewer/view/view_data.hh>
#include <shaped-viewer/view/viewer_definition.hh>

namespace sv
{
void viewer_renderer::init_declare(sg::context& ctx)
{
    // The frame runs through these two, so warm the whole chain when this is first initialized rather than stalling on the first frame.
    view_renderer::prewarm(ctx);
    layout_routine::prewarm(ctx);
}

void viewer_renderer::execute(sg::command_list& cmd,
                              viewer_definition const& def,
                              render_plan const& plan,
                              gpu_resource_manager& resources,
                              view_store& store,
                              sg::color_target const& output)
{
    // Nothing of ours is read back — this is what runs init_declare (and so warms the chain) on first use.
    (void)acquire(cmd);

    CC_ASSERT(plan.validate(), "a render plan must be in dependency order before it is recorded");

    // Allocate (or resize) every texture the plan names, and touch every view it reaches.
    auto const res = view_renderer::resolve(cmd, plan, store);
    auto const textures = res.textures();

    // Every trace first.
    //
    // The builder proved this legal rather than the executor assuming it: no trace this frame reads a target this
    // frame, so all of them hoist above every pass.
    // The loop below would still be correct if that stopped holding — it would just alternate more — which is what a
    // future compute post-process would need.
    for (auto i = u32(0); i < plan.traces.size(); ++i)
        if (plan.traces[i].refresh)
            view_renderer::trace(cmd, def, plan, i, res, resources, store);

    // Then one pass per target, in dependency order, so a source is finished before anything samples it.
    // Each pass closes before the next begins, which is what releases the output-merger binding — a target still bound
    // could not be transitioned to a sampled read.
    for (auto ti = u32(0); ti < plan.targets.size(); ++ti)
    {
        auto const& target = plan.targets[ti];

        // A throttled target records nothing, and whatever samples it reads last frame's content.
        // That is exactly why a view target is persistent rather than pooled.
        if (!target.refresh)
            continue;

        auto const draws = plan.draws_of(ti);

        if (target.is_output)
        {
            auto scope = cmd.raster.render_to({.color_targets = {output}});
            layout_routine::execute(scope, window_id(0), draws, textures);
            continue;
        }

        if (textures.targets[ti].raw() == nullptr)
            continue;

        // Cleared rather than preserved: a layout layer only covers its leaves, so the gaps between them (spacing,
        // empty grid cells, a collapsed rect) must be defined rather than holding whatever the texture held before.
        // Transparent black, since every view target carries premultiplied alpha.
        auto scope = cmd.raster.render_to(
            {.color_targets = {textures.targets[ti].as_render_target_view().cleared(tg::vec4f(0, 0, 0, 0))}});
        layout_routine::execute(scope, window_id(0), draws, textures);
    }
}
} // namespace sv
