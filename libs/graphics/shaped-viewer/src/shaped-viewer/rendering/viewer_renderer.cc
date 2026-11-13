#include <clean-core/common/asserts.hh>
#include <clean-core/common/utility.hh> // cc::clamp
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/bloom_routine.hh>
#include <shaped-rendering/exposure_meter_routine.hh>
#include <shaped-rendering/slug_routine.hh>
#include <shaped-viewer/drawing/drawing_manager.hh>
#include <shaped-viewer/impl/view_state.hh>
#include <shaped-viewer/rendering/depth_fill_routine.hh>
#include <shaped-viewer/rendering/layout_routine.hh>
#include <shaped-viewer/rendering/view_renderer.hh>
#include <shaped-viewer/rendering/viewer_renderer.hh>
#include <shaped-viewer/resources/gpu_resource_manager.hh>
#include <shaped-viewer/view/view_data.hh>
#include <shaped-viewer/view/view_store.hh>
#include <shaped-viewer/view/viewer_definition.hh>

namespace sv
{
namespace
{
/// One atlas page's share of a drawing job: its uploaded frames and quads, drawn from that page's atlas.
struct page_job
{
    u32 page = 0;
    sr::slug_routine::prepared_job job;
};

/// Uploads one layer's drawings as Slug jobs, one per run of placements sharing an atlas page: a frame per placement,
/// and a quad per record under it.
/// How far an automatic exposure moves toward a new reading, per reading: a few frames to settle, and no flicker from noise.
constexpr f32 auto_exposure_rate = 0.2f;

/// Meters trace `i`'s image for its layer's automatic exposure, and folds in the reading an earlier frame started.
/// One reading is in flight per layer: a new meter starts only once the last one landed.
void meter_auto_exposure(sg::command_list& cmd,
                         viewer_definition const& def,
                         render_plan const& plan,
                         u32 i,
                         sg::texture_2d const& image,
                         view_store& store)
{
    auto const& tr = plan.traces[i];
    auto const& v = def[tr.view];
    if (!v.layers[tr.layer].settings.display.automatic || image.raw() == nullptr)
        return;

    auto& rec = store.get_or_create(v.id);
    while (rec.auto_exposure.size() <= isize(tr.layer))
        rec.auto_exposure.push_back({});
    auto& state = rec.auto_exposure[tr.layer];

    if (state.pending.is_valid())
    {
        if (!state.pending.is_ready())
            return;
        auto const partials = state.pending.try_get_data();
        state.pending = {};
        ++state.meters_landed;
        if (partials.has_value())
        {
            auto const average = sr::exposure_meter_routine::average_luminance(
                cc::span<tg::vec2f const>(partials.value().data(), partials.value().size()));
            // At the meter's floor nothing was drawn yet, as while the shaders compile, and such a reading would prime the exposure about 11 stops over.
            if (average.has_value() && average.value() > 1.01f * sr::exposure_meter_routine::darkest)
            {
                auto const target = cc::clamp(sr::exposure_for_average(average.value()), -24.0f, 24.0f);
                auto const rate = store.snaps_auto_exposure() ? 1.0f : auto_exposure_rate;
                state.exposure = state.primed ? state.exposure + (target - state.exposure) * rate : target;
                state.primed = true;
            }
        }
    }

    auto const partials = sr::exposure_meter_routine::execute(cmd, image);
    if (partials.has_value())
    {
        state.pending = cmd.download.data_from_buffer(partials.value());
        ++state.meters_started;
    }
}

/// What trace `i`'s layer is shown at, its automatic exposure included once a reading has landed.
[[nodiscard]] f32 shown_exposure_scale(viewer_definition const& def, render_plan const& plan, u32 i, view_store& store)
{
    auto const& tr = plan.traces[i];
    auto const& v = def[tr.view];
    auto display = v.layers[tr.layer].settings.display;
    auto const* const rec = store.get_ptr(v.id);
    if (display.automatic && rec != nullptr && isize(tr.layer) < rec->auto_exposure.size()
        && rec->auto_exposure[tr.layer].primed)
        display.exposure += rec->auto_exposure[tr.layer].exposure;
    return display.exposure_scale();
}

/// `draws` with every automatic exposure resolved: the layer's adapted exposure plus its compensation.
[[nodiscard]] cc::vector<layout_draw> with_auto_exposure(cc::span<layout_draw const> draws,
                                                         viewer_definition const& def,
                                                         render_plan const& plan,
                                                         view_store& store)
{
    auto out = cc::vector<layout_draw>();
    out.reserve(draws.size());
    for (auto const& d : draws)
        out.push_back(d);
    for (auto& d : out)
    {
        if (d.kind != draw_kind::view || d.primary.kind != draw_source_kind::trace || !d.display.automatic)
            continue;
        auto const& tr = plan.traces[d.primary.index];
        auto const* const rec = store.get_ptr(def[tr.view].id);
        if (rec != nullptr && isize(tr.layer) < rec->auto_exposure.size() && rec->auto_exposure[tr.layer].primed)
            d.display.exposure += rec->auto_exposure[tr.layer].exposure;
    }
    return out;
}

/// Runs keep the placements' order, so a drawing added later draws over an earlier one whichever pages they live in.
/// Records copies, so it runs before any pass opens.
[[nodiscard]] cc::vector<page_job> prepare_drawing_job(sg::command_list& cmd,
                                                       viewer_definition const& def,
                                                       plan_drawing_job const& job,
                                                       drawing_manager& drawings)
{
    auto const& placements
        = job.kind == drawing_job_kind::layer ? def[job.view].layers[job.layer].drawings : job.placements;
    auto out = cc::vector<page_job>();
    auto frames = cc::vector<sr::slug_frame>();
    auto quads = cc::vector<sr::slug_quad>();
    auto page = u32(0);
    auto const flush = [&]
    {
        if (!quads.empty())
            out.push_back({.page = page, .job = sr::slug_routine::prepare_job(cmd, drawings.atlas(page), frames, quads)});
        frames.clear();
        quads.clear();
    };
    for (auto const& p : placements)
    {
        if (p.record_count == 0)
            continue;
        if (p.page != page)
        {
            flush();
            page = p.page;
        }

        // A 2D position measured from a right or bottom edge points into the view, and is where the block's far edge
        // sits: the block's anchor counts back from that edge by the distance and by how far the block reaches, and
        // the placement keeps its offset within the block.
        auto at = p.at;
        if (!job.is_3d)
        {
            if (p.from == corner::top_right || p.from == corner::bottom_right)
                at[0] = job.logical_size[0] - (at[0] - p.offset[0]) - p.reach[0] + p.offset[0];
            if (p.from == corner::bottom_left || p.from == corner::bottom_right)
                at[1] = job.logical_size[1] - (at[1] - p.offset[1]) - p.reach[1] + p.offset[1];
        }

        auto const frame = u32(frames.size());
        frames.push_back({.at = at,
                          .x_axis = p.x_axis,
                          .y_axis = p.y_axis,
                          .tint = p.tint,
                          .visibility = p.visibility,
                          .probe = p.probe,
                          .probe_depth = p.probe_depth});
        for (auto r = p.first_record; r < p.first_record + p.record_count; ++r)
            quads.push_back({.record = r, .frame = frame});
    }
    flush();
    return out;
}

/// One layer's drawings as this frame records them: the uploaded job, for a 3D one the trace's depth it is tested
/// against, and for annotations the same depth their probes read; null where neither applies.
struct recorded_job
{
    cc::vector<page_job> pages;
    sg::texture_2d depth;
    sg::texture_2d probe;
};

/// The primary-hit depth the trace of `job`'s layer writes this frame, or null when it has none.
[[nodiscard]] sg::texture_2d primary_depth_of(plan_drawing_job const& job, viewer_definition const& def, view_store& store)
{
    if (job.trace == u32(-1))
        return {};
    auto const* const state = store.get_ptr(def[job.view].id);
    auto const* const slot
        = state != nullptr ? state->temporal.get_ptr(temporal_id::primary_depth(u8(job.layer))) : nullptr;
    return slot != nullptr ? slot->texture : sg::texture_2d();
}

/// Records a target's draws, in the order the plan lists them, into as many passes as its 3D drawings need.
///
/// Every run of layout draws and every untested drawings job shares one color pass.
/// A 3D job tested against its trace's depth gets two passes of its own: one filling a depth target from that depth,
/// then one drawing over the target's color against it, since nothing else in the target's pass is built for depth.
/// `color` is how the first pass treats the target; every later one preserves what the earlier ones drew.
/// Returns whether anything declined.
[[nodiscard]] bool record_target(sg::command_list& cmd,
                                 sg::color_target color,
                                 cc::span<layout_draw const> draws,
                                 plan_textures const& textures,
                                 render_plan const& plan,
                                 cc::span<recorded_job const> jobs,
                                 drawing_manager& drawings)
{
    auto const is_tested
        = [&](layout_draw const& d) { return d.kind == draw_kind::drawings && jobs[d.job].depth.raw() != nullptr; };

    auto declined = false;
    auto opened = false;
    auto i = isize(0);
    while (i < draws.size() || !opened)
    {
        // Everything up to the next tested job, in one color pass; the first pass opens even with nothing in it, so a
        // target with no draws is still cleared.
        auto end = i;
        while (end < draws.size() && !is_tested(draws[end]))
            ++end;
        if (end > i || !opened)
        {
            auto scope = cmd.raster.render_to({.color_targets = {color}});
            auto run_start = i;
            auto const flush = [&](isize run_end)
            {
                if (run_end > run_start
                    && layout_routine::execute(scope, window_id(0),
                                               draws.subspan({.offset = run_start, .size = run_end - run_start}), textures)
                           == sg::routine_outcome::declined)
                    declined = true;
            };
            for (auto k = i; k < end; ++k)
            {
                if (draws[k].kind != draw_kind::drawings)
                    continue;
                flush(k);
                run_start = k + 1;
                for (auto const& p : jobs[draws[k].job].pages)
                    if (sr::slug_routine::execute(scope, drawings.atlas(p.page), p.job,
                                                  {.object_to_clip = plan.drawing_jobs[draws[k].job].object_to_clip,
                                                   .probe_depth = jobs[draws[k].job].probe})
                        == sg::routine_outcome::declined)
                        declined = true;
            }
            flush(end);
            opened = true;
            color.op = sg::target_op::preserve;
        }
        if (end == draws.size())
            break;

        // A tested job: its trace's depth into a depth target, then the drawings against it over the color so far.
        auto const& recorded = jobs[draws[end].job];
        auto const depth = cmd.context().transient.create_texture_2d({.format = sg::pixel_format::depth32_float,
                                                                      .width = recorded.depth.width(),
                                                                      .height = recorded.depth.height(),
                                                                      .usage = sg::texture_usage::depth_stencil});
        {
            auto fill = cmd.raster.render_to({.depth_stencil_target = depth.as_depth_stencil_view().cleared(1.0f)});
            if (depth_fill_routine::execute(fill, recorded.depth) == sg::routine_outcome::declined)
                declined = true;
        }
        {
            auto scope = cmd.raster.render_to(
                {.color_targets = {color}, .depth_stencil_target = depth.as_depth_stencil_view().preserved()});
            for (auto const& p : recorded.pages)
                if (sr::slug_routine::execute(scope, drawings.atlas(p.page), p.job,
                                              {.object_to_clip = plan.drawing_jobs[draws[end].job].object_to_clip})
                    == sg::routine_outcome::declined)
                    declined = true;
        }
        i = end + 1;
    }
    return declined;
}
} // namespace

cc::shared_async<cc::unit> viewer_renderer::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    // The frame runs through the view renderer, so the edge is declared: this routine reports pending until it is ready.
    // The tracers are not in that chain, so a raster layer never waits on them; a traced layer declines while they come up.
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
    auto textures = res.textures();

    // Every drawing job's uploads, since a copy recorded inside a pass would split it.
    // A 3D job's depth is looked up now, though the trace writes it below: the slot is the same texture either way.
    auto jobs = cc::vector<recorded_job>();
    jobs.reserve(plan.drawing_jobs.size());
    for (auto const& job : plan.drawing_jobs)
    {
        auto const depth = primary_depth_of(job, def, store);
        jobs.push_back({.pages = prepare_drawing_job(cmd, def, job, resources.drawings),
                        .depth = job.is_3d ? depth : sg::texture_2d(),
                        .probe = job.kind == drawing_job_kind::annotations ? depth : sg::texture_2d()});
    }

    // Every trace's acquires before any trace freezes the tables, so the frame mints one snapshot of them, not one per
    // view that brought something new — each is a whole-table copy in the persistent descriptor heap.
    for (auto i = u32(0); i < plan.traces.size(); ++i)
        if (plan.traces[i].refresh)
            view_renderer::acquire_trace(cmd, def, plan, i, res, resources);

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

    // Every layer showing at an automatic exposure is metered from the image it shows, finished or not.
    for (auto i = u32(0); i < plan.traces.size(); ++i)
        meter_auto_exposure(cmd, def, plan, i, textures.traces[i], store);

    // After the meter, so turning bloom up does not move an automatic exposure.
    // From here a layer's image is the bloomed one, which only the composite reads; a trace next frame starts from its own mean.
    auto shown_traces = cc::vector<sg::texture_2d>::create_copy_of(textures.traces);
    for (auto i = u32(0); i < plan.traces.size(); ++i)
    {
        auto const& tr = plan.traces[i];
        auto const& display = def[tr.view].layers[tr.layer].settings.display;
        if (display.bloom.strength <= 0.0f || textures.traces[i].raw() == nullptr)
            continue;

        auto const bloomed = sr::bloom_routine::execute(cmd, textures.traces[i], display.bloom,
                                                        shown_exposure_scale(def, plan, i, store));
        if (bloomed.has_value())
            shown_traces[i] = bloomed.value();
        else
            declined = true;
    }
    textures.traces = shown_traces;

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

        auto const resolved = with_auto_exposure(plan.draws_of(ti), def, plan, store);
        auto const draws = cc::span<layout_draw const>(resolved);

        if (target.is_output)
        {
            // The layout routine is acquired per format, so it can still be building for THIS one even though the
            // chain above was ready — the one place in the frame where that is possible.
            if (record_target(cmd, output, draws, textures, plan, jobs, resources.drawings))
                declined = true;
            continue;
        }

        if (textures.targets[ti].raw() == nullptr)
            continue;

        // Cleared rather than preserved: a layout layer only covers its leaves, so the gaps between them (spacing,
        // empty grid cells, a collapsed rect) must be defined rather than holding whatever the texture held before.
        // Transparent black, since every view target carries premultiplied alpha.
        if (record_target(cmd, textures.targets[ti].as_render_target_view().cleared(tg::vec4f(0, 0, 0, 0)), draws,
                          textures, plan, jobs, resources.drawings))
            declined = true;
    }
    return declined ? sg::routine_outcome::declined : sg::routine_outcome::executed;
}
} // namespace sv
