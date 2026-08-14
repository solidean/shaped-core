#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/impl/keyed_cache.hh>
#include <shaped-viewer/rendering/layout_routine.hh> // plan_textures
#include <shaped-viewer/view/view_id.hh>

/// One traced layer's accumulation texture and how far its estimator has run.
///
/// `content_hash` covers everything the trace uploads, so any change to the image restarts it rather than averaging two
/// different scenes together.
struct sv::impl::accumulation_slot
{
    sg::texture_2d texture;
    u64 content_hash = 0;
    u32 accum_frame = 0;
};

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
/// This one only sequences it and owns the per-view persistent cache.
///
/// It rasters nothing and opens no scope, and it never sees the frame's output target.
/// Getting these textures onto a target is sv::viewer_renderer, which is the one place a rect, a viewport or the output's format is known.
///
/// Nothing here advances `resources` or this routine's own cache: both are per-frame operations, and a per-view routine cannot know how many more views the frame holds.
/// Call `resources.begin_frame(ctx.current_epoch())` and `view_renderer::begin_frame(cmd)` once per frame, before the first view.
///
/// Being a routine, the persistent cache lives on the per-context instance under the routine's own lock, rather than in a value the caller has to keep around.
/// It is keyed by view_id, which is what makes a view's accumulated image survive the frame that produced it.
/// It takes that guard for the whole trace; the leaf it drives takes none of its own, so driving it under the guard nests no lock.
///
/// `sg::render_routine::evict` drops this cache with the instance, so every view restarts its accumulation — correct, if visible as a one-frame flash of noise.
class sv::view_renderer : public sg::render_routine<view_renderer>
{
public:
    /// Path-traces `v`'s first scene_3d layer into the rgba16f texture kept under `v.id`, sized from `v.resolution`, and hands it back.
    /// Records the trace onto `cmd`; the returned texture is written on that same command list, so whatever samples it must be recorded there too.
    ///
    /// `v` needs a scene_3d layer holding at least one triangle-mesh item.
    /// Several traced layers per view is the multi-layer seam: the accumulation is still keyed by view id alone.
    ///
    /// The texture persists across frames rather than expiring with the epoch, which is what lets the trace blend into it.
    /// It belongs to this routine: holding it past the next `execute` or `begin_frame` for the same id is invalid, since either may resize or release it.
    /// A view whose traced image changed at all — camera, resolution, lights, geometry, settings — restarts from a blank accumulator on its own.
    [[nodiscard]] static sg::texture_2d execute(sg::command_list& cmd, view_data const& v, scene_resources& resources);

    /// Reclaim per-view persistent resources and advance the cache's clock to the context's current epoch.
    /// The frame's job, once, before the first view — mirroring `scene_resources::begin_frame`.
    ///
    /// Skipping it only means nothing is reclaimed, so a caller tracing a handful of views without a frame loop needs no ceremony.
    /// It takes a command list because that is the only way to reach the routine instance under its own lock.
    static void begin_frame(sg::command_list& cmd);

    /// How many frames the view under `id` has accumulated; 0 for a view that restarted this frame or was never seen.
    /// For tests and debug overlays — the trace needs none of it.
    [[nodiscard]] static u32 accumulated_frames(sg::command_list& cmd, view_id id);

    /// Allocates or resizes every texture `plan` names, and hands them back index-parallel to its targets and traces.
    ///
    /// It also **touches every reachable view, refreshing or not**.
    /// A throttled view records nothing yet is still sampled by its parent, so leaving it untouched would let the
    /// cache's idle reclaim release its texture out from under that parent.
    ///
    /// The output target is the caller's, so its slot comes back empty and nothing here allocates it.
    /// One exclusive acquire covers the whole call, which is what keeps this routine's lock off any open scope.
    [[nodiscard]] static plan_resources resolve(sg::command_list& cmd, render_plan const& plan);

    /// Records the trace at `trace_index` into its own accumulation texture.
    /// Must be called with no rendering scope open, and before anything samples that texture.
    static void trace(sg::command_list& cmd,
                      viewer_definition const& def,
                      render_plan const& plan,
                      u32 trace_index,
                      plan_resources const& res,
                      scene_resources& resources);

protected:
    /// No shaders of its own; it warms the path tracer so its compiles start early.
    void init_declare(sg::context& ctx) override;

private:
    /// Per-view resources that persist across frames.
    /// `content_hash` covers everything the trace uploads, so any change to the image restarts the accumulation rather than blending two different scenes.
    struct persistent_view_resources
    {
        /// One per traced layer, indexed by the layer's position in the view.
        /// A vector rather than a single texture because a view may hold several traced layers, and each converges on
        /// its own.
        cc::vector<impl::accumulation_slot> accumulation;

        /// This view's composited image — what its parent samples, and what it re-presents on a throttled frame.
        /// Absent for the frame's output, which is the caller's texture.
        sg::texture_2d composite;

        /// The frame this view last re-recorded, which is what its refresh rate is measured against.
        u64 last_refresh_frame = 0;
        bool has_refreshed = false;
    };

    sv::impl::keyed_cache<view_id, persistent_view_resources> _persistent;
};
