#include <clean-core/common/asserts.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/slug_routine.hh>
#include <shaped-viewer/drawing/drawing_manager.hh>
#include <shaped-viewer/rendering/layout_routine.hh>
#include <shaped-viewer/rendering/view_renderer.hh>
#include <shaped-viewer/rendering/viewer_renderer.hh>
#include <shaped-viewer/resources/gpu_resource_manager.hh>
#include <shaped-viewer/view/view_data.hh>
#include <shaped-viewer/view/viewer_definition.hh>

namespace sv
{
namespace
{
/// Uploads one layer's drawings as a Slug job: a frame per placement, and a quad per record under it.
/// Records copies, so it runs before any pass opens.
[[nodiscard]] sr::slug_routine::prepared_job prepare_drawing_job(sg::command_list& cmd,
                                                                 viewer_definition const& def,
                                                                 plan_drawing_job const& job,
                                                                 drawing_manager& drawings)
{
    auto const& placements = def[job.view].layers[job.layer].drawings;
    auto frames = cc::vector<sr::slug_frame>();
    auto quads = cc::vector<sr::slug_quad>();
    frames.reserve(placements.size());
    for (auto const& p : placements)
    {
        if (p.record_count == 0)
            continue;

        // A 2D position measured from a right or bottom edge points into the view, and is where the drawing's far edge
        // sits, so it counts back from that edge by the distance and by how far the drawing reaches.
        auto at = p.at;
        if (!job.is_3d)
        {
            if (p.from == corner::top_right || p.from == corner::bottom_right)
                at[0] = job.logical_size[0] - at[0] - p.reach[0];
            if (p.from == corner::bottom_left || p.from == corner::bottom_right)
                at[1] = job.logical_size[1] - at[1] - p.reach[1];
        }

        auto const frame = u32(frames.size());
        frames.push_back({.at = at, .x_axis = p.x_axis, .y_axis = p.y_axis, .tint = p.tint});
        for (auto r = p.first_record; r < p.first_record + p.record_count; ++r)
            quads.push_back({.record = r, .frame = frame});
    }
    return sr::slug_routine::prepare_job(cmd, drawings.atlas(), frames, quads);
}

/// Records a target's draws into its open pass: each run of layout draws through the layout routine, and each drawings
/// draw as its layer's Slug job, in the order the plan lists them.
/// Returns whether anything declined.
[[nodiscard]] bool record_target(sg::rendering_scope& scope,
                                 cc::span<layout_draw const> draws,
                                 plan_textures const& textures,
                                 render_plan const& plan,
                                 cc::span<sr::slug_routine::prepared_job const> jobs,
                                 sr::slug_atlas const& atlas)
{
    auto declined = false;
    auto run_start = isize(0);
    auto const flush = [&](isize end)
    {
        if (end > run_start
            && layout_routine::execute(scope, window_id(0),
                                       draws.subspan({.offset = run_start, .size = end - run_start}), textures)
                   == sg::routine_outcome::declined)
            declined = true;
    };
    for (auto i = isize(0); i < draws.size(); ++i)
    {
        if (draws[i].kind != draw_kind::drawings)
            continue;
        flush(i);
        run_start = i + 1;
        auto const& job = plan.drawing_jobs[draws[i].job];
        if (sr::slug_routine::execute(scope, atlas, jobs[draws[i].job], {.object_to_clip = job.object_to_clip})
            == sg::routine_outcome::declined)
            declined = true;
    }
    flush(draws.size());
    return declined;
}
} // namespace

cc::shared_async<cc::unit> viewer_renderer::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    // The frame runs through the view renderer, so the edge is declared: this routine reports pending until the whole
    // chain below it -- view_renderer, and the pathtracer it traces through -- is ready.
    _view_renderer = depend_on<view_renderer>(ctx);

    // The layout routine is neither declared nor prewarmed here, and cannot be either: it is acquired per target
    // format, this frame draws the output in one format and every intermediate target in another, and neither is
    // known until the frame runs.
    // So it is registered by the first execute that reaches it and brought up by the following tick, which costs the
    // frames in between -- an application that knows its swapchain format can prewarm it itself and skip that.
    co_return;
}

sg::routine_outcome viewer_renderer::execute(sg::command_list& cmd,
                                             viewer_definition const& def,
                                             render_plan const& plan,
                                             gpu_resource_manager& resources,
                                             view_store& store,
                                             sg::color_target const& output)
{
    // Nothing of ours is read back; what this establishes is that the whole chain below is ready.
    // A token holder is not handed out until its subtree is, so one check here stands for every routine under it.
    auto const self = try_acquire(cmd);
    if (!self.is_ready())
        return sg::routine_outcome::declined;

    CC_ASSERT(plan.validate(), "a render plan must be in dependency order before it is recorded");

    // A frame where any pass declined is a frame the caller should not treat as complete, even though the rest of it
    // recorded — a capture that saved it would be missing whatever that pass was going to place.
    auto declined = false;

    // Allocate (or resize) every texture the plan names, and touch every view it reaches.
    auto res = view_renderer::resolve(cmd, plan, store);
    auto const textures = res.textures();

    // Every drawing job's uploads, since a copy recorded inside a pass would split it.
    auto jobs = cc::vector<sr::slug_routine::prepared_job>();
    jobs.reserve(plan.drawing_jobs.size());
    for (auto const& job : plan.drawing_jobs)
        jobs.push_back(prepare_drawing_job(cmd, def, job, resources.drawings));

    // Every trace first.
    //
    // The builder proved this legal rather than the executor assuming it: no trace this frame reads a target this
    // frame, so all of them hoist above every pass.
    // The loop below would still be correct if that stopped holding — it would just alternate more — which is what a
    // future compute post-process would need.
    for (auto i = u32(0); i < plan.traces.size(); ++i)
        if (plan.traces[i].refresh)
            if (view_renderer::trace(cmd, def, plan, i, res, resources, store) == sg::routine_outcome::declined)
                declined = true;

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
            // The layout routine is acquired per format, so it can still be building for THIS one even though the
            // chain above was ready — the one place in the frame where that is possible.
            if (record_target(scope, draws, textures, plan, jobs, resources.drawings.atlas()))
                declined = true;
            continue;
        }

        if (textures.targets[ti].raw() == nullptr)
            continue;

        // Cleared rather than preserved: a layout layer only covers its leaves, so the gaps between them (spacing,
        // empty grid cells, a collapsed rect) must be defined rather than holding whatever the texture held before.
        // Transparent black, since every view target carries premultiplied alpha.
        auto scope = cmd.raster.render_to(
            {.color_targets = {textures.targets[ti].as_render_target_view().cleared(tg::vec4f(0, 0, 0, 0))}});
        if (record_target(scope, draws, textures, plan, jobs, resources.drawings.atlas()))
            declined = true;
    }
    return declined ? sg::routine_outcome::declined : sg::routine_outcome::executed;
}
} // namespace sv
