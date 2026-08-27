#include <clean-core/container/vector.hh>
#include <clean-core/record/stat.hh>
#include <nexus/bench/report.hh>
#include <nexus/test.hh>

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

TEST("bench - uncertainty brackets the digits the interval reaches")
{
    // 123 +/- 2: the uncertainty's leading digit is at the ones place, so only the last digit is unresolved.
    CHECK(nx::bench::format_uncertain(123, 2, &cc::rec::unit_count, plain()) == "12[3]");

    // 123 +/- 15: the leading digit is at the tens place, so both the tens and the ones go.
    CHECK(nx::bench::format_uncertain(123, 15, &cc::rec::unit_count, plain()) == "1[23]");

    // 1.834 ms +/- 0.004 ms, in seconds — the prefix is picked first, then the digits are marked in those units.
    CHECK(nx::bench::format_uncertain(0.001834, 0.000004, &cc::rec::unit_seconds, plain()) == "1.83[4] ms");
}

TEST("bench - an uncertainty below every printed digit brackets nothing")
{
    // A femtosecond of noise on a millisecond value: nothing shown is in doubt.
    auto const s = nx::bench::format_uncertain(0.001834, 1e-15, &cc::rec::unit_seconds, plain());
    CHECK(!s.contains('['));
    CHECK(s.contains("1.834"));
}

TEST("bench - a value with no reliable digit reads as a failure, not a wide error bar")
{
    // 123 +/- 200: the interval reaches past the leading digit, so there is no number to report.
    auto const s = nx::bench::format_uncertain(123, 200, &cc::rec::unit_count, plain());
    CHECK(s.contains("unstable"));
    CHECK(!s.contains('['));
}

TEST("bench - brackets are the plain-text form, muted digits the coloured one")
{
    auto styled = plain();
    styled.color = true;

    auto const colored = nx::bench::format_uncertain(123, 2, &cc::rec::unit_count, styled);

    // No bracket pair around the digits — the eye is meant to land on the stable part unaided.
    // A bare '[' check would be wrong here: an SGR escape is literally "\x1b[90m", brackets and all.
    CHECK(!colored.contains("[3]"));
    CHECK(colored.contains("\x1b[")); // an SGR escape wraps the unreliable digits
    CHECK(colored.contains("12"));

    // The same value without colour takes the bracketed form instead.
    CHECK(nx::bench::format_uncertain(123, 2, &cc::rec::unit_count, plain()) == "12[3]");
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

    // Brackets are markdown-safe, which is why they beat the tilde form the notation nearly took.
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
