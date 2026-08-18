#include <clean-core/common/asserts.hh>
#include <clean-core/common/utility.hh> // cc::clamp, cc::max, cc::min, cc::move
#include <shaped-viewer/rendering/render_plan.hh>
#include <shaped-viewer/view/view_data.hh>
#include <shaped-viewer/view/viewer_definition.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::round

namespace sv
{
namespace
{
constexpr tg::aabb2f full_uv = tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(1, 1));

/// What `emit` returns for a subtree it refused: an index into `render_plan::targets` that names none.
constexpr u32 invalid_target = u32(-1);

[[nodiscard]] int rect_w(tg::aabb2i const& r)
{
    return r.max[0] - r.min[0];
}
[[nodiscard]] int rect_h(tg::aabb2i const& r)
{
    return r.max[1] - r.min[1];
}

/// How this view's texels map onto window pixels: `w = t * scale + offset`.
/// Threaded down the walk so a leaf nested three views deep still knows where a cursor over it lands.
struct window_map
{
    tg::vec2f scale = tg::vec2f(1, 1);
    tg::vec2f offset = tg::vec2f(0, 0);
};

/// Where a source of `src_res` texels lands inside `rect`, and which part of it is sampled.
///
/// `native` is expressed entirely as a shrunk destination plus an inset uv, so nothing downstream ever learns what a
/// fit mode is: a source smaller than the rect insets the rect, a larger one crops centered.
struct fitted
{
    tg::aabb2i dst;
    tg::aabb2f uv = full_uv;
};

[[nodiscard]] fitted fit_source(fit_mode fit, tg::vec2i src_res, tg::aabb2i rect)
{
    if (fit == fit_mode::stretch || src_res[0] <= 0 || src_res[1] <= 0)
        return {.dst = rect, .uv = full_uv};

    auto const dw = cc::min(src_res[0], rect_w(rect));
    auto const dh = cc::min(src_res[1], rect_h(rect));

    auto const x0 = rect.min[0] + (rect_w(rect) - dw) / 2;
    auto const y0 = rect.min[1] + (rect_h(rect) - dh) / 2;

    auto const uw = float(dw) / float(src_res[0]);
    auto const uh = float(dh) / float(src_res[1]);

    return {.dst = tg::aabb2i(tg::pos2i(x0, y0), tg::pos2i(x0 + dw, y0 + dh)),
            .uv = tg::aabb2f(tg::pos2f((1.0f - uw) * 0.5f, (1.0f - uh) * 0.5f),
                             tg::pos2f((1.0f + uw) * 0.5f, (1.0f + uh) * 0.5f))};
}

/// `uv` narrowed by `zoom` around `center`, clamped so the window never leaves what the fit already chose.
///
/// Applied after the fit, so it magnifies the part of the source the leaf was going to show rather than fighting it —
/// and the clamp is what makes zooming back out always land on the whole image again.
[[nodiscard]] tg::aabb2f apply_zoom(tg::aabb2f const& uv, float zoom, tg::pos2f center)
{
    if (zoom <= 1.0f)
        return uv;

    auto const span_u = (uv.max[0] - uv.min[0]) / zoom;
    auto const span_v = (uv.max[1] - uv.min[1]) / zoom;

    auto const lo_u = cc::clamp(center[0] - span_u * 0.5f, uv.min[0], uv.max[0] - span_u);
    auto const lo_v = cc::clamp(center[1] - span_v * 0.5f, uv.min[1], uv.max[1] - span_v);

    return tg::aabb2f(tg::pos2f(lo_u, lo_v), tg::pos2f(lo_u + span_u, lo_v + span_v));
}

/// The map from a source's own texels to window pixels, given where it landed in its parent and what the parent's own map is.
[[nodiscard]] window_map compose(window_map const& parent, fitted const& f, tg::vec2i src_res)
{
    auto const span_u = cc::max(f.uv.max[0] - f.uv.min[0], 1e-6f);
    auto const span_v = cc::max(f.uv.max[1] - f.uv.min[1], 1e-6f);

    auto const sx = float(rect_w(f.dst)) / (float(cc::max(src_res[0], 1)) * span_u);
    auto const sy = float(rect_h(f.dst)) / (float(cc::max(src_res[1], 1)) * span_v);

    auto const ox = float(f.dst.min[0]) - f.uv.min[0] * float(rect_w(f.dst)) / span_u;
    auto const oy = float(f.dst.min[1]) - f.uv.min[1] * float(rect_h(f.dst)) / span_v;

    return {.scale = tg::vec2f(parent.scale[0] * sx, parent.scale[1] * sy),
            .offset = tg::vec2f(parent.offset[0] + parent.scale[0] * ox, parent.offset[1] + parent.scale[1] * oy)};
}

[[nodiscard]] tg::aabb2i map_rect(window_map const& m, tg::aabb2i r)
{
    auto const x0 = int(tg::round(float(r.min[0]) * m.scale[0] + m.offset[0]));
    auto const y0 = int(tg::round(float(r.min[1]) * m.scale[1] + m.offset[1]));
    auto const x1 = int(tg::round(float(r.max[0]) * m.scale[0] + m.offset[0]));
    auto const y1 = int(tg::round(float(r.max[1]) * m.scale[1] + m.offset[1]));
    return tg::aabb2i(tg::pos2i(x0, y0), tg::pos2i(cc::max(x1, x0), cc::max(y1, y0)));
}

/// Whether a target re-records this frame.
///
/// Forced on a first sighting and on a resize: those are the cases where the previous content is not merely stale but absent.
[[nodiscard]] bool should_refresh(view_data const& v, tg::vec2i resolution, view_history_entry const& h, u64 frame_index)
{
    if (!h.exists || h.resolution != resolution)
        return true;

    auto const rate = v.refresh.rate;
    if (rate >= 1.0f)
        return true;
    if (rate <= 0.0f)
        return false;

    // A clock running backwards means the caller restarted the loop, which is a refresh rather than a very long wait.
    if (frame_index < h.last_refresh_frame)
        return true;

    auto const period = u64(cc::max(1, int(tg::round(1.0f / rate))));
    return frame_index - h.last_refresh_frame >= period;
}

/// The builder's working state, so the two walks share one place to record into.
struct builder
{
    viewer_definition const& def;
    view_history const& history;
    u64 frame_index = 0;
    render_plan plan;

    cc::vector<tg::vec2i> resolution;
    cc::vector<u8> measured;
    cc::vector<u8> color; ///< 0 unvisited, 1 on the stack, 2 done
    cc::vector<u32> memo;
    u32 order = 0;

    /// Guards the measure fixpoint: resolutions only grow, but a broad DAG could still re-descend a lot.
    int measure_budget = 1 << 16;

    void diagnose(diagnostic_reason reason, view_id id, layout_node_id node)
    {
        plan.diagnostics.push_back({.reason = reason, .id = id, .node = node});
    }

    [[nodiscard]] bool valid_view(view_index view) const { return isize(u32(view)) < def.views.size(); }

    // ---- pass 1: resolutions ------------------------------------------------------------------------------

    /// Sizes every reachable view top-down, taking the largest rect any leaf asks it for.
    ///
    /// The max rather than the first sighting is deliberate: a view shared by a big pane and a small thumbnail must not
    /// have its resolution decided by which sibling happened to be authored first.
    void measure(view_index view, tg::vec2i requested, int depth)
    {
        if (!valid_view(view) || depth > max_layout_depth || measure_budget-- <= 0)
            return;

        auto const index = u32(view);
        auto const& v = def[view];
        auto const desired = v.resolution_follows_layout ? requested : v.resolution;

        auto const grew = !measured[index] || desired[0] > resolution[index][0] || desired[1] > resolution[index][1];
        if (!grew)
            return;
        if (color[index] == 1) // a back edge; the cycle is reported by the emit walk
            return;

        resolution[index] = measured[index] ? tg::vec2i(cc::max(resolution[index][0], desired[0]),
                                                        cc::max(resolution[index][1], desired[1]))
                                            : desired;
        measured[index] = 1;

        color[index] = 1;
        for (auto const& l : v.layers)
        {
            if (l.kind != layer_kind::layout)
                continue;

            auto const solution
                = resolve_layout(def.nodes, l.root_node,
                                 tg::aabb2i(tg::pos2i(0, 0), tg::pos2i(resolution[index][0], resolution[index][1])));
            for (auto const& item : solution.items)
            {
                if (item.kind != resolved_item::item_kind::leaf)
                    continue;
                for (auto const child : def.nodes[item.node].leaf.views)
                    measure(child, tg::vec2i(rect_w(item.rect), rect_h(item.rect)), depth + 1);
            }
        }
        color[index] = 0;
    }

    // ---- pass 2: emission ---------------------------------------------------------------------------------

    /// Appends everything the view at `index` needs, children first, and returns its target index.
    /// `invalid_target` means the subtree was refused; the referencing leaf then simply draws nothing.
    u32 emit(view_index view, window_map const& map, int depth)
    {
        if (!valid_view(view))
            return invalid_target;

        auto const index = u32(view);
        auto const& v = def[view];

        if (color[index] == 1)
        {
            diagnose(diagnostic_reason::cycle, v.id, invalid_node);
            return invalid_target;
        }
        if (depth > max_layout_depth)
        {
            diagnose(diagnostic_reason::too_deep, v.id, invalid_node);
            return invalid_target;
        }
        if (color[index] == 2)
            return memo[index]; // shared by two leaves: rendered once, sampled twice
        if (isize(plan.targets.size()) >= max_plan_targets)
            return invalid_target;

        color[index] = 1;
        plan.reachable.push_back(v.id);

        auto const res = resolution[index];

        // Sized here because an unset resolution means "the view's own", and this is the first point that knows it.
        for (auto const& t : temporal_inputs_of(v))
            plan.temporals.push_back({.id = v.id,
                                      .temporal_id = t.id,
                                      .resolution = t.resolution.has_value() ? t.resolution.value() : res,
                                      .format = t.format,
                                      .reset_hash = t.reset_hash});

        // Decided before the layer walk so a trace can carry it: the renderer needs to know whether a trace records
        // before it records, to rotate its history only on the frames that produce a new image.
        auto const refreshes = should_refresh(v, res, history.lookup(v.id), frame_index);

        auto local = cc::vector<layout_draw>();

        for (auto layer_index = u32(0); layer_index < v.layers.size(); ++layer_index)
        {
            auto const& l = v.layers[layer_index];
            switch (l.kind)
            {
            case layer_kind::scene_3d:
            {
                // A layer with no geometry renders nothing, so it gets no trace and no draw sampling one.
                // Emitting them anyway hands the renderer a dispatch with nothing to bind, which it asserts on —
                // and `add_scene().add_light(...)` before any mesh exists is ordinary authoring, not an error.
                if (!is_traceable(l))
                    break;

                auto const trace = u32(plan.traces.size());
                plan.traces.push_back(
                    {.id = v.id, .view = view, .layer = u8(layer_index), .resolution = res, .refresh = refreshes});
                local.push_back({.kind = draw_kind::view,
                                 .dst_rect = tg::aabb2i(tg::pos2i(0, 0), tg::pos2i(res[0], res[1])),
                                 .primary = {.kind = draw_source_kind::trace, .index = trace, .uv = full_uv},
                                 .blend = l.blend,
                                 .opacity = l.opacity});
                break;
            }
            case layer_kind::layout:
                emit_layout(view, l, res, map, depth, local);
                break;
            case layer_kind::scene_2d:
            case layer_kind::ui:
                // Neither draws yet; see libs/graphics/shaped-viewer/docs/TODO.md for what each still needs.
                break;
            }
        }

        color[index] = 2;

        auto const target = u32(plan.targets.size());
        memo[index] = target;

        plan.target_first_draw.push_back(u32(plan.draws.size()));
        plan.target_draw_count.push_back(u32(local.size()));
        for (auto& d : local)
            plan.draws.push_back(cc::move(d));

        plan.targets.push_back({.id = v.id, .view = view, .resolution = res, .refresh = refreshes});
        return target;
    }

    void emit_layout(view_index view,
                     layer const& l,
                     tg::vec2i res,
                     window_map const& map,
                     int depth,
                     cc::vector<layout_draw>& local)
    {
        auto const solution
            = resolve_layout(def.nodes, l.root_node, tg::aabb2i(tg::pos2i(0, 0), tg::pos2i(res[0], res[1])));

        for (auto const& item : solution.items)
        {
            if (item.kind == resolved_item::item_kind::background || item.kind == resolved_item::item_kind::border)
            {
                auto const kind
                    = item.kind == resolved_item::item_kind::background ? draw_kind::background : draw_kind::border;
                local.push_back({.kind = kind, .dst_rect = item.rect, .color = item.color, .node = item.node});
                continue;
            }

            auto const& leaf = def.nodes[item.node].leaf;
            if (leaf.views.empty())
                continue;

            // Everything the sources below push belongs *inside* this leaf, which is what the parent links record.
            auto const first_inner = u32(plan.hit_regions.size());

            auto const post = leaf.post_processes.empty() ? post_process{} : leaf.post_processes.front();
            if (leaf.post_processes.size() > 1)
                diagnose(diagnostic_reason::unsupported_chain, def[view].id, item.node);

            auto const required = post_process_source_count(post.kind);
            if (isize(leaf.views.size()) != required)
            {
                diagnose(diagnostic_reason::source_count_mismatch, def[view].id, item.node);
                continue;
            }

            // Every source is planned before this leaf's draw, which is what keeps a source's target index below ours.
            auto sources = cc::vector<draw_source>();
            auto first_fit = fitted{.dst = item.rect};
            auto first_res = tg::vec2i(0, 0);
            auto refused = false;

            for (auto const child : leaf.views)
            {
                auto const child_res = valid_view(child) ? resolution[u32(child)] : tg::vec2i(0, 0);
                auto f = fit_source(leaf.fit, child_res, item.rect);
                if (leaf.allow_zoom)
                    f.uv = apply_zoom(f.uv, leaf.zoom, leaf.zoom_center);
                auto const child_target = emit(child, compose(map, f, child_res), depth + 1);
                if (child_target == invalid_target)
                {
                    refused = true;
                    break;
                }
                if (sources.empty())
                {
                    first_fit = f;
                    first_res = child_res;
                }
                sources.push_back({.kind = draw_source_kind::target, .index = child_target, .uv = f.uv});
            }
            if (refused)
                continue;

            local.push_back({.kind = post.kind == post_process_kind::wipe ? draw_kind::wipe : draw_kind::view,
                             .dst_rect = first_fit.dst,
                             .primary = sources[0],
                             .secondary = sources.size() > 1 ? sources[1] : draw_source{},
                             .sampler = leaf.sampler,
                             .blend = l.blend,
                             .opacity = l.opacity,
                             .post = post,
                             .node = item.node});

            // One region per *reference*, so a view shared by two leaves is hit-testable in both places.
            // A view's own inner leaves are mapped through whichever reference emitted it first — sharing a view
            // between two differently-placed leaves makes its interior ambiguous, which only a z-ordered pick can settle.
            auto const child_map = compose(map, first_fit, first_res);
            auto const me = u32(plan.hit_regions.size());
            plan.hit_regions.push_back({.view = leaf.views[0],
                                        .id = def[leaf.views[0]].id,
                                        .node = item.node,
                                        .window_rect = map_rect(map, first_fit.dst),
                                        .scale = child_map.scale,
                                        .offset = child_map.offset,
                                        .order = order++});

            // Only the regions still unclaimed are this leaf's *direct* children; a deeper leaf already took its own.
            for (auto i = first_inner; i < me; ++i)
                if (plan.hit_regions[i].parent == invalid_hit_region)
                    plan.hit_regions[i].parent = me;
        }
    }
};
} // namespace

namespace
{
[[nodiscard]] bool contains(tg::aabb2i const& r, tg::pos2f p)
{
    auto const x = int(p[0]);
    auto const y = int(p[1]);
    return x >= r.min[0] && x < r.max[0] && y >= r.min[1] && y < r.max[1];
}

/// The chain of regions enclosing `r`, outermost first and `r` last.
void ancestry(cc::span<hit_region const> regions, u32 r, cc::vector<u32>& out)
{
    out.clear();
    for (auto i = r; i != invalid_hit_region; i = regions[i].parent)
        out.push_back(i);

    for (auto lo = isize(0), hi = out.size() - 1; lo < hi; ++lo, --hi)
    {
        auto const t = out[lo];
        out[lo] = out[hi];
        out[hi] = t;
    }
}

/// Whether the region at the end of `a` is drawn in front of the one at the end of `b`.
///
/// Compared at the first ancestor they disagree on, because that is where their draws actually diverge — and a region
/// nested inside another is always in front of it, which is why a pure prefix loses.
[[nodiscard]] bool in_front(cc::span<u32 const> a, cc::span<u32 const> b)
{
    auto const common = cc::min(a.size(), b.size());
    for (auto i = isize(0); i < common; ++i)
        if (a[i] != b[i])
            return a[i] > b[i];
    return a.size() > b.size();
}
} // namespace

u32 pick_hit_region(cc::span<hit_region const> regions, tg::pos2f window_point)
{
    auto best = invalid_hit_region;
    auto best_path = cc::vector<u32>();
    auto path = cc::vector<u32>();

    for (auto i = u32(0); i < regions.size(); ++i)
    {
        if (!contains(regions[i].window_rect, window_point))
            continue;

        ancestry(regions, i, path);
        if (best == invalid_hit_region || in_front(path, best_path))
        {
            best = i;
            best_path = path;
        }
    }
    return best;
}

view_history_entry view_history::lookup(view_id id) const
{
    auto const* const e = entries.get_ptr(id);
    return e == nullptr ? view_history_entry{} : *e;
}

cc::span<layout_draw const> render_plan::draws_of(u32 target) const
{
    CC_ASSERT(isize(target) < target_first_draw.size(), "no such target");
    return cc::span<layout_draw const>(draws).subspan(
        cc::offset_size{isize(target_first_draw[target]), isize(target_draw_count[target])});
}

bool render_plan::validate() const
{
    if (target_first_draw.size() != targets.size() || target_draw_count.size() != targets.size())
        return false;

    auto expected_first = u32(0);
    for (auto t = u32(0); t < targets.size(); ++t)
    {
        if (target_first_draw[t] != expected_first)
            return false;
        expected_first += target_draw_count[t];

        for (auto const& d : draws_of(t))
        {
            // The whole point of the post-order append: a source is finished before anything samples it.
            if (d.primary.kind == draw_source_kind::target && d.primary.index >= t)
                return false;
            if (d.kind == draw_kind::wipe && d.secondary.kind == draw_source_kind::target && d.secondary.index >= t)
                return false;
            if (d.primary.kind == draw_source_kind::trace && isize(d.primary.index) >= traces.size())
                return false;
        }
    }
    return expected_first == draws.size();
}

render_plan build_render_plan(viewer_definition const& def,
                              tg::vec2i output_size,
                              u64 frame_index,
                              view_history const& history)
{
    auto b = builder{.def = def, .history = history, .frame_index = frame_index};

    auto const n = def.views.size();
    b.resolution.resize_to_filled(n, tg::vec2i(0, 0));
    b.measured.resize_to_filled(n, u8(0));
    b.color.resize_to_filled(n, u8(0));
    b.memo.resize_to_filled(n, invalid_target);

    if (n == 0 || isize(u32(def.root_view)) >= n)
        return cc::move(b.plan);

    b.measure(def.root_view, output_size, 0);

    // The root's target *is* the output, so it renders at the output's size whatever it asked for.
    b.resolution[u32(def.root_view)] = output_size;

    auto const root = b.emit(def.root_view, window_map{}, 0);
    if (root != invalid_target)
    {
        b.plan.targets[root].is_output = true;
        b.plan.targets[root].refresh = true; // the caller's target holds nothing of ours to re-present

        // A root view is normally a layout wrapper and traces nothing, but nothing forbids it a scene_3d layer —
        // and a trace under a target forced to refresh has to be forced with it, or it re-presents into a cleared one.
        for (auto& tr : b.plan.traces)
            if (tr.view == def.root_view)
                tr.refresh = true;
    }

    return cc::move(b.plan);
}
} // namespace sv
