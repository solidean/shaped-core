#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh>

#include <algorithm>
#include <chrono>

// Shared helpers for the clean-core micro-benchmarks under tests/benchmarks/. These are guide benchmarks
// (GUIDE_BENCHMARK) that print timing tables and record representative points via nx::guide; see
// libs/base/clean-core/docs/benchmarks/ and docs/guides/perf-results.md.

namespace bench
{
using cc::isize;
using cc::u64;

// Length sweep shared by the hash benchmarks: 1..32 (every length), then +8 up to 64, then *1.5 up to ~100k.
inline cc::vector<isize> hash_lengths()
{
    cc::vector<isize> lengths;
    for (isize l = 1; l <= 32; ++l)
        lengths.push_back(l);
    for (isize l = 40; l <= 64; l += 8)
        lengths.push_back(l);
    for (isize l = 64;;)
    {
        isize next = isize(double(l) * 1.5);
        if (next <= l)
            next = l + 1;
        if (next > 100000)
            break;
        lengths.push_back(next);
        l = next;
    }
    return lengths;
}

// Keeps benchmark results from being optimized away.
inline u64 volatile sink = 0;

// Adaptive timer. Runs `pass` (one full unit of work covering `units_per_pass` units, returning a u64 it
// computed) repeatedly until at least ~50 ms elapses, then returns units processed per second. Callers turn
// units/s into GB/s, Mops/s, etc. The returned accumulator is funneled into `sink` so the work stays live.
template <class Pass>
double measure_units_per_sec(double units_per_pass, Pass&& pass)
{
    using clock = std::chrono::steady_clock;

    u64 acc = pass(); // warmup
    long long reps = 1;
    double seconds = 0;
    for (;;)
    {
        auto const t0 = clock::now();
        for (long long r = 0; r < reps; ++r)
            acc ^= pass();
        seconds = std::chrono::duration<double>(clock::now() - t0).count();

        if (seconds >= 0.05 || reps >= (1ll << 24))
            break;
        reps *= 2;
    }
    sink = acc;

    return (units_per_pass * double(reps)) / seconds;
}

// Median of `Runs` independent measure_units_per_sec calls (each does its own prewarm), returned as
// units/second. The simplest robust noise filter for micro-benchmarks — call it in place of a single
// measure_units_per_sec when run-to-run variance matters.
template <int Runs = 5, class Pass>
double median_units_per_sec(double units_per_pass, Pass&& pass)
{
    static_assert(Runs >= 1);
    double results[Runs];
    for (int i = 0; i < Runs; ++i)
        results[i] = measure_units_per_sec(units_per_pass, pass);
    std::sort(results, results + Runs);
    return results[Runs / 2];
}
} // namespace bench
