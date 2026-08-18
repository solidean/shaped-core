#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/rendering/layout_routine.hh> // plan_textures
#include <shaped-viewer/view/view_id.hh>

/// The textures a render plan resolved to, owned for the length of the frame that recorded it.
///
/// Index-parallel to `render_plan::targets` / `::traces`, so a draw's source index reaches its texture directly.
/// The output's slot is deliberately empty: that target belongs to the caller.
struct sv::plan_resources
{
    cc::vector<sg::texture_2d> targets;
    cc::vector<sg::texture_2d> traces;

    [[nodiscard]] plan_textures textures() const { return {.targets = targets, .traces = traces}; }
};

/// Renders one view into its own intermediate target — itself a render routine, and the orchestrator that drives the trace.
///
/// It resolves the view's scene items to GPU resources through the managers, uploads the per-view constants, and path-traces the view into its own rgba16f texture.
/// That trace is driven through sv::pathtrace_routine, and the texture is what this routine hands back.
/// It never records a trace itself — that rule stays in the leaf routine.
/// This one only sequences it.
///
/// It rasters nothing and opens no scope, and it never sees the frame's output target.
/// Getting these textures onto a target is sv::viewer_renderer, which is the one place a rect, a viewport or the output's format is known.
///
/// **The persistent resources are the caller's**, held in the `sv::view_store` every call here takes.
/// The routine keeps no per-view state of its own, so two viewers on one context cannot collide in each other's view ids, and a store is a value a test can inspect.
/// It takes its own guard for the whole trace; the leaf it drives takes none, so driving it under the guard nests no lock.
///
/// Nothing here advances `resources` or the store: both are per-frame operations, and a per-view routine cannot know how many more views the frame holds.
/// Call `resources.begin_frame(ctx.current_epoch())` and `store.begin_frame(ctx.current_epoch())` once per frame, before the first view.
///
/// `sg::render_routine::evict` no longer reaches the accumulated images, since they are not held here — what keeps a
/// reloaded tracer from blending into an image the old shaders produced is the reload generation folded into the trace hash.
class sv::view_renderer : public sg::render_routine<view_renderer>
{
public:
    /// Path-traces `v`'s first scene_3d layer into the rgba16f texture `store` keeps under `v.id`, sized from `v.resolution`, and hands it back.
    /// Records the trace onto `cmd`; the returned texture is written on that same command list, so whatever samples it must be recorded there too.
    ///
    /// `v` needs a scene_3d layer holding at least one triangle-mesh item.
    /// Several traced layers per view is the multi-layer seam: this single-view entry point still traces the first alone.
    ///
    /// The texture persists across frames rather than expiring with the epoch, which is what lets the trace blend into it.
    /// It belongs to the store: holding it past the next `execute` or `store.begin_frame` for the same id is invalid, since either may resize or release it.
    /// A view whose traced image changed at all — camera, resolution, lights, geometry, settings — restarts from a blank accumulator on its own.
    [[nodiscard]] static sg::texture_2d execute(sg::command_list& cmd,
                                                view_data const& v,
                                                scene_resources& resources,
                                                view_store& store);

    /// Allocates or resizes every texture `plan` names, and hands them back index-parallel to its targets and traces.
    ///
    /// It also **touches every reachable view, refreshing or not**.
    /// A throttled view records nothing yet is still sampled by its parent, so leaving it untouched would let the
    /// store's idle reclaim release its texture out from under that parent.
    ///
    /// The output target is the caller's, so its slot comes back empty and nothing here allocates it.
    /// Takes no guard: it reads nothing this routine initializes, only the store and the context's persistent allocator.
    [[nodiscard]] static plan_resources resolve(sg::command_list& cmd, render_plan const& plan, view_store& store);

    /// Records the trace at `trace_index` into its own accumulation texture.
    /// Must be called with no rendering scope open, and before anything samples that texture.
    static void trace(sg::command_list& cmd,
                      viewer_definition const& def,
                      render_plan const& plan,
                      u32 trace_index,
                      plan_resources const& res,
                      scene_resources& resources,
                      view_store& store);

protected:
    /// No shaders of its own; it warms the path tracer so its compiles start early.
    void init_declare(sg::context& ctx) override;

private:
    /// Bumped every time the routine initializes, which is once per shader reload.
    ///
    /// Folded into the trace hash, so a reloaded tracer restarts rather than blending a new image into one the old
    /// shaders produced.
    /// Back when the accumulation lived on this routine, `evict` gave that for free; a store the caller owns outlives
    /// the instance, so the invalidation has to be said out loud.
    u64 _shader_generation = 0;
};
