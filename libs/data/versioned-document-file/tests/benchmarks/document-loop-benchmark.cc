// The ordinary loop, end to end: open a .vdoc, look at it, make an edit, save, close.
//
// This exists to settle one standing question rather than to optimize anything.
// libs/data/versioned-document/docs/decisions.md accepts BLAKE3 with a reservation whose reopen condition is
// "BLAKE3 shows up in a profile of an ordinary open / edit / save loop", and milestone 6 is the first point at which
// such a loop exists to measure.
// The write-up is ../../docs/benchmarks/document-loop-benchmark.md.
//
// Hashing is not separable by a hardware counter — on Windows they are read at context switches, so attributing one to
// a call inside the loop would be a guess dressed as a measurement.
// So it is measured three ways instead, none of which is an attribution:
//   1. the loop itself, per stage;
//   2. the same hashes the loop performs, run alone over the same bytes;
//   3. an additivity check — the loop again with that many EXTRA hashes injected.
// If (3) grows by (2), the isolated number composes and the share is real rather than arithmetic.
//
// As measured, (3) reads `~same` as (1): a whole extra hash round over every loaded op sits inside the loop's own
// variance.
// That is not the control failing — it IS the answer to the standing question, since a cost that cannot be seen after
// being DOUBLED is not one a profile of the loop would show either.
//
// The medium comes from the conformance fixture, so "open" here is the same open the suite tests.
// Each size and medium is its own table of three loops, and **the additivity check IS the comparison column**: the
// injected row's percentage over the plain one is what the isolated hashing figure has to account for.
//
// Run e.g.
//   uv run dev.py benchmark "bench-vdoc-loop" --timeout 0

#include "../conformance/store_fixture.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/string/format.hh>
#include <nexus/bench/run.hh>
#include <nexus/bench/units.hh>
#include <nexus/pgo.hh>
#include <nexus/test.hh>
#include <versioned-document/op.hh>
#include <versioned-document/op_builder.hh>
#include <versioned-document/snapshot_cache.hh>
#include <versioned-document/snapshot_document.hh>

#include <chrono>

using namespace cc::primitive_defines;
using namespace vdoc::file;
using namespace vdoc::file::test;

namespace
{
using clock_type = std::chrono::steady_clock;

[[nodiscard]] double seconds_since(clock_type::time_point t0)
{
    return std::chrono::duration<double>(clock_type::now() - t0).count();
}

/// A document of a realistic editing session: `ops` ops, each giving one new entity a handful of properties.
///
/// Each op writes to its OWN entity, because op_builder diffs against its parents — rewriting one entity would emit
/// nothing and measure a loop over an empty document.
///
/// `cache` is optional: an application that has just opened a file has a snapshot at its head, and passing one is what
/// lets each build's diff terminate there instead of replaying the whole history.
[[nodiscard]] cc::vector<vdoc::op_id> extend(vdoc::op_graph& graph,
                                             cc::optional<vdoc::op_id> from,
                                             isize first,
                                             isize count,
                                             vdoc::snapshot_cache* cache = nullptr)
{
    auto head = from;
    auto added = cc::vector<vdoc::op_id>();
    auto const transform = vdoc::component_type_id::of("transform");

    for (isize i = 0; i < count; ++i)
    {
        auto op = vdoc::op_builder();
        if (head.has_value())
            op.set_parents(cc::span<vdoc::op_id const>(&head.value(), 1));

        auto const entity = vdoc::entity_id::of(cc::format("wall-{}", first + i));
        op.set_raw(entity, transform, vdoc::property_id::of("x"), vdoc::value::of(f64(i)));
        op.set_raw(entity, transform, vdoc::property_id::of("y"), vdoc::value::of(f64(i) * 2));
        op.set_raw(entity, transform, vdoc::property_id::of("z"), vdoc::value::of(f64(i) * 3));
        op.set_raw(entity, transform, vdoc::property_id::of("angle"), vdoc::value::of(f64(i) * 0.25));
        op.set_raw(entity, transform, vdoc::property_id::of("label"), vdoc::value::of(cc::format("wall {}", first + i)));
        op.set_raw(entity, transform, vdoc::property_id::of("layer"), vdoc::value::of(i64(i % 8)));

        head = graph.add(cache == nullptr ? op.build(graph) : op.build(graph, *cache));
        added.push_back(head.value());
    }

    return added;
}

/// The mean payload an op in this document carries, which is the size hashing is actually argued about at.
[[nodiscard]] isize mean_payload_bytes(vdoc::op_graph const& graph, cc::span<vdoc::op_id const> ids)
{
    auto total = isize(0);
    for (auto const& id : ids)
    {
        auto const& payload = graph.find(id)->payload.value();
        total += payload.metadata_bytes.size() + payload.assignment_bytes.size();
    }
    return total / ids.size();
}

struct stage_times
{
    double open = 0;
    double materialize = 0;
    double edit = 0;
    double publish = 0;
    double close = 0;

    [[nodiscard]] double total() const { return open + materialize + edit + publish + close; }
};

/// One pass of the loop, against a file that already exists.
///
/// `extra_hashes` re-hashes every loaded op that many times over, which is the additivity control: injecting work the
/// loop already does is the one way to find out whether the isolated measurement composes.
[[nodiscard]] stage_times run_loop(store_medium& medium, isize edits, isize extra_hashes, isize& ops_loaded)
{
    auto times = stage_times();

    auto const t_open = clock_type::now();
    auto opened = medium.open();
    REQUIRE(opened.has_value());
    auto const file = cc::move(opened.value());
    times.open = seconds_since(t_open);

    ops_loaded = file->ops().size();
    auto const head = file->refs().get(cc::string_view("main"));

    auto const t_materialize = clock_type::now();
    auto const raw = file->ops().materialize(head, file->snapshot_cache());
    times.materialize = seconds_since(t_materialize);
    CHECK(raw.property_count() > 0);

    // Collected ONCE and reused below, so the injected rounds add hashing and nothing else.
    // Walking the DAG a second time would cost far more than the hashes and make the control measure the walk.
    auto resident = cc::vector<vdoc::op const*>();
    for (auto const& id : file->ops().collect_reachable(cc::span<vdoc::op_id const>(&head, 1)))
        resident.push_back(file->ops().find(id));

    for (isize round = 0; round < extra_hashes; ++round)
        for (auto const* const op : resident)
        {
            auto const& payload = op->payload.value();
            CHECK(vdoc::compute_op_id(op->parents, payload.metadata_bytes, payload.assignment_bytes) == op->id);
        }

    // The edit: one user action's worth of ops, built and added but not yet saved.
    auto graph = vdoc::op_graph();
    for (auto const* const op : resident)
        (void)graph.add(*op);

    // What an application has after an open: a snapshot at the head it just materialized, so the builder's diff
    // terminates there rather than replaying the history.
    // Untimed, because it is the reuse of a materialization the loop has already paid for above.
    auto edit_cache = vdoc::snapshot_cache();
    edit_cache.install(head, vdoc::snapshot_document::create_owning_copy(raw), /*pinned =*/true);

    auto const t_edit = clock_type::now();
    auto const fresh = extend(graph, head, 1'000'000, edits, &edit_cache);
    times.edit = seconds_since(t_edit);

    // Only the NEW ops are handed over: the rest are already in the store, and re-offering them would time a copy the
    // publish itself does not need.
    auto const t_publish = clock_type::now();
    copy_ops_into(*file, graph, fresh);
    auto const published = wait_for(*file, file->publish({.refs = {{cc::string("main"), fresh.back()}}}));
    times.publish = seconds_since(t_publish);
    REQUIRE(published.has_value());

    auto const t_close = clock_type::now();
    file->close();
    times.close = seconds_since(t_close);

    return times;
}

/// Every hash the loop performs, run alone over the same bytes.
///
/// The loader re-hashes every op it reads and the builder stamps one per new op, so that is the count — and it is the
/// loader's share that dominates, which is the honest thing to report about an OPEN.
[[nodiscard]] double seconds_hashing(vdoc::op_graph const& graph, cc::span<vdoc::op_id const> ids, isize edits)
{
    auto const t0 = clock_type::now();
    auto matched = isize(0);

    for (auto const& id : ids)
    {
        auto const* const op = graph.find(id);
        auto const& payload = op->payload.value();
        matched += vdoc::compute_op_id(op->parents, payload.metadata_bytes, payload.assignment_bytes) == id ? 1 : 0;
    }

    for (isize i = 0; i < edits; ++i)
    {
        auto const* const op = graph.find(ids[i % ids.size()]);
        auto const& payload = op->payload.value();
        matched += vdoc::compute_op_id(op->parents, payload.metadata_bytes, payload.assignment_bytes) == op->id ? 1 : 0;
    }

    auto const seconds = seconds_since(t0);
    CHECK(matched == ids.size() + edits);
    return seconds;
}

/// What one loop measurement is allowed to cost.
///
/// Unbatched, because a pass is milliseconds and each one needs its own freshly seeded medium: batching would time
/// several passes as one span and there is nothing to amortize a clock over anyway.
/// 5% rather than 2% is what a measurement crossing a filesystem can honestly claim.
constexpr auto loop_config = nx::bench::run_config{
    .min_time_secs = 0.3,
    .max_time_secs = 3.0,
    .min_samples = 8,
    .target_relative_error = 0.05,
    .warmup_time_secs = 0.05,
    .batch = false,
    .measure_counters = false,
};

/// Files each stage of a pass as a fraction of it.
///
/// Shares rather than absolute times: the pass total is already the median column, so a share recovers the absolute
/// figure and puts five stages in five narrow columns instead of five wide ones.
void record_stages(nx::bench::iteration& it, stage_times const& times)
{
    auto const whole = times.total();
    if (whole <= 0)
        return;

    it.record("open", nx::bench::unit_cost_share, times.open / whole);
    it.record("materialize", nx::bench::unit_cost_share, times.materialize / whole);
    it.record("edit", nx::bench::unit_cost_share, times.edit / whole);
    it.record("publish", nx::bench::unit_cost_share, times.publish / whole);
    it.record("close", nx::bench::unit_cost_share, times.close / whole);
}

struct loop_measurement
{
    isize ops = 0;
    isize properties = 0;
    isize payload_bytes = 0;
    nx::bench::result loop;
    nx::bench::result injected;
    nx::bench::result hashing;
};

/// Seeds a document of `ops` ops, then measures the loop over it three ways.
///
/// The three rows are the three ways the header names, and they are in one table on purpose:
///
///   * `loop` is the baseline.
///   * `loop + one extra hash round` re-hashes every loaded op once more, so its comparison column is the cost of
///     exactly the work the loop already does — the additivity control.
///   * `hashing alone` runs those same hashes over the same bytes with no loop around them.
///
/// If the second row's excess over the first matches the third row's median, the isolated figure composes and the
/// share is a measurement rather than a ratio of two unrelated numbers.
[[nodiscard]] loop_measurement measure(store_impl const& impl, cc::string_view label, isize ops, isize edits)
{
    // The history is built ONCE, because building it is the expensive part and it is not what is being measured.
    auto seed = vdoc::op_graph();
    auto const seeded = extend(seed, {}, 0, ops);
    auto const head = seeded.back();

    // **Every pass gets its own medium, seeded identically.**
    // A pass appends its edits, and the loop is superlinear in document size — so reusing one medium would make each
    // pass slower than the last, and any delta between two series would measure that growth instead of the injection.
    // It is PAUSED out of every pass below, which is the only reason a per-pass reseed is affordable at all.
    auto const seeded_medium = [&]
    {
        auto medium = impl.make_medium();
        auto opened = medium->open();
        REQUIRE(opened.has_value());
        auto const file = cc::move(opened.value());
        copy_ops_into(*file, seed, seeded);
        auto const published = wait_for(*file, file->publish({.refs = {{cc::string("main"), head}}}));
        REQUIRE(published.has_value());
        file->close();
        return medium;
    };

    auto out = loop_measurement();
    out.ops = ops;
    out.properties = seed.materialize(head).property_count();
    out.payload_bytes = mean_payload_bytes(seed, seeded);

    auto loaded = isize(0);
    auto const measure_pass = [&](cc::string_view name, isize extra_hashes)
    {
        return nx::bench::run(name, loop_config,
                              [&](nx::bench::iteration& it)
                              {
                                  it.pause();
                                  auto const medium = seeded_medium();
                                  it.resume();

                                  auto const times = run_loop(*medium, edits, extra_hashes, loaded);
                                  record_stages(it, times);
                              });
    };

    out.loop = measure_pass(cc::format("{} loop", label), 0);
    out.injected = measure_pass(cc::format("{} loop + one extra hash round", label), 1);

    out.hashing = nx::bench::run(cc::format("{} hashing alone", label), loop_config,
                                 [&] { nx::bench::sink(seconds_hashing(seed, seeded, edits)); });

    return out;
}

} // namespace

// The representative size, recorded: an editing session's worth of history, saved once more.
PGO_BENCHMARK("bench-vdoc-loop (open / edit / publish / close)")
{
    auto const impl = sqlite_impl();
    if (!impl.is_available())
        return; // SQLite was not compiled in, so there is no file to measure a loop against

    auto const m = measure(impl, "sqlite", 2000, 50);

    nx::pgo::report_time_for("loop-total", m.loop.time.median);
    nx::pgo::report_time_for("op-hashing-total", m.hashing.time.median);

    // Two shares, because only the second is a number worth watching.
    // Against the whole loop hashing is invisible; against the OPEN — where the loader re-hashes every op it reads —
    // it is a real fraction, and it is the one that grows with history length.
    auto const* const open_share = m.loop.find_quantity("open");
    auto const share_of_loop = m.loop.time.median > 0 ? m.hashing.time.median / m.loop.time.median : 0.0;
    auto const open_secs = open_share != nullptr ? open_share->per_iteration * m.loop.time.median : 0.0;

    nx::pgo::report_time_for("loop-open", open_secs);
    nx::pgo::report("hash-share-of-loop", share_of_loop, nx::bench::unit_cost_share);
    if (open_secs > 0)
        nx::pgo::report("hash-share-of-open", m.hashing.time.median / open_secs, nx::bench::unit_cost_share);
}

// The human-facing sweep the write-up analyses: three document sizes, on both arms.
//
// One benchmark per (medium, size) rather than one table of all six.
// The comparison column is the injected hashing against the plain loop, and that only means something within one
// medium at one size — a 200-op in-memory row against an 8,000-op SQLite one compares two different questions.
BENCHMARK("bench-vdoc-loop - in-memory, 200 ops")
{
    auto const impl = in_memory_impl();
    if (impl.is_available())
        (void)measure(impl, "200 ops", 200, 50);
}

BENCHMARK("bench-vdoc-loop - in-memory, 2000 ops")
{
    auto const impl = in_memory_impl();
    if (impl.is_available())
        (void)measure(impl, "2000 ops", 2000, 50);
}

BENCHMARK("bench-vdoc-loop - in-memory, 8000 ops")
{
    auto const impl = in_memory_impl();
    if (impl.is_available())
        (void)measure(impl, "8000 ops", 8000, 50);
}

BENCHMARK("bench-vdoc-loop - sqlite, 200 ops")
{
    auto const impl = sqlite_impl();
    if (impl.is_available())
        (void)measure(impl, "200 ops", 200, 50);
}

BENCHMARK("bench-vdoc-loop - sqlite, 2000 ops")
{
    auto const impl = sqlite_impl();
    if (impl.is_available())
        (void)measure(impl, "2000 ops", 2000, 50);
}

BENCHMARK("bench-vdoc-loop - sqlite, 8000 ops")
{
    auto const impl = sqlite_impl();
    if (impl.is_available())
        (void)measure(impl, "8000 ops", 8000, 50);
}
