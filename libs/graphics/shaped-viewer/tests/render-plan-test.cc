#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-viewer/rendering/render_plan.hh>
#include <shaped-viewer/view/view_data.hh>
#include <shaped-viewer/view/viewer_definition.hh>

// The render plan is a pure function of a definition, so every invariant that keeps the recorder correct is pinned
// here without a GPU: the dependency ordering, the nesting, cycle and depth safety, sharing, and the refresh clock.

using namespace cc::primitive_defines;

namespace
{
[[nodiscard]] tg::aabb2i rect_of(int x0, int y0, int x1, int y1)
{
    return tg::aabb2i(tg::pos2i(x0, y0), tg::pos2i(x1, y1));
}

/// A view holding one traced scene, which is the only layer kind that renders today.
[[nodiscard]] sv::view_index add_traced_view(sv::viewer_definition& def, char const* name)
{
    auto v = sv::view_data{};
    v.id = sv::view_id::from_string(name);
    sv::ensure_scene_3d(v).items.push_back({.mesh = sv::mesh_id(1), .instance = sv::instance_id(1)});
    def.views.push_back(cc::move(v));
    return sv::view_index(def.views.size() - 1);
}

/// A view whose single layer is a layout tree, and the tree's root node.
[[nodiscard]] sv::view_index add_layout_view(sv::viewer_definition& def,
                                             char const* name,
                                             sv::box_style style = {},
                                             sv::grid_params grid = {})
{
    auto const root = def.nodes.add_container(sv::invalid_node, style, grid);

    auto v = sv::view_data{};
    v.id = sv::view_id::from_string(name);
    v.layers.push_back({.kind = sv::layer_kind::layout, .blend = sv::layer_blend::replace, .root_node = root});
    def.views.push_back(cc::move(v));
    return sv::view_index(def.views.size() - 1);
}

[[nodiscard]] sv::layout_node_id layout_root_of(sv::viewer_definition const& def, sv::view_index view)
{
    return def[view].layers[0].root_node;
}

/// Appends a leaf naming `views` under `parent`.
sv::layout_node_id add_leaf(sv::viewer_definition& def,
                            sv::layout_node_id parent,
                            cc::span<sv::view_index const> views,
                            sv::layout_leaf proto = {})
{
    auto leaf = cc::move(proto);
    for (auto const v : views)
        leaf.views.push_back(v);
    return def.nodes.add_leaf(parent, cc::move(leaf));
}

/// The single-view leaf almost every case below wants.
sv::layout_node_id add_leaf(sv::viewer_definition& def,
                            sv::layout_node_id parent,
                            sv::view_index view,
                            sv::layout_leaf proto = {})
{
    return add_leaf(def, parent, cc::span<sv::view_index const>(&view, 1), cc::move(proto));
}

[[nodiscard]] int count_targets_named(sv::render_plan const& plan, char const* name)
{
    auto const id = sv::view_id::from_string(name);
    auto n = 0;
    for (auto const& t : plan.targets)
        if (t.id == id)
            ++n;
    return n;
}

[[nodiscard]] bool has_diagnostic(sv::render_plan const& plan, sv::diagnostic_reason reason)
{
    for (auto const& d : plan.diagnostics)
        if (d.reason == reason)
            return true;
    return false;
}
} // namespace

TEST("sv - a plan of one traced view is one target and one trace")
{
    auto def = sv::viewer_definition{};
    def.root_view = add_traced_view(def, "only");

    auto const plan = sv::build_render_plan(def, tg::vec2i(160, 90), 0, {});

    REQUIRE(plan.validate());
    REQUIRE(plan.targets.size() == 1);
    REQUIRE(plan.traces.size() == 1);

    // The root's target is the caller's output, and it renders at the output's size whatever it asked for.
    CHECK(plan.targets[0].is_output);
    CHECK(plan.targets[0].resolution == tg::vec2i(160, 90));
    CHECK(plan.traces[0].resolution == tg::vec2i(160, 90));

    // One draw, sampling the trace across the whole target.
    auto const draws = plan.draws_of(0);
    REQUIRE(draws.size() == 1);
    CHECK(draws[0].kind == sv::draw_kind::view);
    CHECK(draws[0].primary.kind == sv::draw_source_kind::trace);
    CHECK(draws[0].dst_rect == rect_of(0, 0, 160, 90));
}

TEST("sv - nested views are planned children first")
{
    // output <- root(layout) <- two traced views side by side
    auto def = sv::viewer_definition{};
    def.root_view = add_layout_view(def, "root", {}, {.rows = 1});
    auto const left = add_traced_view(def, "left");
    auto const right = add_traced_view(def, "right");

    auto const root_node = layout_root_of(def, def.root_view);
    add_leaf(def, root_node, left);
    add_leaf(def, root_node, right);

    auto const plan = sv::build_render_plan(def, tg::vec2i(160, 90), 0, {});

    REQUIRE(plan.validate());

    // Three textures: each nested view's own, then the output last.
    REQUIRE(plan.targets.size() == 3);
    CHECK(plan.targets[2].is_output);
    CHECK(!(plan.targets[0].is_output));

    // Each half is traced at its own cell's size, not the output's.
    REQUIRE(plan.traces.size() == 2);
    CHECK(plan.traces[0].resolution == tg::vec2i(80, 90));
    CHECK(plan.traces[1].resolution == tg::vec2i(80, 90));

    // The output samples the two finished targets, each across its cell.
    auto const draws = plan.draws_of(2);
    REQUIRE(draws.size() == 2);
    CHECK(draws[0].primary.kind == sv::draw_source_kind::target);
    CHECK(draws[0].dst_rect == rect_of(0, 0, 80, 90));
    CHECK(draws[1].dst_rect == rect_of(80, 0, 160, 90));
}

TEST("sv - the phase structure does not grow with nesting depth")
{
    // output <- a(layout) <- b(layout) <- c(traced): the worked three-level case.
    auto def = sv::viewer_definition{};
    def.root_view = add_layout_view(def, "a");
    auto const b = add_layout_view(def, "b");
    auto const c = add_traced_view(def, "c");

    add_leaf(def, layout_root_of(def, b), c);
    add_leaf(def, layout_root_of(def, def.root_view), b);

    auto const plan = sv::build_render_plan(def, tg::vec2i(128, 128), 0, {});

    REQUIRE(plan.validate());

    // One dispatch and one pass per target — the alternation count is 1 regardless of how deep the nesting goes,
    // because no trace this frame reads any target this frame.
    CHECK(plan.traces.size() == 1);
    REQUIRE(plan.targets.size() == 3);

    // Post-order: c, then b, then a. Every source is finished before the target that samples it.
    CHECK(plan.targets[0].id == sv::view_id::from_string("c"));
    CHECK(plan.targets[1].id == sv::view_id::from_string("b"));
    CHECK(plan.targets[2].id == sv::view_id::from_string("a"));
    CHECK(plan.targets[2].is_output);
}

TEST("sv - a view referenced twice is rendered once at the larger resolution")
{
    // A wide pane and a narrow one both showing the same view: it converges once and is sampled twice.
    auto def = sv::viewer_definition{};
    def.root_view = add_layout_view(def, "root", {}, {.cols = 2, .rows = 1});

    auto const shared = add_traced_view(def, "shared");
    auto const root_node = layout_root_of(def, def.root_view);
    add_leaf(def, root_node, shared);
    add_leaf(def, root_node, shared);

    auto const plan = sv::build_render_plan(def, tg::vec2i(200, 100), 0, {});

    REQUIRE(plan.validate());

    // Rendered once — a shared view is a DAG, not a cycle.
    CHECK(count_targets_named(plan, "shared") == 1);
    CHECK(plan.traces.size() == 1);

    // Both cells are 100 wide here, so this only pins that a shared view takes one resolution rather than the
    // sibling order deciding it.
    CHECK(plan.traces[0].resolution == tg::vec2i(100, 100));

    // Two draws in the output, both naming the same source target.
    auto const draws = plan.draws_of(u32(plan.targets.size() - 1));
    REQUIRE(draws.size() == 2);
    CHECK(draws[0].primary.index == draws[1].primary.index);

    // And two hit regions, so the shared view is pickable in both places.
    CHECK(plan.hit_regions.size() == 2);
}

TEST("sv - a shared view takes the largest rect that asks for it")
{
    // The sharp case: a big pane and a small inset.
    // Taking the first sighting would make the image depend on which sibling happened to be authored first, so the
    // builder takes the max.
    auto def = sv::viewer_definition{};
    def.root_view = add_layout_view(def, "root");
    auto const shared = add_traced_view(def, "shared");
    auto const root_node = layout_root_of(def, def.root_view);

    // A small inset first, then the full-size flowed leaf.
    auto const inset = def.nodes.add_relative(root_node, {.position = tg::pos2f(0, 0), .size = tg::vec2f(0.25f, 0.25f)});
    add_leaf(def, inset, shared);
    add_leaf(def, root_node, shared);

    auto const plan = sv::build_render_plan(def, tg::vec2i(400, 200), 0, {});

    REQUIRE(plan.validate());
    REQUIRE(plan.traces.size() == 1);
    CHECK(plan.traces[0].resolution == tg::vec2i(400, 200)); // the big one wins, not the 100x50 inset
}

TEST("sv - a cycle is reported and every other leaf still renders")
{
    // root's layout holds a view whose own layout points back at root.
    auto def = sv::viewer_definition{};
    def.root_view = add_layout_view(def, "root", {}, {.rows = 1});
    auto const looper = add_layout_view(def, "looper");
    auto const fine = add_traced_view(def, "fine");

    add_leaf(def, layout_root_of(def, looper), def.root_view); // the back edge

    auto const root_node = layout_root_of(def, def.root_view);
    add_leaf(def, root_node, looper);
    add_leaf(def, root_node, fine);

    auto const plan = sv::build_render_plan(def, tg::vec2i(160, 90), 0, {});

    // It degrades rather than asserting: a view tree is frequently data, and the default preset has assertions on.
    CHECK(has_diagnostic(plan, sv::diagnostic_reason::cycle));
    REQUIRE(plan.validate());

    // The unaffected sibling is untouched — the failure is local to the leaf that closed the loop.
    CHECK(count_targets_named(plan, "fine") == 1);
    CHECK(plan.traces.size() == 1);
}

TEST("sv - nesting past the depth cap is reported rather than overflowing")
{
    // A chain of layout views one deeper than the cap allows.
    auto def = sv::viewer_definition{};
    auto chain = cc::vector<sv::view_index>();
    for (auto i = 0; i < sv::max_layout_depth + 3; ++i)
        chain.push_back(add_layout_view(def, i == 0 ? "d0" : "deep"));

    for (auto i = 0; i + 1 < isize(chain.size()); ++i)
        add_leaf(def, layout_root_of(def, chain[i]), chain[i + 1]);

    def.root_view = chain[0];
    auto const plan = sv::build_render_plan(def, tg::vec2i(64, 64), 0, {});

    CHECK(has_diagnostic(plan, sv::diagnostic_reason::too_deep));
    CHECK(plan.validate());
    CHECK(isize(plan.targets.size()) <= sv::max_layout_depth + 1);
}

TEST("sv - a leaf whose view count disagrees with its post-process is reported")
{
    auto def = sv::viewer_definition{};
    def.root_view = add_layout_view(def, "root");
    auto const only = add_traced_view(def, "only");

    // A wipe combines two views; naming one is a caller error the builder reports rather than asserting on.
    auto proto = sv::layout_leaf{};
    proto.post_processes.push_back({.kind = sv::post_process_kind::wipe});
    add_leaf(def, layout_root_of(def, def.root_view), only, cc::move(proto));

    auto const plan = sv::build_render_plan(def, tg::vec2i(64, 64), 0, {});

    CHECK(has_diagnostic(plan, sv::diagnostic_reason::source_count_mismatch));
    CHECK(plan.validate());
}

TEST("sv - a wipe leaf plans one draw over two sources")
{
    auto def = sv::viewer_definition{};
    def.root_view = add_layout_view(def, "root");
    auto const a = add_traced_view(def, "a");
    auto const b = add_traced_view(def, "b");

    auto proto = sv::layout_leaf{};
    proto.post_processes.push_back({.kind = sv::post_process_kind::wipe, .split = 0.25f});

    auto const both = cc::vector<sv::view_index>{a, b};
    add_leaf(def, layout_root_of(def, def.root_view), both, cc::move(proto));

    auto const plan = sv::build_render_plan(def, tg::vec2i(128, 64), 0, {});

    REQUIRE(plan.validate());

    // Both sides converge on their own, and the wipe is one draw — no intermediate texture and no extra pass, which
    // is why dragging the split cannot restart either trace.
    CHECK(plan.traces.size() == 2);
    REQUIRE(plan.targets.size() == 3);

    auto const draws = plan.draws_of(2);
    REQUIRE(draws.size() == 1);
    CHECK(draws[0].kind == sv::draw_kind::wipe);
    CHECK(draws[0].post.split == 0.25f);
    CHECK(draws[0].primary.index != draws[0].secondary.index);
}

TEST("sv - a border emits its bands before the leaf it frames")
{
    auto def = sv::viewer_definition{};
    def.root_view = add_layout_view(def, "root", {.border = 2, .border_color = tg::vec4f(1, 0, 0, 1)});
    auto const inner = add_traced_view(def, "inner");
    add_leaf(def, layout_root_of(def, def.root_view), inner);

    auto const plan = sv::build_render_plan(def, tg::vec2i(100, 100), 0, {});

    REQUIRE(plan.validate());

    auto const draws = plan.draws_of(u32(plan.targets.size() - 1));
    REQUIRE(draws.size() == 5); // four bands, then the view
    for (auto i = 0; i < 4; ++i)
        CHECK(draws[i].kind == sv::draw_kind::border);
    CHECK(draws[4].kind == sv::draw_kind::view);
    CHECK(draws[4].dst_rect == rect_of(2, 2, 98, 98));
}

TEST("sv - a background reaches the plan as a flat fill under the border")
{
    auto def = sv::viewer_definition{};
    def.root_view = add_layout_view(
        def, "root",
        {.border = 2, .border_color = tg::vec4f(1, 0, 0, 1), .background_color = tg::vec4f(0, 0, 1, 1), .padding = 8});
    auto const inner = add_traced_view(def, "inner");
    add_leaf(def, layout_root_of(def, def.root_view), inner);

    auto const plan = sv::build_render_plan(def, tg::vec2i(100, 100), 0, {});

    REQUIRE(plan.validate());

    auto const draws = plan.draws_of(u32(plan.targets.size() - 1));
    REQUIRE(draws.size() == 6); // the fill, then four bands, then the view
    CHECK(draws[0].kind == sv::draw_kind::background);
    CHECK(draws[0].dst_rect == rect_of(0, 0, 100, 100));
    CHECK(draws[0].color == tg::vec4f(0, 0, 1, 1));
    for (auto i = 1; i < 5; ++i)
        CHECK(draws[i].kind == sv::draw_kind::border);

    // The padded gap between the fill and the view is exactly what the background is there to color.
    CHECK(draws[5].kind == sv::draw_kind::view);
    CHECK(draws[5].dst_rect == rect_of(10, 10, 90, 90));
}

TEST("sv - a throttled view refreshes on its own rate but is reachable every frame")
{
    auto def = sv::viewer_definition{};
    def.root_view = add_layout_view(def, "root");
    auto const slow = add_traced_view(def, "slow");
    def[slow].refresh.rate = 0.5f; // every second frame
    add_leaf(def, layout_root_of(def, def.root_view), slow);

    auto const slow_id = sv::view_id::from_string("slow");

    auto history = sv::view_history{};
    auto refreshes = 0;
    auto reachable_every_frame = true;

    for (auto frame = u64(0); frame < 10; ++frame)
    {
        auto const plan = sv::build_render_plan(def, tg::vec2i(64, 64), frame, history);
        REQUIRE(plan.validate());

        // The trap this pins: a throttled view must be *touched* every frame, or the cache's idle reclaim releases
        // its texture out from under the parent that samples it.
        auto found = false;
        for (auto const id : plan.reachable)
            if (id == slow_id)
                found = true;
        reachable_every_frame = reachable_every_frame && found;

        for (auto const& t : plan.targets)
        {
            if (t.id != slow_id)
                continue;
            if (t.refresh)
            {
                ++refreshes;
                history.entries[slow_id] = {.exists = true, .resolution = t.resolution, .last_refresh_frame = frame};
            }
        }
    }

    CHECK(reachable_every_frame);
    CHECK(refreshes == 5); // frames 0, 2, 4, 6, 8
}

TEST("sv - a resize refreshes a throttled view immediately")
{
    auto def = sv::viewer_definition{};
    def.root_view = add_layout_view(def, "root");
    auto const slow = add_traced_view(def, "slow");
    def[slow].refresh.rate = 0.0f; // only when something invalidates it
    add_leaf(def, layout_root_of(def, def.root_view), slow);

    auto const slow_id = sv::view_id::from_string("slow");
    auto history = sv::view_history{};
    history.entries[slow_id] = {.exists = true, .resolution = tg::vec2i(64, 64), .last_refresh_frame = 0};

    // Same size and a zero rate: nothing to redo, so the parent re-presents what is already there.
    auto const steady = sv::build_render_plan(def, tg::vec2i(64, 64), 100, history);
    for (auto const& t : steady.targets)
        if (t.id == slow_id)
            CHECK(!(t.refresh));

    // A different size has no previous content to present, so the rate cannot suppress it.
    auto const resized = sv::build_render_plan(def, tg::vec2i(128, 64), 101, history);
    for (auto const& t : resized.targets)
        if (t.id == slow_id)
            CHECK(t.refresh);
}

TEST("sv - the output target always refreshes")
{
    // It is the caller's texture, so there is no previous content of ours to re-present.
    auto def = sv::viewer_definition{};
    def.root_view = add_traced_view(def, "root");
    def[def.root_view].refresh.rate = 0.0f;

    auto history = sv::view_history{};
    history.entries[sv::view_id::from_string("root")]
        = {.exists = true, .resolution = tg::vec2i(64, 64), .last_refresh_frame = 0};

    auto const plan = sv::build_render_plan(def, tg::vec2i(64, 64), 50, history);
    REQUIRE(plan.targets.size() == 1);
    CHECK(plan.targets[0].is_output);
    CHECK(plan.targets[0].refresh);
}

TEST("sv - a nested leaf maps into window space")
{
    // A view nested inside the right half: a cursor over its middle must land in the middle of *its* texture.
    auto def = sv::viewer_definition{};
    def.root_view = add_layout_view(def, "root", {}, {.rows = 1});
    auto const left = add_traced_view(def, "left");
    auto const right = add_traced_view(def, "right");

    auto const root_node = layout_root_of(def, def.root_view);
    add_leaf(def, root_node, left);
    add_leaf(def, root_node, right);

    auto const plan = sv::build_render_plan(def, tg::vec2i(160, 90), 0, {});
    REQUIRE(plan.hit_regions.size() == 2);

    auto const& r = plan.hit_regions[1];
    CHECK(r.view == right);
    CHECK(r.window_rect == rect_of(80, 0, 160, 90));

    // The right view is 80x90 and sits at x=80, so its texel 0 is window x=80 and the map is 1:1.
    CHECK(r.scale[0] == 1.0f);
    CHECK(r.offset[0] == 80.0f);

    // Later regions draw in front, which is what a hit test picks by.
    CHECK(plan.hit_regions[1].order > plan.hit_regions[0].order);
}

TEST("sv - an empty definition plans nothing")
{
    auto const empty = sv::viewer_definition{};
    auto const plan = sv::build_render_plan(empty, tg::vec2i(64, 64), 0, {});
    CHECK(plan.targets.empty());
    CHECK(plan.draws.empty());
    CHECK(plan.validate());
}

TEST("sv - a hit lands on the nested view, not the wrapper containing it")
{
    // output <- root(layout) <- middle(layout) <- two traced views.
    //
    // Every one of these regions covers the same pixels, so ordering is the whole answer.
    // Emission puts the inner regions *before* the wrapper's — children are planned first — so a plain "highest
    // counter wins" picks the wrapper, whose camera drives nothing.
    // Painter's order over the parent links is what fixes it.
    auto def = sv::viewer_definition{};
    def.root_view = add_layout_view(def, "root", {}, {.rows = 1});
    auto const left = add_traced_view(def, "left");
    auto const right = add_traced_view(def, "right");

    auto const inner_node = def.nodes.add_container(sv::invalid_node, {}, {.rows = 1});
    add_leaf(def, inner_node, left);
    add_leaf(def, inner_node, right);

    auto middle = sv::view_data{};
    middle.id = sv::view_id::from_string("middle");
    middle.layers.push_back({.kind = sv::layer_kind::layout, .blend = sv::layer_blend::replace, .root_node = inner_node});
    def.views.push_back(cc::move(middle));
    auto const middle_index = sv::view_index(def.views.size() - 1);

    add_leaf(def, layout_root_of(def, def.root_view), middle_index);

    auto const plan = sv::build_render_plan(def, tg::vec2i(200, 100), 0, {});
    REQUIRE(plan.validate());
    REQUIRE(plan.hit_regions.size() == 3);

    // A point in the left half is the left view's, not the wrapper's.
    auto const hit = sv::pick_hit_region(plan.hit_regions, tg::pos2f(50, 50));
    REQUIRE(hit != sv::invalid_hit_region);
    CHECK(plan.hit_regions[hit].id == sv::view_id::from_string("left"));

    // And the right half is the right view's.
    auto const other = sv::pick_hit_region(plan.hit_regions, tg::pos2f(150, 50));
    REQUIRE(other != sv::invalid_hit_region);
    CHECK(plan.hit_regions[other].id == sv::view_id::from_string("right"));

    // The wrapper is still reachable in principle, but never where a child covers the point.
    CHECK(plan.hit_regions[hit].parent != sv::invalid_hit_region);
}

TEST("sv - a shallow overlay wins over a deeper view underneath it")
{
    // The case depth-first picking gets wrong: the inset sits at the root's level, so it is *shallower* than the
    // views inside `middle` — but it is drawn after them, so it must win where it overlaps.
    auto def = sv::viewer_definition{};
    def.root_view = add_layout_view(def, "root");
    auto const deep = add_traced_view(def, "deep");

    auto const inner_node = def.nodes.add_container(sv::invalid_node);
    add_leaf(def, inner_node, deep);

    auto middle = sv::view_data{};
    middle.id = sv::view_id::from_string("middle");
    middle.layers.push_back({.kind = sv::layer_kind::layout, .blend = sv::layer_blend::replace, .root_node = inner_node});
    def.views.push_back(cc::move(middle));
    auto const middle_index = sv::view_index(def.views.size() - 1);

    auto const root_node = layout_root_of(def, def.root_view);
    add_leaf(def, root_node, middle_index);

    // Out of the flow and authored last, so it draws over everything.
    auto const overlay_node
        = def.nodes.add_relative(root_node, {.position = tg::pos2f(0, 0), .size = tg::vec2f(0.5f, 0.5f)});
    auto const inset = add_traced_view(def, "inset");
    add_leaf(def, overlay_node, inset);

    auto const plan = sv::build_render_plan(def, tg::vec2i(200, 100), 0, {});
    REQUIRE(plan.validate());

    // Inside the overlay: the shallow-but-later region wins over the deeper one it covers.
    auto const covered = sv::pick_hit_region(plan.hit_regions, tg::pos2f(20, 20));
    REQUIRE(covered != sv::invalid_hit_region);
    CHECK(plan.hit_regions[covered].id == sv::view_id::from_string("inset"));

    // Outside it, the deep view is still what answers.
    auto const uncovered = sv::pick_hit_region(plan.hit_regions, tg::pos2f(180, 80));
    REQUIRE(uncovered != sv::invalid_hit_region);
    CHECK(plan.hit_regions[uncovered].id == sv::view_id::from_string("deep"));
}

TEST("sv - a point outside every region hits nothing")
{
    auto def = sv::viewer_definition{};
    def.root_view = add_layout_view(def, "root");
    auto const only = add_traced_view(def, "only");
    add_leaf(def, layout_root_of(def, def.root_view), only);

    auto const plan = sv::build_render_plan(def, tg::vec2i(64, 64), 0, {});
    CHECK(sv::pick_hit_region(plan.hit_regions, tg::pos2f(1000, 1000)) == sv::invalid_hit_region);
    CHECK(sv::pick_hit_region({}, tg::pos2f(0, 0)) == sv::invalid_hit_region);
}

TEST("sv - the zoom narrows what a leaf samples and nothing else")
{
    auto def = sv::viewer_definition{};
    def.root_view = add_layout_view(def, "root");
    auto const only = add_traced_view(def, "only");
    auto const leaf = add_leaf(def, layout_root_of(def, def.root_view), only);

    auto const unzoomed = sv::build_render_plan(def, tg::vec2i(200, 100), 0, {});
    REQUIRE(unzoomed.draws_of(1).size() == 1);
    auto const& wide = unzoomed.draws_of(1)[0];
    CHECK(wide.primary.uv.min == tg::pos2f(0, 0));
    CHECK(wide.primary.uv.max == tg::pos2f(1, 1));

    // Zoom 4x on the centre: a quarter of the source per axis, still centred.
    def.nodes[leaf].leaf.zoom = 4.0f;
    def.nodes[leaf].leaf.zoom_center = tg::pos2f(0.5f, 0.5f);

    auto const zoomed = sv::build_render_plan(def, tg::vec2i(200, 100), 0, {});
    REQUIRE(zoomed.draws_of(1).size() == 1);
    auto const& tight = zoomed.draws_of(1)[0];
    CHECK(tight.primary.uv.min == tg::pos2f(0.375f, 0.375f));
    CHECK(tight.primary.uv.max == tg::pos2f(0.625f, 0.625f));

    // The rect it lands in is untouched — a zoom magnifies the source, it does not resize the pane.
    CHECK(tight.dst_rect == wide.dst_rect);

    // And the trace is untouched: same resolution, so the converged image survives being inspected.
    REQUIRE(zoomed.traces.size() == 1);
    CHECK(zoomed.traces[0].resolution == unzoomed.traces[0].resolution);
}

TEST("sv - a zoom near an edge stays inside the source")
{
    auto def = sv::viewer_definition{};
    def.root_view = add_layout_view(def, "root");
    auto const only = add_traced_view(def, "only");
    auto const leaf = add_leaf(def, layout_root_of(def, def.root_view), only);

    // Centred hard in the corner: the window has to slide back in rather than sample outside the image.
    def.nodes[leaf].leaf.zoom = 4.0f;
    def.nodes[leaf].leaf.zoom_center = tg::pos2f(0.0f, 1.0f);

    auto const plan = sv::build_render_plan(def, tg::vec2i(128, 128), 0, {});
    auto const& uv = plan.draws_of(1)[0].primary.uv;

    CHECK(uv.min[0] == 0.0f);
    CHECK(uv.max[0] == 0.25f);
    CHECK(uv.min[1] == 0.75f);
    CHECK(uv.max[1] == 1.0f);
}

TEST("sv - a leaf that forbids zoom ignores it")
{
    auto def = sv::viewer_definition{};
    def.root_view = add_layout_view(def, "root");
    auto const only = add_traced_view(def, "only");
    auto const leaf = add_leaf(def, layout_root_of(def, def.root_view), only);

    def.nodes[leaf].leaf.allow_zoom = false;
    def.nodes[leaf].leaf.zoom = 8.0f;

    auto const plan = sv::build_render_plan(def, tg::vec2i(64, 64), 0, {});
    auto const& uv = plan.draws_of(1)[0].primary.uv;
    CHECK(uv.min == tg::pos2f(0, 0));
    CHECK(uv.max == tg::pos2f(1, 1));
}

TEST("sv - a view lifted out of the flow leaves its siblings tiling as if it were absent")
{
    // What drag-to-move produces: the leaf is re-parented under a `relative` node on the root's layout, so the two
    // views that stay behind split the whole window rather than two thirds of a three-cell row.
    auto def = sv::viewer_definition{};
    def.root_view = add_layout_view(def, "root");
    auto const root_node = layout_root_of(def, def.root_view);

    auto const a = add_traced_view(def, "a");
    auto const b = add_traced_view(def, "b");
    auto const floated = add_traced_view(def, "floated");

    add_leaf(def, root_node, a);
    add_leaf(def, root_node, b);

    // The lifted one hangs off a relative node instead of being a flowed child.
    auto const holder
        = def.nodes.add_relative(root_node, {.position = tg::pos2f(0.5f, 0.25f), .size = tg::vec2f(0.25f, 0.25f)});
    add_leaf(def, holder, floated);

    auto const plan = sv::build_render_plan(def, tg::vec2i(160, 80), 0, {});
    REQUIRE(plan.validate());

    auto const draws = plan.draws_of(u32(plan.targets.size() - 1));
    REQUIRE(draws.size() == 3);

    // The two flowed views split the window in half — the lifted one took no cell.
    CHECK(draws[0].dst_rect == rect_of(0, 0, 80, 80));
    CHECK(draws[1].dst_rect == rect_of(80, 0, 160, 80));

    // And the lifted one lands at its fraction of the window, drawn last so it is in front.
    CHECK(draws[2].dst_rect == rect_of(80, 20, 120, 40));

    // A hit inside it reaches it, not the view it covers.
    auto const hit = sv::pick_hit_region(plan.hit_regions, tg::pos2f(100, 30));
    REQUIRE(hit != sv::invalid_hit_region);
    CHECK(plan.hit_regions[hit].id == sv::view_id::from_string("floated"));
}

// Temporal resources are a plan-level fact: the plan is what sizes them, because an unset resolution means "the
// view's own" and only the measure pass knows that.
// This is also where the accumulator stops being a special case baked into the renderer — a traced layer *implies*
// a temporal input, and a caller can declare more beside it.
TEST("sv - a traced layer implies an accumulation temporal input, sized by the plan")
{
    auto def = sv::viewer_definition{};
    auto const traced = add_traced_view(def, "traced");
    auto const root = add_layout_view(def, "root");
    (void)add_leaf(def, layout_root_of(def, root), cc::span<sv::view_index const>(&traced, 1));
    def.root_view = root;

    auto const plan = sv::build_render_plan(def, tg::vec2i(128, 64), 0, {});
    REQUIRE(plan.validate());

    // One per traced layer — its accumulator — and none for the layout views, which accumulate nothing.
    REQUIRE(plan.temporals.size() == 1);
    auto const& t = plan.temporals[0];
    CHECK(t.id == sv::view_id::from_string("traced"));
    CHECK(t.temporal_id == sv::temporal_id::accumulation(0));
    CHECK(t.resolution == tg::vec2i(128, 64)); // the view's own, taken from the rect it landed in

    // Full floats, because the accumulation is uncapped: at frame n the blend weight is 1 / (n + 1), and a half
    // float stops moving the mean while the estimate is still converging.
    CHECK(t.format == sg::pixel_format::rgba32_float);

    // Outside the range a caller may declare in, so an implied id and a declared one cannot collide.
    CHECK(t.temporal_id >= sv::temporal_id::caller_range_end);
}

// A layer inserted *above* a traced one must not hand its accumulator to a different layer.
// This is the whole reason the slots are keyed by id rather than indexed by layer position: an id derived from the
// layer's own index moves with it, so the history follows the layer that owns it.
TEST("sv - a caller-declared temporal input rides alongside the implied one")
{
    auto def = sv::viewer_definition{};

    auto v = sv::view_data{};
    v.id = sv::view_id::from_string("declared");
    sv::ensure_scene_3d(v).items.push_back({.mesh = sv::mesh_id(1), .instance = sv::instance_id(1)});

    // A caller's own resource: half resolution, its own format, and its own reset rule.
    v.temporal_inputs.push_back(
        {.id = 7, .resolution = tg::vec2i(32, 16), .format = sg::pixel_format::rgba8_unorm, .reset_hash = 0xABCD});
    auto const declared = sv::view_index(def.views.size());
    def.views.push_back(cc::move(v));

    auto const root = add_layout_view(def, "root");
    (void)add_leaf(def, layout_root_of(def, root), cc::span<sv::view_index const>(&declared, 1));
    def.root_view = root;

    auto const plan = sv::build_render_plan(def, tg::vec2i(128, 64), 0, {});
    REQUIRE(plan.temporals.size() == 2); // the caller's, plus the traced layer's accumulator

    // The declaration is carried verbatim — a set resolution is NOT overwritten by the view's.
    auto const& own = plan.temporals[0];
    CHECK(own.temporal_id == 7u);
    CHECK(own.resolution == tg::vec2i(32, 16));
    CHECK(own.format == sg::pixel_format::rgba8_unorm);
    CHECK(own.reset_hash == 0xABCDu);

    // The implied one still lands, at the view's own resolution.
    CHECK(plan.temporals[1].temporal_id == sv::temporal_id::accumulation(0));
    CHECK(plan.temporals[1].resolution == tg::vec2i(128, 64));
}

// A scene layer that is authored but holds no geometry.
//
// `f.add_scene().add_light(...)` before any mesh exists is ordinary authoring — you light a view, then fill it — and
// it has to render an empty image.
// Planning a trace for it instead hands `view_renderer` a dispatch with nothing to bind, and `resolve_scene` asserts,
// which is a hard crash out of a perfectly reasonable frame.
// That is what broke every interactive manual test once the placeholder cube was dropped (59d73bfe).
TEST("sv - a scene layer with no geometry plans no trace")
{
    auto def = sv::viewer_definition{};

    auto v = sv::view_data{};
    v.id = sv::view_id::from_string("lit-but-empty");
    sv::ensure_scene_3d(v).area_lights.push_back({.center = tg::pos3f(0, 3, 0),
                                                  .half_extent_u = tg::vec3f(0.75f, 0, 0),
                                                  .half_extent_v = tg::vec3f(0, 0, 0.75f),
                                                  .emission = tg::vec3f(12, 12, 12)});
    auto const empty = sv::view_index(def.views.size());
    def.views.push_back(cc::move(v));

    auto const root = add_layout_view(def, "root");
    (void)add_leaf(def, layout_root_of(def, root), cc::span<sv::view_index const>(&empty, 1));
    def.root_view = root;

    auto const plan = sv::build_render_plan(def, tg::vec2i(64, 64), 0, {});
    REQUIRE(plan.validate());

    CHECK(plan.traces.empty());

    // And nothing is allocated for it either: the trace and its accumulator key off the same predicate, so a layer
    // that traces nothing cannot leave a pair of megabyte textures behind.
    CHECK(plan.temporals.empty());

    // The view is still reachable and still gets its own target — it renders, it just renders empty.
    CHECK(plan.reachable.size() == 2); // the view and the root
}
