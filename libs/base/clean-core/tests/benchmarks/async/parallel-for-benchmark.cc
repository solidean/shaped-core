#include <clean-core/common/macros.hh> // CC_HAS_THREADS

// The pools measured here need OS threads; this file compiles to nothing where they are unavailable (wasm).
#if CC_HAS_THREADS

// cc::async_parallel_for against a serial loop and against a node per chunk, the shape it replaces.
//
// Wall time on a machine that may be loaded, so every round runs every variant once, interleaved, and a row is the median over rounds.
// One timed run is one coroutine awaiting the loop `calls` times on the pool, as an inner loop of a solver does, with an optional serial gap between calls.
// The body is a dependent hash chain, so its cost per index is compute and not memory.
// How to read the tables, and what they said when the loop was designed, is in libs/base/clean-core/docs/benchmarks/async-benchmark.md.

#include <clean-core/common/time.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/async_parallel_for.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <nexus/bench/run.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
CC_FORCE_INLINE u32 mix(u32 h)
{
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

cc::vector<u32> g_out;
f64 g_ns_per_round = 1;

u32 work(isize i, int rounds)
{
    auto h = u32(i) + 1;
    for (auto r = 0; r < rounds; ++r)
        h = mix(h);
    return h;
}

void calibrate()
{
    auto const rounds = 20'000'000;
    auto const t0 = cc::current_time_steady_secs();
    nx::bench::sink(work(1, rounds));
    g_ns_per_round = 1e9 * (cc::current_time_steady_secs() - t0) / rounds;
}

struct scenario
{
    char const* name;
    isize n;
    f64 ns_per_index;  // the mean
    bool ramp = false; // index i costs 3 (i/n)^2 times the mean: a load the static split gets wrong
    f64 gap_us = 0;    // serial work between two calls, long enough for the pool to go to sleep
};

int rounds_of(scenario const& s, isize i)
{
    auto const mean = s.ns_per_index / g_ns_per_round;
    if (!s.ramp)
        return int(mean + 0.5);
    auto const t = f64(i) / f64(s.n);
    return int(3 * t * t * mean + 0.5);
}

void spin_for(f64 us)
{
    auto const until = cc::current_time_steady_secs() + 1e-6 * us;
    while (cc::current_time_steady_secs() < until)
    {
    }
}

void run_range(scenario const& s, isize begin, isize end)
{
    for (auto i = begin; i < end; ++i)
        g_out[i] = work(i, rounds_of(s, i));
}

enum class variant
{
    serial,
    node_per_chunk, // four chunks per participant, a coroutine each, joined by async_all
    parallel_for,
    parallel_for_grain_1,
};

char const* name_of(variant v)
{
    switch (v)
    {
    case variant::serial:
        return "serial";
    case variant::node_per_chunk:
        return "node per chunk";
    case variant::parallel_for:
        return "async_parallel_for";
    case variant::parallel_for_grain_1:
        return "  ... at grain 1";
    }
    return "";
}

cc::shared_async<cc::unit> run_chunk(scenario s, isize begin, isize end)
{
    run_range(s, begin, end);
    co_return;
}

cc::shared_async<cc::unit> drive(variant v, scenario s, int calls)
{
    for (auto c = 0; c < calls; ++c)
    {
        switch (v)
        {
        case variant::serial:
            run_range(s, 0, s.n);
            break;
        case variant::node_per_chunk:
        {
            auto const count = cc::min(s.n, isize(4 * cc::async_scheduler::current().parallelism()));
            auto parts = cc::vector<cc::shared_async<cc::unit>>();
            for (auto k = isize(0); k < count; ++k)
                parts.push_back(run_chunk(s, s.n * k / count, s.n * (k + 1) / count));
            co_await cc::async_all(cc::span<cc::shared_async<cc::unit> const>(parts));
            break;
        }
        case variant::parallel_for:
            co_await cc::async_parallel_for(s.n, [&](isize begin, isize end) { run_range(s, begin, end); });
            break;
        case variant::parallel_for_grain_1:
            co_await cc::async_parallel_for(s.n, [&](isize begin, isize end) { run_range(s, begin, end); }, {.grain = 1});
            break;
        }
        if (s.gap_us > 0)
            spin_for(s.gap_us);
    }
}

f64 median(cc::vector<f64> v)
{
    for (auto i = isize(1); i < v.size(); ++i)
        for (auto j = i; j > 0 && v[j - 1] > v[j]; --j)
            cc::swap(v[j - 1], v[j]);
    return v[v.size() / 2];
}

/// One table: every variant against the first, which is serial.
void run_table(cc::async_thread_pool& pool, scenario const& s, cc::span<variant const> vs)
{
    g_out.resize_to_defaulted(s.n);
    auto const serial_us = s.n * s.ns_per_index * 1e-3 + s.gap_us;
    auto const calls = int(cc::max(1.0, 15'000 / serial_us)); // a serial run near 15 ms
    auto const rounds = 11;

    auto times = cc::vector<cc::vector<f64>>();
    times.resize_to_defaulted(vs.size());
    for (auto r = 0; r < rounds + 1; ++r) // round 0 warms up and is dropped
        for (auto vi = isize(0); vi < vs.size(); ++vi)
        {
            auto const t0 = cc::current_time_steady_secs();
            cc::async_blocking_get_on(pool, drive(vs[vi], s, calls));
            if (r > 0)
                times[vi].push_back(1e6 * (cc::current_time_steady_secs() - t0) / calls);
        }

    cc::println("  {}: n {}, {:.0f} ns/index{}{}", s.name, s.n, s.ns_per_index, s.ramp ? " (ramp)" : "",
                s.gap_us > 0 ? cc::format(", {:.0f} us serial gap", s.gap_us) : cc::string());
    auto const base = median(times[0]);
    for (auto vi = isize(0); vi < vs.size(); ++vi)
    {
        auto const m = median(times[vi]);
        cc::println("    {:<20} {:>10.2f} us/call  {:>6.2f}x", name_of(vs[vi]), m, base / m);
    }
}
} // namespace

BENCHMARK("bench-async-parallel-for - against serial and a node per chunk", no_scheduler)
{
    calibrate();
    cc::async_thread_pool pool; // hardware concurrency - 1, plus the driving thread
    cc::println("{} workers + the driving thread, {:.3f} ns per hash round", pool.worker_count(), g_ns_per_round);
    variant const vs[] = {variant::serial, variant::node_per_chunk, variant::parallel_for, variant::parallel_for_grain_1};
    scenario const ss[] = {
        {.name = "small", .n = 128, .ns_per_index = 300},
        {.name = "inner loop", .n = 2000, .ns_per_index = 300},
        {.name = "inner loop, larger", .n = 8000, .ns_per_index = 300},
        {.name = "ramp", .n = 8000, .ns_per_index = 300, .ramp = true},
        {.name = "cheap", .n = 65536, .ns_per_index = 5},
        {.name = "cheap, large", .n = 1 << 20, .ns_per_index = 2},
    };
    for (auto const& s : ss)
        run_table(pool, s, vs);
}

BENCHMARK("bench-async-parallel-for - a sleeping pool, and idle linger", no_scheduler)
{
    calibrate();
    cc::async_thread_pool pool;
    variant const vs[] = {variant::serial, variant::parallel_for};
    for (auto const linger : {0.0, 50.0})
    {
        pool.set_idle_linger_us(linger);
        cc::println("{} workers + the driving thread, idle linger {:.0f} us", pool.worker_count(), linger);
        for (auto const gap : {0.0, 100.0, 1000.0})
            run_table(pool, {.name = "inner loop", .n = 2000, .ns_per_index = 300, .gap_us = gap}, vs);
    }
}

#endif
