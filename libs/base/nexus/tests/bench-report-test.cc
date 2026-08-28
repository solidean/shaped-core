#include <clean-core/common/assert.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/platform/system_info.hh>
#include <clean-core/record/stat.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/bench/environment.hh>
#include <nexus/bench/report.hh>
#include <nexus/test.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

// The uncertainty notation, against the examples the design was settled on.
//
// Every case here is worked out by hand in the comment beside it, because the whole value of this layer is that the
// rendering is predictable: a reader who learns it once must be able to trust it everywhere.

using namespace cc::primitive_defines;

namespace
{
nx::bench::report_style plain()
{
    return {}; // no colour, no markdown — the form that gets pasted into a message
}

// A result carrying nothing but the statistics the report reads.
nx::bench::result loop_with(cc::string_view name, f64 median, f64 ci_low, f64 ci_high)
{
    auto r = nx::bench::result{};
    r.name = cc::string(name);
    r.time.median = median;
    r.time.ci95_low = ci_low;
    r.time.ci95_high = ci_high;
    r.time.min = ci_low;
    r.time.max = ci_high;
    r.time.mean = median;
    r.time.trimmed_mean = median;
    r.time.sample_count = 32;
    r.samples.push_back(median);
    r.batch_size = 1;
    r.measured_iterations = 32;
    r.measured_seconds = median * 32;
    r.converged = true;
    return r;
}
} // namespace

TEST("bench - a value is printed to exactly the decimal place its interval reaches")
{
    // The uncertainty's leading digit is at the ones place, so both numbers stop there.
    CHECK(nx::bench::format_uncertain(123, 2, &cc::rec::unit_count, plain()) == "123 \xc2\xb1 2");

    // A wider interval does not move the decimal place here — it is already at the ones — but it is visibly wider.
    CHECK(nx::bench::format_uncertain(123, 15, &cc::rec::unit_count, plain()) == "123 \xc2\xb1 15");

    // 1.834 ms +/- 0.004 ms, in seconds: the prefix is picked first, then both numbers are written in those units.
    CHECK(nx::bench::format_uncertain(0.001834, 0.000004, &cc::rec::unit_seconds, plain()) == "1.834 \xc2\xb1 0.004 ms");
}

TEST("bench - an uncertainty below every printed digit is not printed at all")
{
    // A femtosecond of noise on a millisecond value: nothing shown is in doubt, so there is nothing to qualify.
    auto const s = nx::bench::format_uncertain(0.001834, 1e-15, &cc::rec::unit_seconds, plain());
    CHECK(!s.contains("\xc2\xb1"));
    CHECK(s.contains("1.834"));
}

TEST("bench - a value with no reliable digit reads as a failure, not a wide error bar")
{
    // 123 +/- 200: the interval reaches past the leading digit, so no digit of the value survives it.
    auto const s = nx::bench::format_uncertain(123, 200, &cc::rec::unit_count, plain());
    CHECK(s.contains("unstable"));
    CHECK(s.contains("\xc2\xb1"));
}

TEST("bench - colour mutes the uncertainty without changing the text")
{
    auto styled = plain();
    styled.color = true;

    auto const colored = nx::bench::format_uncertain(123, 2, &cc::rec::unit_count, styled);

    CHECK(colored.contains("\x1b[")); // an SGR escape wraps the uncertainty term
    CHECK(colored.contains("123"));
    CHECK(colored.contains("\xc2\xb1 2"));

    // Colour changes what stands out, never what is said: strip the escapes and the two forms are identical.
    CHECK(nx::bench::format_uncertain(123, 2, &cc::rec::unit_count, plain()) == "123 \xc2\xb1 2");
}

TEST("bench - quantities carry their unit's prefix base")
{
    // unit_bytes takes binary prefixes.
    CHECK(nx::bench::format_quantity(4096, &cc::rec::unit_bytes) == "4.00 KiB");
    CHECK(nx::bench::format_quantity(1024 * 1024, &cc::rec::unit_bytes) == "1.00 MiB");

    // unit_count takes decimal ones, and has no symbol.
    CHECK(nx::bench::format_quantity(1500000, &cc::rec::unit_count) == "1.50 M");

    // unit_seconds scales down as readily as up.
    CHECK(nx::bench::format_quantity(0.000001234, &cc::rec::unit_seconds) == "1.23 us");
}

TEST("bench - one loop is drawn as a block, several as a table")
{
    auto const single = cc::vector<nx::bench::result>{loop_with("only", 0.001, 0.00099, 0.00101)};
    auto const block = nx::bench::format_report("a benchmark", single, plain());

    // The block spends its width on depth, since there is nothing to compare against.
    CHECK(block.contains("median"));
    CHECK(block.contains("interval"));
    CHECK(block.contains("samples"));
    CHECK(block.contains("mad"));

    auto const several = cc::vector<nx::bench::result>{
        loop_with("cc::sort", 0.001, 0.00099, 0.00101),
        loop_with("std::sort", 0.002, 0.00198, 0.00202),
    };
    auto const table = nx::bench::format_report("a comparison", several, plain());

    CHECK(table.contains("cc::sort"));
    CHECK(table.contains("std::sort"));
    CHECK(table.contains("baseline"));

    // A table trades depth for comparability, so the per-loop block is not in it.
    CHECK(!table.contains("interval"));
}

TEST("bench - the tail is reported only where a sample is one iteration")
{
    auto batched = cc::vector<nx::bench::result>{loop_with("batched", 0.001, 0.00099, 0.00101)};
    CHECK(batched[0].config.batch); // the default, and what every throughput benchmark runs

    // Under batching a sample is a batch MEAN, so its 95th percentile is the tail of an average.
    // Printing that as a latency percentile would be a claim the numbers do not support.
    CHECK(!nx::bench::format_report("batched", batched, plain()).contains("p95"));

    auto unbatched = batched;
    unbatched[0].config.batch = false;
    CHECK(nx::bench::format_report("unbatched", unbatched, plain()).contains("p95"));
}

TEST("bench - a difference whose interval spans one is drawn as no difference")
{
    // The two intervals overlap heavily, so the ratio's interval contains 1.
    auto const overlapping = cc::vector<nx::bench::result>{
        loop_with("a", 0.001, 0.0009, 0.0011),
        loop_with("b", 0.00105, 0.00095, 0.00115),
    };
    auto const table = nx::bench::format_report("overlap", overlapping, plain());

    CHECK(table.contains("~same"));
    CHECK(!table.contains('%')); // no percentage is claimed where none was measured
}

TEST("bench - a real difference is drawn with its sign")
{
    auto const separated = cc::vector<nx::bench::result>{
        loop_with("fast", 0.001, 0.00099, 0.00101),
        loop_with("slow", 0.002, 0.00198, 0.00202),
    };
    auto const table = nx::bench::format_report("separated", separated, plain());

    CHECK(table.contains("+100.0%"));
    CHECK(!table.contains("~same"));
}

TEST("bench - past a factor of two the comparison is a factor, not a percentage")
{
    // A 6x speedup, which is what a scaling sweep at 19 workers looks like.
    // As a percentage it reads "-83.0%", and nobody turns that into the 5.9x they would go on to quote.
    auto const speedup = cc::vector<nx::bench::result>{
        loop_with("serial", 0.0018, 0.00179, 0.00181),
        loop_with("pool", 0.0003, 0.000299, 0.000301),
    };
    CHECK(nx::bench::format_report("speedup", speedup, plain()).contains("6.0x faster"));

    // And the other way, which is the shape an async graph against a direct call takes.
    auto const slowdown = cc::vector<nx::bench::result>{
        loop_with("direct", 0.001, 0.00099, 0.00101),
        loop_with("async", 0.05, 0.0499, 0.0501),
    };
    CHECK(nx::bench::format_report("slowdown", slowdown, plain()).contains("50.0x slower"));
}

TEST("bench - the baseline is the first loop unless one asks to be it")
{
    auto loops = cc::vector<nx::bench::result>{
        loop_with("first", 0.002, 0.00198, 0.00202),
        loop_with("second", 0.001, 0.00099, 0.00101),
    };
    loops[1].config.is_baseline = true;

    auto const table = nx::bench::format_report("marked", loops, plain());

    // "second" is the baseline, so "first" reads as twice its time.
    // Had the default first-declared rule won instead, the table would carry -50% against "first".
    CHECK(table.contains("+100.0%"));
    CHECK(!table.contains("-50"));
}

TEST("bench - a column no row fills is not printed")
{
    // A sweep measuring whole passes: no loop declares items, and a sweep has no baseline either.
    auto loops = cc::vector<nx::bench::result>{
        loop_with("w=1", 0.001, 0.00099, 0.00101),
        loop_with("w=2", 0.002, 0.00198, 0.00202),
    };
    for (auto& r : loops)
        r.config.no_baseline = true;

    auto const table = nx::bench::format_report("sweep", loops, plain());

    // Both the items/s and the comparison column would otherwise be a column of dashes.
    // That reads as data missing rather than as data never asked for, which is the failure this pins.
    CHECK(!table.contains('-'));
    CHECK(table.contains("w=1"));
    CHECK(table.contains("w=2"));
}

TEST("bench - a column is as wide as its widest entry")
{
    // The two medians render at different widths, so an unpadded median column knocks the items column right of it out
    // of line -- which is the whole reason a table beats a list of numbers.
    auto loops = cc::vector<nx::bench::result>{
        loop_with("row-a", 0.00095, 0.0009, 0.001),
        loop_with("row-b", 0.001, 0.00099, 0.00101),
    };
    for (auto& r : loops)
    {
        r.items = 1000;
        r.items_per_second = 1000.0; // identical text in that cell, so only the median's padding can move it
    }

    auto const rendered = nx::bench::format_report("aligned", loops, plain());
    auto const table = cc::string_view(rendered);

    // Where the items cell falls in its own line, which is equal across rows exactly when the median column is padded.
    auto const items_column = [&](cc::string_view row)
    {
        auto const at = table.find(row);
        CC_ASSERT(at >= 0, "row is missing from the table");
        return table.find("/s", at) - table.rfind('\n', at);
    };

    CHECK(items_column("row-a") == items_column("row-b"));
}

TEST("bench - markdown mode emits a table that survives a paste into a doc")
{
    auto style = plain();
    style.markdown = true;

    auto const loops = cc::vector<nx::bench::result>{
        loop_with("a", 0.001, 0.00099, 0.00101),
        loop_with("b", 0.002, 0.00198, 0.00202),
    };
    auto const table = nx::bench::format_report("md", loops, style);

    CHECK(table.contains("|---|"));
    CHECK(table.contains("| a "));

    // The uncertainty notation is markdown-safe, which is why it beat the tilde form the notation nearly took.
    // A tilde would have been read as strikethrough or subscript by a renderer; the only one here is "~same".
    auto const tilde_free = !table.contains('~');
    CHECK(tilde_free);
}

TEST("bench - a table renders its median column in one unit")
{
    // Three orders of magnitude apart: per-row prefixes would give "1.00 ns", "1.00 us", "1.00 ms" and make the
    // column a conversion exercise rather than a comparison.
    auto const loops = cc::vector<nx::bench::result>{
        loop_with("tiny", 1e-9, 0.99e-9, 1.01e-9),
        loop_with("small", 1e-6, 0.99e-6, 1.01e-6),
        loop_with("big", 1e-3, 0.99e-3, 1.01e-3),
    };
    auto const table = nx::bench::format_report("scales", loops, plain());

    // The slowest row picks the scale, so everything reads in milliseconds.
    CHECK(table.contains(" ms"));
    CHECK(!table.contains(" ns"));
    CHECK(!table.contains(" us"));
}

TEST("bench - a table with nothing to compare scales each row on its own")
{
    // The same three decades, as a sweep.
    // Nothing is read across the rows here, so one shared scale would spell the fast end in millionths of a
    // millisecond, standing in for a number the reader would rather see as 1 ns.
    auto loops = cc::vector<nx::bench::result>{
        loop_with("n=1", 1e-9, 0.99e-9, 1.01e-9),
        loop_with("n=2", 1e-6, 0.99e-6, 1.01e-6),
        loop_with("n=3", 1e-3, 0.99e-3, 1.01e-3),
    };
    for (auto& r : loops)
        r.config.no_baseline = true;

    auto const table = nx::bench::format_report("sweep", loops, plain());

    CHECK(table.contains(" ns"));
    CHECK(table.contains(" us"));
    CHECK(table.contains(" ms"));
}

TEST("bench - a table names the loop a warning came from")
{
    auto loops = cc::vector<nx::bench::result>{
        loop_with("alpha", 0.001, 0.00099, 0.00101),
        loop_with("beta", 0.002, 0.00198, 0.00202),
    };
    loops[1].warnings.push_back({
        .kind = nx::bench::warning_kind::did_not_converge,
        .severity = nx::bench::warning_severity::warning,
        .detail = cc::string("ran out of samples"),
    });

    auto const table = nx::bench::format_report("attributed", loops, plain());

    // Under a table, an unattributed warning says nothing about which row produced it.
    CHECK(table.contains("beta: ran out of samples"));
}

// The BENCHMARK macro's own wiring, checked against the registry rather than against the macro text.
//
// These are ordinary TESTs so they run in a normal sweep: the declarations they inspect live in the benchmark bucket
// and never run here, which is exactly the property being asserted.
TEST("bench - BENCHMARK declares the bucket, exclusivity and the main thread")
{
    auto const& registry = nx::get_static_test_registry();

    auto const* found = static_cast<nx::test_declaration const*>(nullptr);
    for (auto const& decl : registry.declarations)
        if (decl.name == "nx::bench - the barriers")
            found = &decl;

    REQUIRE(found != nullptr);

    CHECK(found->test_config.bucket == nx::config::test_bucket::benchmark);

    // Nothing else may run alongside: two tests sharing a machine share its caches and its memory bandwidth, so a
    // timing taken while another runs is a timing of the pair.
    CHECK(found->test_config.exclusive_global);

    CHECK(found->test_config.main_thread);

    // Left to the author, deliberately — an async benchmark needs the pool, and a microbenchmark does not care.
    CHECK(found->test_config.scheduler == nx::config::scheduler_mode::shared);
}

TEST("bench - a benchmark stays out of a normal sweep")
{
    auto const& registry = nx::get_static_test_registry();

    auto config = nx::test_schedule_config{};
    CHECK(!config.filters.empty() == false); // an unfiltered sweep of the normal bucket

    for (auto const& decl : registry.declarations)
        if (decl.test_config.bucket == nx::config::test_bucket::benchmark)
            CHECK(!config.would_run(decl));
}

// The environment layer: what the report header and the sidecar both read.
TEST("bench - the system summary is structurally complete even where it cannot be filled")
{
    auto const& sys = nx::bench::describe_system();

    // Never absent, so the shape a consumer parses does not change when sysinfo lands.
    CHECK(!sys.os.empty());
    CHECK(!sys.arch.empty());
    CHECK(!sys.build.empty());

    CHECK(sys.logical_cores >= 1);

    // The report is a view of cc's system information rather than a second source of it, so the two must agree.
    // cpu is not asserted non-empty: a platform that will not name its CPU reports an empty string, which is the honest
    // answer and was the placeholder "unknown" before.
    CHECK(cc::string_view(sys.cpu) == cc::string_view(cc::get_system_info().cpu_brand));
    CHECK(sys.logical_cores == cc::get_system_info().logical_cores());

    // No placeholders left, since cc gained the system-information library this flag was waiting on.
    CHECK(!sys.is_provisional);

    // The one field that decides whether a number means anything, and the compiler knows it exactly.
    CHECK(sys.assertions_enabled == (CC_ASSERT_ENABLED != 0));
}

TEST("bench - a load reading measures the clock this thread actually ran on")
{
    auto const first = nx::bench::sample_load();
    CHECK(first.ticks_per_ns > 0);

    // The OS busy fraction needs two readings to be a fraction at all, so the first is negative by contract.
    CHECK(first.cpu_busy_fraction < 0);

    auto const second = nx::bench::sample_load();
    CHECK(second.ticks_per_ns > 0);

    // A constant-rate counter, so two readings taken moments apart agree closely.
    // Wide tolerance on purpose: this is asserting the reading is a rate at all, not the machine's stability.
    auto const ratio = second.ticks_per_ns / first.ticks_per_ns;
    CHECK(ratio > 0.5);
    CHECK(ratio < 2.0);
}

TEST("bench - pinning reports what it achieved rather than assuming")
{
    // Never asserts, on any platform: where affinity is unavailable — macOS offers only advisory hints — this reports
    // false and changes nothing, which is what lets the caller print the truth.
    auto const pinned = nx::bench::try_pin_to_core(0);
    if (pinned)
        nx::bench::unpin();

    // An out-of-range core is refused rather than clamped onto some other core.
    CHECK(!nx::bench::try_pin_to_core(-1));
    CHECK(!nx::bench::try_pin_to_core(1 << 20));
}

TEST("bench - a sweep opts out of the comparison column")
{
    // Three orders of magnitude of work apart: a ratio against the first point is a statement about the input sizes
    // rather than about the code, and it reads as a percentage in the millions.
    auto loops = cc::vector<nx::bench::result>{
        loop_with("n=16", 1e-8, 0.99e-8, 1.01e-8),
        loop_with("n=1000", 1e-6, 0.99e-6, 1.01e-6),
        loop_with("n=1000000", 1e-3, 0.99e-3, 1.01e-3),
    };

    // Without the opt-out the table happily prints the nonsense.
    // Three orders of magnitude apart, so it renders as a factor rather than a percentage.
    auto const compared = nx::bench::format_report("sweep", loops, plain());
    CHECK(compared.contains("baseline"));
    CHECK(compared.contains("x slower"));

    // One loop opting out drops the column for the whole table.
    loops[0].config.no_baseline = true;
    auto const swept = nx::bench::format_report("sweep", loops, plain());
    CHECK(!swept.contains("baseline"));
    CHECK(!swept.contains("x slower"));

    // The rows themselves are untouched, and items/s is what stays comparable across a sweep.
    CHECK(swept.contains("n=16"));
    CHECK(swept.contains("n=1000000"));
}

TEST("bench - a large ratio reads as a factor, not a percentage")
{
    // A percentage stops being readable once the ratio leaves the neighbourhood of 1: "+42210%" is a true and useless
    // way to say 423x, and an async graph against a direct call produces exactly that.
    auto const slow = cc::vector<nx::bench::result>{
        loop_with("direct", 1e-9, 0.99e-9, 1.01e-9),
        loop_with("async", 423e-9, 422e-9, 424e-9),
    };
    auto const table = nx::bench::format_report("big", slow, plain());
    CHECK(table.contains("x slower"));
    CHECK(!table.contains('%'));

    // The other direction reads as a factor too, rather than as a percentage pinned near -100%.
    auto const fast = cc::vector<nx::bench::result>{
        loop_with("slow", 423e-9, 422e-9, 424e-9),
        loop_with("quick", 1e-9, 0.99e-9, 1.01e-9),
    };
    auto const inverted = nx::bench::format_report("big", fast, plain());
    CHECK(inverted.contains("x faster"));

    // Near 1 it stays a percentage, which is what reads best there.
    auto const near = cc::vector<nx::bench::result>{
        loop_with("a", 1e-9, 0.99e-9, 1.01e-9),
        loop_with("b", 2e-9, 1.99e-9, 2.01e-9),
    };
    auto const percent = nx::bench::format_report("near", near, plain());
    CHECK(percent.contains("+100.0%"));
    CHECK(!percent.contains("x slower"));
}
