#include <clean-core/container/vector.hh>
#include <clean-core/string/print.hh>
#include <clean-core/string/to_string.hh>
#include <nexus/bench/bench.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// The classic cache lesson: two traversals of a large 2D array doing the *same* work — n*n element reads, the same instruction count — in different memory order.
// Row-major walks each row contiguously and is cache friendly; column-major strides a whole row per step and is cache hostile, so almost every access misses.
// Same instructions, far more cycles and cache misses.
//
// Manual and print-only: it allocates ~256 MiB and is a demonstration rather than a fast unit test, so run it by exact name.
// Vectorization and unrolling are disabled on the inner loops, so both walks retire the same scalar instruction stream and the access pattern is the only variable left.
//
// docs/guides/profiling.md walks through the numbers this prints.

namespace
{
u64 volatile s_sink = 0;

// 8192 x 8192 32-bit ints = 256 MiB — far larger than any last-level cache.
constexpr auto s_n = isize(8192);

u64 sum_row_major(cc::span<u32 const> data)
{
    auto sum = u64(0);
    for (auto y = isize(0); y < s_n; ++y)
    {
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
        for (auto x = isize(0); x < s_n; ++x)
            sum += data[y * s_n + x]; // contiguous inner stride
    }
    return sum;
}

u64 sum_col_major(cc::span<u32 const> data)
{
    auto sum = u64(0);
    for (auto x = isize(0); x < s_n; ++x)
    {
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
        for (auto y = isize(0); y < s_n; ++y)
            sum += data[y * s_n + x]; // inner stride jumps a full row -> a fresh cache line almost every step
    }
    return sum;
}

void print_row(cc::string_view label, nx::bench::hw_measurement const& m)
{
    using nx::bench::hw_counter;
    auto val = [&](hw_counter c) -> cc::string
    {
        auto const v = m.value_of(c);
        return v.has_value() ? cc::to_string(v.value()) : cc::string("n/a");
    };
    cc::println("  {}: ns={} ref_cycles={} instructions={} llc_misses={}", label, val(hw_counter::elapsed_nanoseconds),
                val(hw_counter::reference_cycles), val(hw_counter::instructions_retired),
                val(hw_counter::cache_llc_misses));
}
} // namespace

TEST("nexus bench - 2d traversal cache effect", nx::config::manual)
{
    using nx::bench::hw_counter;

    auto data = cc::vector<u32>::create_defaulted(s_n * s_n);
    for (auto i = isize(0); i < data.size(); ++i)
        data[i] = u32(i); // touch every page so we do not measure first-touch faults

    // Baseline (elapsed + ref cycles) plus two PMU counters.
    // measure_all re-runs the walk across PMC subsets if the two do not fit at once, so instructions and cache misses are both measured whatever other sessions hold.
    // The walks are deterministic, so the combined values stay comparable.
    auto const cfg = nx::bench::hw_measure_config{
        .counters = cc::vector<hw_counter>{hw_counter::elapsed_nanoseconds, hw_counter::reference_cycles,
                                           hw_counter::instructions_retired, hw_counter::cache_llc_misses},
        .measure_all = true,
    };

    auto const row = nx::bench::measure_hw_counters([&] { s_sink = sum_row_major(data); }, cfg);
    auto const col = nx::bench::measure_hw_counters([&] { s_sink = sum_col_major(data); }, cfg);

    cc::println("2d traversal of {}x{} u32 ({} MiB), same work, different memory order:", s_n, s_n,
                (s_n * s_n * isize(sizeof(u32))) >> 20);
    print_row("row-major", row);
    print_row("col-major", col);

    // The core, always-available demonstration: identical work, but the cache-hostile walk burns far more
    // reference cycles (wall-time). A modest margin keeps it robust to noise.
    auto const row_cycles = row.value_of(hw_counter::reference_cycles);
    auto const col_cycles = col.value_of(hw_counter::reference_cycles);
    REQUIRE(row_cycles.has_value());
    REQUIRE(col_cycles.has_value());
    CHECK(col_cycles.value() > row_cycles.value() * 3 / 2);

    // Same instruction count (within a small margin): the two walks retire the same scalar stream.
    auto const row_ins = row.value_of(hw_counter::instructions_retired);
    auto const col_ins = col.value_of(hw_counter::instructions_retired);
    if (row_ins.has_value() && col_ins.has_value())
    {
        auto const lo = row_ins.value() < col_ins.value() ? row_ins.value() : col_ins.value();
        auto const hi = row_ins.value() < col_ins.value() ? col_ins.value() : row_ins.value();
        CHECK(hi <= lo * 6 / 5); // within 20%
    }

    // When cache-miss counters flow, the column-major walk misses the last-level cache far more often.
    auto const row_miss = row.value_of(hw_counter::cache_llc_misses);
    auto const col_miss = col.value_of(hw_counter::cache_llc_misses);
    if (row_miss.has_value() && col_miss.has_value())
        CHECK(col_miss.value() > row_miss.value() * 2);
    else
        SUCCEED("cache-miss counter did not flow (no PMU access or budget) — cycle gap still shown above");
}
