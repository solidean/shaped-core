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
//   uv run dev.py test "bench-vdoc-edit-latency (full sweep)" --preset release-clang --timeout 0 --manual

#include <clean-core/algorithm/sort.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <nexus/guide.hh>
#include <nexus/test.hh>
#include <versioned-document/incremental_parse.hh>
#include <versioned-document/op.hh>
#include <versioned-document/op_builder.hh>
#include <versioned-document/op_graph.hh>
#include <versioned-document/parse.hh>
#include <versioned-document/snapshot_cache.hh>
#include <versioned-document/snapshot_document.hh>
#include <versioned-document/value_builder.hh>

#include <chrono>
#include <cstdio>

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

/// One stage's per-op timings.
///
/// A latency target is a claim about the tail, so a median alone cannot check it — p95 and the worst sample are what
/// a frame budget is actually spent against.
struct stage_samples
{
    cc::vector<double> seconds;

    void add(double s) { seconds.push_back(s); }

    [[nodiscard]] double quantile(double q) const
    {
        auto sorted = cc::vector<double>::create_copy_of(seconds);
        cc::sort(sorted);
        auto const at = isize(q * double(sorted.size() - 1) + 0.5);
        return sorted[at];
    }

    [[nodiscard]] double p50() const { return quantile(0.5); }
    [[nodiscard]] double p95() const { return quantile(0.95); }
    [[nodiscard]] double worst() const { return quantile(1.0); }
};

/// The whole per-op loop, one entry per measured stage.
struct edit_samples
{
    stage_samples build;   ///< op_builder::build — the diff against the parents
    stage_samples add;     ///< op_graph::add
    stage_samples advance; ///< rolling the pinned snapshot onto the new head; empty where the shape cannot advance
    stage_samples apply;   ///< the typed document at the new head, plus the change summary

    /// What an application actually waits for: everything above, per op.
    [[nodiscard]] stage_samples total() const
    {
        auto out = stage_samples();
        for (isize i = 0; i < build.seconds.size(); ++i)
            out.add(build.seconds[i] + add.seconds[i] + apply.seconds[i]
                    + (advance.seconds.empty() ? 0.0 : advance.seconds[i]));
        return out;
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

void print_stage(char const* name, stage_samples const& s)
{
    std::printf("    %-12s p50 %8.3f ms   p95 %8.3f ms   max %8.3f ms\n", name, s.p50() * 1000, s.p95() * 1000,
                s.worst() * 1000);
}

struct measurement
{
    isize entities = 0;
    isize samples = 0;
    edit_samples linear;
    edit_samples drag_fan;
    edit_samples drag_chain;
    isize resident_ops_after_drag = 0;
};

/// Seeds a document of `entities` entities, then times `samples` single-entity edits in each shape.
[[nodiscard]] measurement measure(isize entities, isize samples, bool record)
{
    auto out = measurement{.entities = entities, .samples = samples};

    auto graph = vdoc::op_graph();
    auto const base = seed_linear(graph, entities);

    // The snapshot an application pins where it knows the good place is — immediately after the load.
    auto cache = vdoc::snapshot_cache();
    cache.install(base, vdoc::snapshot_document::create_owning_copy(graph.materialize(base)), /*pinned =*/true);

    auto registry = vdoc::component_registry();
    registry.register_component<vdoc_bench::wall>();

    // Deliberately NOT create_with_local_head: collecting the closure is a walk of the whole history, and a session
    // that built one per frame would pay that before anything else in the loop.
    auto const policy = vdoc::default_parse_policy::create_with_registry(registry);

    auto report = vdoc::parse_report();
    auto doc = vdoc::parse(graph.materialize(base, cache), policy, report);
    CHECK(doc.entity_count() == entities);

    auto changes = vdoc::change_summary();
    auto stats = vdoc::incremental_apply_stats();

    // ---- the linear shape: each op extends the last ----------------------------------------------------------------
    auto head = base;
    for (isize i = 0; i < samples; ++i)
    {
        auto const t_build = clock_type::now();
        auto op = build_move(graph, cache, head, i % entities, f64(i) + 0.5);
        out.linear.build.add(seconds_since(t_build));

        auto const previous = head;
        auto const t_add = clock_type::now();
        head = graph.add(cc::move(op));
        out.linear.add.add(seconds_since(t_add));

        // The op is accepted as history the moment it is added here, so the snapshot rolls onto it and the next
        // build's walk is one op again rather than two, then three.
        auto const t_advance = clock_type::now();
        auto const advanced = vdoc::advance_snapshot(graph, cache, previous, head);
        out.linear.advance.add(seconds_since(t_advance));
        CHECK(advanced);

        auto const t_apply = clock_type::now();
        doc = vdoc::apply(cc::move(doc), graph, previous, head, policy, report, changes, {.cache = &cache}, &stats);
        out.linear.apply.add(seconds_since(t_apply));

        CHECK(stats.took_fast_path);
        CHECK(doc.entity_count() == entities);
    }

    // ---- the drag, as a FAN: every frame branches from the same state -----------------------------------------------
    //
    // The snapshot deliberately does not advance here, because the frames are siblings of each other.
    // The apply cannot stay on its fast path either: frame k+1 does not descend from frame k, so evolving the document
    // from one frame to the next is a full re-parse however small the edit was.
    auto const before_drag = graph.size();
    auto drag_frames = cc::vector<vdoc::op_id>();
    for (isize i = 0; i < samples; ++i)
    {
        auto const t_build = clock_type::now();
        auto op = build_move(graph, cache, head, 0, f64(i) * 0.01);
        out.drag_fan.build.add(seconds_since(t_build));

        auto const t_add = clock_type::now();
        auto const frame = graph.add(cc::move(op));
        out.drag_fan.add.add(seconds_since(t_add));
        drag_frames.push_back(frame);

        auto const previous = i == 0 ? head : drag_frames[i - 1];
        auto const t_apply = clock_type::now();
        doc = vdoc::apply(cc::move(doc), graph, previous, frame, policy, report, changes, {.cache = &cache}, &stats);
        out.drag_fan.apply.add(seconds_since(t_apply));

        CHECK(doc.entity_count() == entities);
    }

    // The drag ends: the last frame is accepted as history and the rest are forgotten, which is the only thing that
    // stops a long session's graph growing with every frame ever drawn.
    for (isize i = 0; i + 1 < drag_frames.size(); ++i)
        CHECK(graph.drop_leaf(drag_frames[i]));

    out.resident_ops_after_drag = graph.size() - before_drag;
    head = drag_frames.back();
    cache.install(head, vdoc::snapshot_document::create_owning_copy(graph.materialize(head)), /*pinned =*/true);

    // ---- the same drag, CHAINED: each frame extends the previous one ------------------------------------------------
    //
    // The behaviour a user sees is identical — one history entry, produced on release — but every frame is now a
    // single-parent child, which is the shape both the snapshot and the incremental apply are built for.
    auto chain_frames = cc::vector<vdoc::op_id>();
    for (isize i = 0; i < samples; ++i)
    {
        auto const previous = chain_frames.empty() ? head : chain_frames.back();

        auto const t_build = clock_type::now();
        auto op = build_move(graph, cache, previous, 0, f64(i) * 0.02);
        out.drag_chain.build.add(seconds_since(t_build));

        auto const t_add = clock_type::now();
        auto const frame = graph.add(cc::move(op));
        out.drag_chain.add.add(seconds_since(t_add));
        chain_frames.push_back(frame);

        auto const t_advance = clock_type::now();
        auto const advanced = vdoc::advance_snapshot(graph, cache, previous, frame);
        out.drag_chain.advance.add(seconds_since(t_advance));
        CHECK(advanced);

        auto const t_apply = clock_type::now();
        doc = vdoc::apply(cc::move(doc), graph, previous, frame, policy, report, changes, {.cache = &cache}, &stats);
        out.drag_chain.apply.add(seconds_since(t_apply));

        CHECK(stats.took_fast_path);
    }

    auto const linear_total = out.linear.total();
    auto const fan_total = out.drag_fan.total();
    auto const chain_total = out.drag_chain.total();

    std::printf("entities=%6lld samples=%4lld\n", (long long)entities, (long long)samples);
    std::printf("  linear\n");
    print_stage("build", out.linear.build);
    print_stage("add", out.linear.add);
    print_stage("advance", out.linear.advance);
    print_stage("apply", out.linear.apply);
    print_stage("TOTAL", linear_total);
    std::printf("  drag as a fan (%lld frames off one parent, %lld resident once the discarded ones are dropped)\n",
                (long long)samples, (long long)out.resident_ops_after_drag);
    print_stage("build", out.drag_fan.build);
    print_stage("apply", out.drag_fan.apply);
    print_stage("TOTAL", fan_total);
    std::printf("  drag as a chain (same behaviour to a user, single-parent edges)\n");
    print_stage("build", out.drag_chain.build);
    print_stage("advance", out.drag_chain.advance);
    print_stage("apply", out.drag_chain.apply);
    print_stage("TOTAL", chain_total);

    if (record)
    {
        nx::guide::report_time_for("edit-p50", linear_total.p50());
        nx::guide::report_time_for("edit-p95", linear_total.p95());
        nx::guide::report_time_for("edit-max", linear_total.worst());
        nx::guide::report_time_for("edit-build-p95", out.linear.build.p95());
        nx::guide::report_time_for("edit-advance-p95", out.linear.advance.p95());
        nx::guide::report_time_for("edit-apply-p95", out.linear.apply.p95());
        nx::guide::report_time_for("drag-chained-frame-p95", chain_total.p95());
        nx::guide::report_time_for("drag-fanned-frame-p95", fan_total.p95());
    }

    return out;
}
} // namespace

// The representative size, recorded: an editing session's worth of history, one op at a time.
GUIDE_BENCHMARK("bench-vdoc-edit-latency (one op at a time)")
{
    (void)measure(2000, 50, /*record =*/true);
}

// The human-facing sweep the write-up analyses.
//
// The sizes are held down by what is affordable TODAY rather than by what is interesting: op_builder::build walks the
// whole history on every call, so seeding aside, the sweep itself is quadratic until that is fixed.
// They go up as the stages land.
TEST("bench-vdoc-edit-latency (full sweep)", nx::config::manual)
{
    (void)measure(500, 100, /*record =*/false);
    (void)measure(2000, 50, /*record =*/false);
    (void)measure(8000, 20, /*record =*/false);
}
