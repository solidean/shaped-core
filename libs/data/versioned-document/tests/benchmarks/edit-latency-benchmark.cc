// The per-op edit latency: what one user action costs, end to end, against a document that already exists.
//
// This is the acceptance harness for the incremental edit path, and it measures a different thing from
// ../../../versioned-document-file/tests/benchmarks/document-loop-benchmark.cc.
// That one times a whole open / edit / save loop through a file, in bursts of fifty ops.
// A gizmo does not run a burst — it runs one op per frame and needs the answer before the frame ends — so the number
// that matters is a PER-OP latency distribution, with no file layer in the way.
//
// The target is [workloads](../../docs/concepts/workloads.md): a linear op touching a handful of entities, well under
// a millisecond end to end, at documents of thousands of entities.
// The write-up is ../../docs/benchmarks/edit-latency-benchmark.md.
//
// Three shapes are measured, because real editing produces the first and can produce the other two:
//   - the linear shape: each op extends the previous one, which is ordinary editing;
//   - a drag as a FAN: every frame branches from the SAME state, and only the last becomes history;
//   - the same drag CHAINED: each frame extends the previous one, and the intermediates are dropped on release.
//
// The two drag shapes look identical to a user and cost very differently, which is the point of measuring both.
//
// Run e.g.
//   uv run dev.py benchmark "bench-vdoc-edit-latency" --timeout 0

#include <clean-core/algorithm/sort.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <nexus/bench/run.hh>
#include <nexus/bench/units.hh>
#include <nexus/pgo.hh>
#include <nexus/test.hh>
#include <versioned-document/incremental_parse.hh>
#include <versioned-document/layer_stack.hh>
#include <versioned-document/op.hh>
#include <versioned-document/op_builder.hh>
#include <versioned-document/op_graph.hh>
#include <versioned-document/parse.hh>
#include <versioned-document/snapshot_cache.hh>
#include <versioned-document/snapshot_document.hh>
#include <versioned-document/value_builder.hh>

#include <chrono>

namespace vdoc_bench
{
using namespace cc::primitive_defines;

struct wall;
} // namespace vdoc_bench

/// The benchmark's one component: six properties, matching what the loop benchmark writes, so the two are comparable.
struct vdoc_bench::wall
{
    f64 x = 0;
    f64 y = 0;
    f64 z = 0;
    f64 angle = 0;
    cc::string label;
    i64 layer = 0;
};

template <>
struct vdoc::component_traits<vdoc_bench::wall>
{
    static constexpr cc::string_view type_name = "transform";
    static constexpr i32 schema_version = 1;

    static void write(vdoc_bench::wall const& c, vdoc::component_writer& w)
    {
        w.set("x", vdoc::value::of(c.x));
        w.set("y", vdoc::value::of(c.y));
        w.set("z", vdoc::value::of(c.z));
        w.set("angle", vdoc::value::of(c.angle));
        w.set("label", vdoc::value::of(cc::string_view(c.label)));
        w.set("layer", vdoc::value::of(c.layer));
    }

    static cc::optional<vdoc_bench::wall> parse(vdoc::property_reader const& r)
    {
        auto out = vdoc_bench::wall();
        if (auto const v = r.try_get("x"); v.has_value())
            out.x = v.value().as_f64();
        if (auto const v = r.try_get("y"); v.has_value())
            out.y = v.value().as_f64();
        if (auto const v = r.try_get("z"); v.has_value())
            out.z = v.value().as_f64();
        if (auto const v = r.try_get("angle"); v.has_value())
            out.angle = v.value().as_f64();
        if (auto const v = r.try_get("label"); v.has_value())
            out.label = cc::string(v.value().as_string());
        if (auto const v = r.try_get("layer"); v.has_value())
            out.layer = v.value().as_i64();
        return out;
    }
};

namespace
{
using namespace cc::primitive_defines;
using clock_type = std::chrono::steady_clock;

[[nodiscard]] double seconds_since(clock_type::time_point t0)
{
    return std::chrono::duration<double>(clock_type::now() - t0).count();
}

/// The four stages of one edit, timed inside the measured body and recorded as shares of it.
///
/// Shares rather than absolute times: the total is already the median column, so a share recovers the absolute figure
/// and answers "where did the time go" without a second time column per stage.
/// They are means over the samples, which is what a cost breakdown wants — the TAIL belongs to the total, and that is
/// what the p95 the target is written against measures.
struct stage_shares
{
    f64 build = 0;   ///< op_builder::build — the diff against the parents
    f64 add = 0;     ///< op_graph::add
    f64 advance = 0; ///< rolling the pinned snapshot onto the new head; zero where the shape cannot advance
    f64 apply = 0;   ///< the typed document at the new head, plus the change summary

    [[nodiscard]] f64 total() const { return build + add + advance + apply; }

    /// Files each stage as a fraction of this edit, on the iteration that produced them.
    void record_into(nx::bench::iteration& it) const
    {
        auto const whole = total();
        if (whole <= 0)
            return;

        it.record("build", nx::bench::unit_cost_share, build / whole);
        it.record("add", nx::bench::unit_cost_share, add / whole);
        it.record("advance", nx::bench::unit_cost_share, advance / whole);
        it.record("apply", nx::bench::unit_cost_share, apply / whole);
    }
};

[[nodiscard]] vdoc::entity_id wall_entity(isize i)
{
    return vdoc::entity_id::of(cc::format("wall-{}", i));
}

/// Adds one op straight from encoded bytes, without diffing against the graph.
///
/// Seeding a history through op_builder is quadratic — every build materializes its parents — and the seed is setup
/// rather than the thing being measured, so it goes around the builder the way the test corpus does.
[[nodiscard]] vdoc::op_id add_encoded(vdoc::op_graph& graph,
                                      cc::span<vdoc::op_id const> parents,
                                      cc::span<vdoc::assignment const> sorted_assignments)
{
    auto const metadata = vdoc::value_builder::object().build();
    auto const metadata_bytes = cc::vector<byte>::create_copy_of(metadata.bytes());
    auto const assignment_bytes = vdoc::encode_assignments(sorted_assignments);

    auto const id = vdoc::compute_op_id(parents, metadata_bytes, assignment_bytes);
    auto decoded = vdoc::try_decode_op(id, parents, metadata_bytes, assignment_bytes);
    REQUIRE(decoded.has_value());

    return graph.add(cc::move(decoded.value()));
}

/// A linear history of `count` ops, each giving one new entity the six wall properties.
///
/// Each op writes its OWN entity, so the document grows with the history — which is the shape an editing session
/// produces and the shape the loop benchmark already measures against.
[[nodiscard]] vdoc::op_id seed_linear(vdoc::op_graph& graph, isize count)
{
    auto const transform = vdoc::component_type_id::of("transform");
    auto head = vdoc::op_id();
    auto has_head = false;

    for (isize i = 0; i < count; ++i)
    {
        auto const entity = wall_entity(i);
        auto const label = cc::format("wall {}", i);

        // Owned first, because an assignment's value is a view into whatever produced it.
        auto const values = cc::vector<vdoc::value>{vdoc::value::of(f64(i)),
                                                    vdoc::value::of(f64(i) * 2),
                                                    vdoc::value::of(f64(i) * 3),
                                                    vdoc::value::of(f64(i) * 0.25),
                                                    vdoc::value::of(cc::string_view(label)),
                                                    vdoc::value::of(i64(i % 8))};
        cc::string_view const names[] = {"x", "y", "z", "angle", "label", "layer"};

        auto assignments = cc::vector<vdoc::assignment>();
        for (isize p = 0; p < 6; ++p)
            assignments.push_back(vdoc::assignment{
                .path = {.entity = entity, .component = transform, .property = vdoc::property_id::of(names[p])},
                .value = values[p]});

        cc::sort(assignments,
                 [](vdoc::assignment const& a, vdoc::assignment const& b) { return a.path.compare_bytes(b.path) < 0; });

        head = add_encoded(graph, has_head ? cc::span<vdoc::op_id const>(&head, 1) : cc::span<vdoc::op_id const>(),
                           assignments);
        has_head = true;
    }

    return head;
}

/// One edit: move one existing entity, which is the gizmo's op.
[[nodiscard]] vdoc::op build_move(vdoc::op_graph const& graph,
                                  vdoc::snapshot_cache& cache,
                                  vdoc::op_id const& parent,
                                  isize entity_index,
                                  f64 x)
{
    auto op = vdoc::op_builder();
    op.set_parents(cc::span<vdoc::op_id const>(&parent, 1));
    op.set_raw(wall_entity(entity_index), vdoc::component_type_id::of("transform"), vdoc::property_id::of("x"),
               vdoc::value::of(x));
    return op.build(graph, cache);
}

/// What a latency measurement is allowed to cost, and why it does not batch.
///
/// **`batch = false` is the whole point.** One sample has to be ONE edit, because the claim under test is about the
/// tail: a batch mean smooths away exactly the slow frame a budget is spent against.
/// A clock pair costs ~14 ns against an edit of tens of microseconds, so per-iteration timing is free here.
///
/// 5% rather than the default 2%: an edit allocates, and an allocator's own variance is not something more sampling
/// removes.
constexpr auto latency_config = nx::bench::run_config{
    .min_time_secs = 0.2,
    .max_time_secs = 2.0,
    .min_samples = 64,
    .target_relative_error = 0.05,
    .warmup_time_secs = 0.02,
    .batch = false,
    .measure_counters = false,
    .no_baseline = true,
};

/// A seeded document plus everything an edit needs, built once and then edited many times.
struct session
{
    vdoc::op_graph graph;
    vdoc::snapshot_cache cache;
    vdoc::component_registry registry;
    vdoc::default_parse_policy policy;
    vdoc::parse_report report;
    vdoc::document doc;
    vdoc::change_summary changes;
    vdoc::incremental_apply_stats stats;
    vdoc::op_id head;
    isize entities = 0;
};

/// Seeds a document of `entities` entities and pins a snapshot where an application would: right after the load.
[[nodiscard]] cc::unique_ptr<session> open_session(isize entities)
{
    auto s = cc::make_unique<session>();
    s->entities = entities;
    s->head = seed_linear(s->graph, entities);

    s->cache.install(s->head, vdoc::snapshot_document::create_owning_copy(s->graph.materialize(s->head)),
                     /*pinned =*/true);

    s->registry.register_component<vdoc_bench::wall>();

    // Deliberately NOT create_with_local_head: collecting the closure is a walk of the whole history, and a session
    // that built one per frame would pay that before anything else in the loop.
    s->policy = vdoc::default_parse_policy::create_with_registry(s->registry);

    s->doc = vdoc::parse(s->graph.materialize(s->head, s->cache), s->policy, s->report);
    CHECK(s->doc.entity_count() == entities);
    return s;
}

/// One linear edit: build, add, advance the pinned snapshot, apply.
///
/// This is the gizmo's op, and the shape ordinary editing produces.
/// The snapshot rolls onto the new head as part of the edit, so the next build's walk is one op again rather than two
/// and then three — which is the property that makes a long session flat, and the one this benchmark checks by running
/// thousands of edits where the hand-rolled version ran fifty.
stage_shares one_linear_edit(session& s, isize index)
{
    auto out = stage_shares();

    auto const t_build = clock_type::now();
    auto op = build_move(s.graph, s.cache, s.head, index % s.entities, f64(index) + 0.5);
    out.build = seconds_since(t_build);

    auto const previous = s.head;
    auto const t_add = clock_type::now();
    s.head = s.graph.add(cc::move(op));
    out.add = seconds_since(t_add);

    auto const t_advance = clock_type::now();
    auto const advanced = vdoc::advance_snapshot(s.graph, s.cache, previous, s.head);
    out.advance = seconds_since(t_advance);
    CC_ASSERT(advanced, "the snapshot must roll onto a single-parent child");

    auto const t_apply = clock_type::now();
    s.doc = vdoc::apply(cc::move(s.doc), s.graph, previous, s.head, s.policy, s.report, s.changes, {.cache = &s.cache},
                        &s.stats);
    out.apply = seconds_since(t_apply);

    CC_ASSERT(s.stats.took_fast_path, "a single-parent edit must take the incremental path");
    return out;
}

/// One whole drag, as a FAN: `frames` frames all branching from the SAME state, then all but the last thrown away.
///
/// **The iteration is the whole gesture, not one frame**, and that is what makes it repeatable: the frames it adds are
/// dropped again and the head goes back where it started, so every iteration sees exactly what the first one saw.
/// Per-frame latency comes from `it.items(frames)`.
///
/// The snapshot deliberately does not advance, because the frames are siblings of each other.
/// The apply cannot stay on its fast path either: frame k+1 does not descend from frame k, so evolving the document
/// from one frame to the next is a full re-parse however small the edit was.
stage_shares one_fanned_drag(session& s, isize frames)
{
    auto out = stage_shares();
    auto drag_frames = cc::vector<vdoc::op_id>();

    for (isize i = 0; i < frames; ++i)
    {
        auto const t_build = clock_type::now();
        auto op = build_move(s.graph, s.cache, s.head, 0, f64(i) * 0.01);
        out.build += seconds_since(t_build);

        auto const t_add = clock_type::now();
        auto const frame = s.graph.add(cc::move(op));
        out.add += seconds_since(t_add);
        drag_frames.push_back(frame);

        auto const previous = i == 0 ? s.head : drag_frames[i - 1];
        auto const t_apply = clock_type::now();
        s.doc = vdoc::apply(cc::move(s.doc), s.graph, previous, frame, s.policy, s.report, s.changes,
                            {.cache = &s.cache}, &s.stats);
        out.apply += seconds_since(t_apply);
    }

    // The drag ends.
    // Every frame is forgotten, including the last: an accepted drag would keep it, but keeping it here would make the
    // next iteration start somewhere else and measure a different thing.
    // Dropping the whole fan is also what stops a long session's graph growing with every frame ever drawn.
    for (isize i = drag_frames.size() - 1; i >= 0; --i)
        CC_ASSERT(s.graph.drop_leaf(drag_frames[i]), "a fanned frame is a leaf and must drop");

    s.doc = vdoc::parse(s.graph.materialize(s.head, s.cache), s.policy, s.report);
    return out;
}

/// The same drag, CHAINED: each frame extends the previous one.
///
/// The behaviour a user sees is identical — one history entry, produced on release — but every frame is now a
/// single-parent child, which is the shape both the snapshot and the incremental apply are built for.
/// Measuring the two side by side is the point: they look the same and cost very differently.
stage_shares one_chained_drag(session& s, isize frames)
{
    auto out = stage_shares();
    auto const base = s.head;
    auto chain = cc::vector<vdoc::op_id>();

    for (isize i = 0; i < frames; ++i)
    {
        auto const previous = chain.empty() ? base : chain.back();

        auto const t_build = clock_type::now();
        auto op = build_move(s.graph, s.cache, previous, 0, f64(i) * 0.02);
        out.build += seconds_since(t_build);

        auto const t_add = clock_type::now();
        auto const frame = s.graph.add(cc::move(op));
        out.add += seconds_since(t_add);
        chain.push_back(frame);

        auto const t_advance = clock_type::now();
        auto const advanced = vdoc::advance_snapshot(s.graph, s.cache, previous, frame);
        out.advance += seconds_since(t_advance);
        CC_ASSERT(advanced, "a chained frame is a single-parent child");

        auto const t_apply = clock_type::now();
        s.doc = vdoc::apply(cc::move(s.doc), s.graph, previous, frame, s.policy, s.report, s.changes,
                            {.cache = &s.cache}, &s.stats);
        out.apply += seconds_since(t_apply);
    }

    // Unwound newest-first, so every drop is of a leaf, and the pinned snapshot goes back to the base the drag started
    // from -- without which iteration k+1 would edit a document k drags deep.
    for (isize i = chain.size() - 1; i >= 0; --i)
        CC_ASSERT(s.graph.drop_leaf(chain[i]), "a chained frame drops newest-first");

    s.cache.install(base, vdoc::snapshot_document::create_owning_copy(s.graph.materialize(base)), /*pinned =*/true);
    s.doc = vdoc::parse(s.graph.materialize(base, s.cache), s.policy, s.report);
    s.head = base;
    return out;
}

/// Frames in one measured gesture — about a second of dragging at 60 Hz, which is a long one.
///
/// A drag is measured as a WHOLE gesture per iteration rather than a frame at a time, because a frame is not
/// repeatable on its own: the fan's frames pile up against one parent, and only the gesture ending clears them.
/// `it.items(frames)` is what turns the gesture back into a per-frame latency.
constexpr isize drag_frames_per_gesture = 60;

/// One drag shape at one document size, as one measured loop.
nx::bench::result measure_drag(session& s, isize frames, bool fanned)
{
    return nx::bench::run(fanned ? cc::string_view("drag, fanned") : cc::string_view("drag, chained"),
                          {.min_time_secs = 0.2,
                           .max_time_secs = 2.0,
                           .min_samples = 32,
                           .target_relative_error = 0.05,
                           .warmup_time_secs = 0.02,
                           .batch = false,
                           .measure_counters = false},
                          [&](nx::bench::iteration& it)
                          {
                              auto const shares = fanned ? one_fanned_drag(s, frames) : one_chained_drag(s, frames);
                              shares.record_into(it);
                              it.items(frames);
                          });
}

/// The linear shape at one document size, as one measured loop.
nx::bench::result measure_linear(isize entities)
{
    auto s = open_session(entities);
    auto index = isize(0);

    return nx::bench::run(cc::format("entities={}", entities), latency_config,
                          [&](nx::bench::iteration& it)
                          {
                              auto const shares = one_linear_edit(*s, index++);
                              shares.record_into(it);
                          });
}

/// One wall's paths and the values that do not change, hoisted out of the frame loop.
///
/// **Interning an id and formatting a string are not what this measures.**
/// A real producer holds its property ids as constants and its entity ids alongside whatever it is producing from, so
/// leaving `entity_id::of(cc::format(...))` in the loop would attribute the intern table's cost to the layer — and at
/// 8,000 entities that dwarfed everything else.
struct produced_wall
{
    cc::vector<vdoc::property_path> paths;
    cc::vector<vdoc::value> values;
};

[[nodiscard]] cc::vector<produced_wall> plan_walls(isize entities)
{
    auto const transform = vdoc::component_type_id::of("transform");
    vdoc::property_id const properties[] = {
        vdoc::property_id::of("$schema_version"),
        vdoc::property_id::of("x"),
        vdoc::property_id::of("y"),
        vdoc::property_id::of("z"),
        vdoc::property_id::of("angle"),
        vdoc::property_id::of("label"),
        vdoc::property_id::of("layer"),
    };

    auto out = cc::vector<produced_wall>();
    out.reserve(entities);

    for (isize i = 0; i < entities; ++i)
    {
        auto wall = produced_wall();
        auto const entity = wall_entity(i);

        for (auto const& p : properties)
            wall.paths.push_back({.entity = entity, .component = transform, .property = p});

        wall.values.push_back(vdoc::value::of(i64(1)));
        wall.values.push_back(vdoc::value::of(f64(i)));
        wall.values.push_back(vdoc::value::of(f64(i) * 2));
        wall.values.push_back(vdoc::value::of(f64(i) * 3));
        wall.values.push_back(vdoc::value::of(f64(i) * 0.25));
        wall.values.push_back(vdoc::value::of(cc::format("wall {}", i)));
        wall.values.push_back(vdoc::value::of(i64(i % 8)));

        out.push_back(cc::move(wall));
    }

    return out;
}

/// Writes one wall's properties into a direct layer, version stamp included.
///
/// The stamp belongs to the layer that supplies the component, which here is the base — an override layer deliberately
/// does not stamp, so the composed version stays the base's.
void produce_wall(vdoc::direct_layer& layer, produced_wall const& wall, cc::optional<vdoc::value> const& moved_x)
{
    for (isize p = 0; p < wall.paths.size(); ++p)
        layer.set(wall.paths[p], p == 1 && moved_x.has_value() ? moved_x.value() : wall.values[p]);
}

/// The per-frame cost of a three-layer stack, which is the shape layering exists for.
///
/// A computed base is rewritten wholesale every frame, a user override layer sits on top, and a forced layer above
/// that.
/// The claim under test is that a frame costs O(dirty entities x layers) rather than O(document), so the composed
/// frame is measured beside the `rebuild` it avoids and **the gap between them is what must not close**.
struct layered_fixture
{
    vdoc::component_registry registry;
    vdoc::default_parse_policy policy;
    vdoc::direct_layer base = vdoc::direct_layer("base");
    vdoc::direct_layer forced = vdoc::direct_layer("forced");
    vdoc::op_graph user;
    vdoc::op_id user_head;
    cc::vector<produced_wall> plan;
    vdoc::layer_stack stack;
    vdoc::parse_report report;
    vdoc::change_summary changes;
};

[[nodiscard]] cc::unique_ptr<layered_fixture> open_layered(isize entities)
{
    auto f = cc::make_unique<layered_fixture>();
    f->registry.register_component<vdoc_bench::wall>();
    f->policy = vdoc::default_parse_policy::create_with_registry(f->registry);
    f->plan = plan_walls(entities);

    // the base's first full production is setup rather than a measured frame
    for (auto const& wall : f->plan)
        produce_wall(f->base, wall, {});

    // a handful of user overrides, which is what a real session has: a few pinned properties, not thousands
    auto staged = vdoc::op_builder();
    for (isize i = 0; i < 8 && i < entities; ++i)
        staged.set_raw({.entity = wall_entity(i),
                        .component = vdoc::component_type_id::of("transform"),
                        .property = vdoc::property_id::of("y")},
                       vdoc::value::of(-1.0));
    f->user_head = f->user.add(staged.build(f->user));

    (void)f->stack.push_direct_layer("base", f->base);
    (void)f->stack.push_graph_layer("user", f->user, f->user_head);
    (void)f->stack.push_direct_layer("forced", f->forced);

    f->stack.rebuild(f->policy, f->report, f->changes);
    REQUIRE(f->stack.composed().entity_count() == entities);
    return f;
}

/// What a layered measurement is allowed to cost.
constexpr auto layered_config = nx::bench::run_config{
    .min_time_secs = 0.2,
    .max_time_secs = 2.0,
    .min_samples = 32,
    .target_relative_error = 0.05,
    .warmup_time_secs = 0.02,
    .batch = false,
    .measure_counters = false,
};

/// Rewrites the base wholesale, moving four entities, which is the producer's half of a frame.
void produce_frame(layered_fixture& f, isize frame)
{
    // Only a few entities actually move, which is the case the diff exists for.
    auto const moved_x = vdoc::value::of(f64(frame));

    f.base.begin_rebuild();
    for (isize i = 0; i < f.plan.size(); ++i)
        produce_wall(f.base, f.plan[i], i < 4 ? cc::optional<vdoc::value>(moved_x) : cc::optional<vdoc::value>());
    f.base.finish_rebuild();
}

struct layered_results
{
    nx::bench::result apply;
    nx::bench::result rebuild;
    nx::bench::result produce;
};

/// The three loops the layering claim is read off.
///
/// **`apply` is the baseline and `rebuild` is what it is measured against**, because that pair IS the claim: composing
/// only what moved must beat recomposing everything, and by more as the document grows.
///
/// The producer is measured beside them rather than folded into either.
/// It rewrites the whole document by construction, so a frame TOTAL is O(document) whatever composing costs — and at
/// 8,000 entities it is over 99% of the frame, which is exactly enough to hide the thing under test.
/// Comparing that total against `rebuild` says nothing about the composition, which is what a first attempt at this
/// table did.
///
/// The produce is PAUSED out of the other two, so each loop times its own stage against the same dirtied state.
layered_results measure_layered(layered_fixture& f)
{
    auto frame = isize(0);
    auto out = layered_results();

    out.apply = nx::bench::run("apply (only what moved)", layered_config,
                               [&](nx::bench::iteration& it)
                               {
                                   it.pause();
                                   produce_frame(f, frame++);
                                   it.resume();

                                   auto stats = vdoc::layered_apply_stats();
                                   f.stack.apply(f.policy, f.report, f.changes, {}, &stats);
                                   CC_ASSERT(stats.took_fast_path, "a composed frame must take the incremental path");
                               });

    out.rebuild = nx::bench::run("rebuild (from nothing)", layered_config,
                                 [&](nx::bench::iteration& it)
                                 {
                                     it.pause();
                                     produce_frame(f, frame++);

                                     // A throwaway stack, so the two are comparable per frame.
                                     auto fresh = vdoc::layer_stack();
                                     (void)fresh.push_direct_layer("base", f.base);
                                     (void)fresh.push_graph_layer("user", f.user, f.user_head);
                                     (void)fresh.push_direct_layer("forced", f.forced);
                                     auto fresh_report = vdoc::parse_report();
                                     it.resume();

                                     fresh.rebuild(f.policy, fresh_report, f.changes);
                                 });

    // The producer's own cost, which a frame pays on top of whichever of the two above it uses.
    // Here so the frame total is recoverable, and because it is what actually dominates a frame today.
    out.produce = nx::bench::run("produce (rewrites everything)", layered_config, [&] { produce_frame(f, frame++); });
    return out;
}

} // namespace

// The representative size, recorded: an editing session's worth of history, one op at a time.
//
// p95 rather than the median, because the target is a claim about the tail.
// The harness reports it only where a sample is one iteration, which is why these run unbatched.
PGO_BENCHMARK("bench-vdoc-edit-latency (one op at a time)")
{
    auto const linear = measure_linear(2000);
    nx::pgo::report_time_for("edit-p50", linear.time.median);
    nx::pgo::report_time_for("edit-p95", linear.time.p95);
    nx::pgo::report_time_for("edit-max", linear.time.max);

    auto s = open_session(2000);
    auto const fanned = measure_drag(*s, drag_frames_per_gesture, /*fanned =*/true);
    auto const chained = measure_drag(*s, drag_frames_per_gesture, /*fanned =*/false);
    nx::pgo::report_time_for("drag-fanned-frame-p95", fanned.time.p95 / f64(drag_frames_per_gesture));
    nx::pgo::report_time_for("drag-chained-frame-p95", chained.time.p95 / f64(drag_frames_per_gesture));
}

// The human-facing sweep the write-up analyses.
//
// The sizes are held down by what is affordable TODAY rather than by what is interesting: op_builder::build walks the
// whole history on every call, so seeding aside, the sweep itself is quadratic until that is fixed.
// They go up as the stages land.
BENCHMARK("bench-vdoc-edit-latency - one linear edit")
{
    (void)measure_linear(500);
    (void)measure_linear(2000);
    (void)measure_linear(8000);
}

// The two drag shapes look identical to a user and cost very differently, which is why they share a table: the
// comparison column is exactly the price of fanning.
BENCHMARK("bench-vdoc-edit-latency - a drag, chained against fanned")
{
    auto s = open_session(2000);
    (void)measure_drag(*s, drag_frames_per_gesture, /*fanned =*/false);
    (void)measure_drag(*s, drag_frames_per_gesture, /*fanned =*/true);
}

// The layered per-frame shape, recorded: the three-layer stack layering exists for.
PGO_BENCHMARK("bench-vdoc-layered-frame (three layers, per frame)")
{
    auto f = open_layered(2000);
    auto const r = measure_layered(*f);

    nx::pgo::report_time_for("layered-frame-p95", r.produce.time.p95 + r.apply.time.p95);
    nx::pgo::report_time_for("layered-apply-p95", r.apply.time.p95);
    nx::pgo::report_time_for("layered-rebuild-p95", r.rebuild.time.p95);
}

// The tables that say whether composing is O(dirty) or O(document).
//
// `apply` must stay roughly flat across these sizes while `rebuild` grows with them, so `rebuild`'s comparison column
// must get LARGER as the document does.
// If it shrinks towards `~same`, the composition has started walking the whole document, and nothing else would notice.
BENCHMARK("bench-vdoc-layered-frame - compose against rebuild, 500 entities")
{
    auto f = open_layered(500);
    (void)measure_layered(*f);
}

BENCHMARK("bench-vdoc-layered-frame - compose against rebuild, 2000 entities")
{
    auto f = open_layered(2000);
    (void)measure_layered(*f);
}

BENCHMARK("bench-vdoc-layered-frame - compose against rebuild, 8000 entities")
{
    auto f = open_layered(8000);
    (void)measure_layered(*f);
}
