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
// The medium comes from the conformance fixture, so "open" here is the same open the suite tests.
// Run e.g.
//   uv run dev.py test "bench-vdoc-loop" --target versioned-document-file-test --preset release-clang --timeout 0

#include "../conformance/store_fixture.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/string/format.hh>
#include <nexus/pgo.hh>
#include <nexus/test.hh>
#include <versioned-document/op.hh>
#include <versioned-document/op_builder.hh>
#include <versioned-document/snapshot_cache.hh>
#include <versioned-document/snapshot_document.hh>

#include <chrono>
#include <cstdio>

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

[[nodiscard]] double median_of(cc::vector<double> values)
{
    // Only the middle element has to end up where a full sort would put it, which sort_at does in O(n).
    cc::sort_at(values, values.size() / 2);
    return values[values.size() / 2];
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

struct loop_measurement
{
    isize ops = 0;
    isize properties = 0;
    isize payload_bytes = 0;
    stage_times times;
    double hashing_seconds = 0;
    double additivity_delta = 0;
};

/// Seeds a document of `ops` ops, then measures the loop over it.
[[nodiscard]] loop_measurement measure(store_impl const& impl, isize ops, isize edits, bool record)
{
    // The history is built ONCE, because building it is the expensive part and it is not what is being measured.
    auto seed = vdoc::op_graph();
    auto const seeded = extend(seed, {}, 0, ops);
    auto const head = seeded.back();

    // **Every pass gets its own medium, seeded identically.**
    // A pass appends its edits, and the loop is superlinear in document size — so reusing one medium would make each
    // pass slower than the last, and any delta between two series would measure that growth instead of the injection.
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

    auto opens = cc::vector<double>();
    auto materializes = cc::vector<double>();
    auto edits_s = cc::vector<double>();
    auto publishes = cc::vector<double>();
    auto closes = cc::vector<double>();
    auto totals = cc::vector<double>();

    auto loaded = isize(0);
    for (isize run = 0; run < 5; ++run)
    {
        auto const medium = seeded_medium();
        auto const times = run_loop(*medium, edits, 0, loaded);
        opens.push_back(times.open);
        materializes.push_back(times.materialize);
        edits_s.push_back(times.edit);
        publishes.push_back(times.publish);
        closes.push_back(times.close);
        totals.push_back(times.total());
    }

    out.times = stage_times{.open = median_of(opens),
                            .materialize = median_of(materializes),
                            .edit = median_of(edits_s),
                            .publish = median_of(publishes),
                            .close = median_of(closes)};
    auto const measured_total = median_of(totals);

    out.hashing_seconds = seconds_hashing(seed, seeded, edits);

    // The control: the same loop, with every loaded op hashed a second time.
    // If the loop grows by what that hashing costs alone, the isolated figure composes — and the shares below are a
    // measurement rather than a ratio of two unrelated numbers.
    auto injected = cc::vector<double>();
    for (isize run = 0; run < 3; ++run)
    {
        auto const medium = seeded_medium();
        injected.push_back(run_loop(*medium, edits, 1, loaded).total());
    }

    out.additivity_delta = median_of(injected) - measured_total;

    // Two shares, because only the second is a number worth watching.
    // Against the whole loop hashing is invisible; against the OPEN — where the loader re-hashes every op it reads —
    // it is a real fraction, and it is the one that grows with history length.
    auto const share_of_loop = 100.0 * out.hashing_seconds / measured_total;
    auto const share_of_open = 100.0 * out.hashing_seconds / out.times.open;

    std::printf("%-10s ops=%5lld props=%6lld payload=%4lld B | loop %7.1f ms (open %6.1f, materialize %5.1f, edit "
                "%7.1f, publish %6.1f, close %5.1f) | hashing %5.2f ms = %4.1f%% of loop, %4.1f%% of open | injected "
                "+%5.2f ms\n",
                impl.name.data(), (long long)out.ops, (long long)out.properties, (long long)out.payload_bytes,
                measured_total * 1000, out.times.open * 1000, out.times.materialize * 1000, out.times.edit * 1000,
                out.times.publish * 1000, out.times.close * 1000, out.hashing_seconds * 1000, share_of_loop,
                share_of_open, out.additivity_delta * 1000);

    if (record)
    {
        nx::pgo::report_time_for("loop-total", measured_total);
        nx::pgo::report_time_for("loop-open", out.times.open);
        nx::pgo::report_time_for("loop-publish", out.times.publish);
        nx::pgo::report_time_for("op-hashing-total", out.hashing_seconds);
        nx::pgo::report_raw("hash-share-of-loop", share_of_loop, "%", /*higher_is_better =*/false);
        nx::pgo::report_raw("hash-share-of-open", share_of_open, "%", /*higher_is_better =*/false);
    }

    return out;
}
} // namespace

// The representative size, recorded: an editing session's worth of history, saved once more.
PGO_BENCHMARK("bench-vdoc-loop (open / edit / publish / close)")
{
    auto const impl = sqlite_impl();
    if (!impl.is_available())
    {
        std::printf("SQLite was not compiled in, so the loop cannot be measured against a file\n");
        return;
    }

    (void)measure(impl, 2000, 50, /*record =*/true);
}

// The human-facing sweep the write-up analyses: three document sizes, on both arms.
TEST("bench-vdoc-loop (full sweep)", nx::config::manual)
{
    isize const sizes[] = {200, 2000, 8000};

    for (auto const& impl : {in_memory_impl(), sqlite_impl()})
    {
        if (!impl.is_available())
            continue;

        for (auto const ops : sizes)
            (void)measure(impl, ops, 50, /*record =*/false);
    }
}
