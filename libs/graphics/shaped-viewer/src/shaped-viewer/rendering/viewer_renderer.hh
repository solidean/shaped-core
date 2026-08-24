#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-viewer/fwd.hh>

/// Records a whole frame from its render plan: every trace, then one pass per texture the plan writes.
///
/// The plan is the frame flattened into recording order, so nothing here walks a layout or resolves a rect — it
/// replays what `build_render_plan` already decided, which is what makes nesting work at any depth.
///
/// The ordering rule it leans on: a plan's targets are in dependency order, so a source is always finished before the
/// pass that samples it.
/// Traces hoist above every pass because a ray-tracing dispatch may not be recorded inside one.
/// The loop is kind-driven rather than two fixed phases, so a future stage that *does* interleave stays expressible.
///
/// Nothing here advances `resources` or `store`: call `resources.advance_to(ctx.current_epoch())` and
/// `store.begin_frame(ctx.current_epoch())` once per frame, before this.
///
/// It holds no state and takes no guard, so the lock order through the frame is view_renderer, then the layout routine.
class sv::viewer_renderer : public sg::render_routine<viewer_renderer>
{
public:
    /// Records `plan` onto `cmd`: every trace first, then one pass per refreshing target, the frame's output last.
    /// The caller submits (and presents) it.
    ///
    /// `plan` must have been built from `def` — the traces name views and layers by index into it.
    /// `store` holds every texture the plan resolves to, and must be the one `plan`'s `view_history` was read from, or
    /// a view the plan believes it can re-present has nothing to re-present.
    /// `output` is the plan's output target and names the format the pipelines are built for; nothing writes the gaps
    /// a layout leaves, so pass `output.cleared(...)` to define them.
    ///
    /// One command list end to end: sg transitions each texture from its trace's UAV write to a sampled read on its own.
    static void execute(sg::command_list& cmd,
                        viewer_definition const& def,
                        render_plan const& plan,
                        gpu_resource_manager& resources,
                        view_store& store,
                        sg::color_target const& output);

protected:
    /// No shaders of its own; it warms the view renderer and the blit it places with.
    void init_declare(sg::context& ctx) override;
};
