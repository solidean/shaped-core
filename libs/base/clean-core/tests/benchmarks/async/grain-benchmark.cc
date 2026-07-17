#include <clean-core/common/macros.hh> // CC_HAS_THREADS

// The concurrent scheduler needs OS threads; this file compiles to nothing where they are unavailable (wasm).
#if CC_HAS_THREADS

// cc::async_thread_pool grain sweep — at what leaf size does fork-join overhead stop dominating?
//
// pool-benchmark.cc pins one grain per case (8192) and sweeps worker count: it answers "does it scale" but by
// construction never shows the region that grain was chosen to avoid. At 8192 elements per leaf, per-node
// scheduling cost is ~1/8192 of the leaf and simply invisible. This file sweeps the other axis — grain and n,
// both 1..8192 / 1..2^20 by powers of two, at a single P-worker pool — so that cost IS the measurement.
//
// Read the output as ns per INPUT element. For leaf work costing X ns/elem and per-node scheduling cost C, a
// grain-g bisection lands near X + C/g. So each grain line is flat where n is large (overhead amortized) and
// rises toward small n (the blocking_get round-trip spread over too few elements), and the vertical spread
// between lines at a fixed n is C/g — the thing this file exists to show.
//
// Three constraints that are not obvious and will quietly corrupt the numbers if broken:
//
// * `mix` must not be affine. pool-benchmark.cc used eight rounds of `x = x*a + b` and clang composed all
//   eight into a single imul+add, quietly turning its leaves into a memory-bandwidth streamer instead of the
//   compute-bound work they were meant to be (it has since been fixed the same way). The xor-shift/multiply
//   finalizer below is not affine over Z_2^32 and cannot collapse that way.
// * The grain is a namespace-scope global, NOT a capture. A two-child frame already captures exactly
//   span(16) + two shared_async(8+8) = the node's whole 32 B inline slot; one more member spills it into a
//   heap-boxed cc::unique_function, which puts an allocation in every task and makes this a benchmark of
//   malloc. It is written on the driving thread before the root is submitted and only read afterwards, so the
//   pool's own submit/steal synchronization publishes it.
// * One pool for the entire sweep. Constructing one is ~100 us of thread spawning; per-point construction
//   would land inside measure_units_per_sec's adaptive loop and dominate every short pass.
//
// Only grain <= n is measured: a grain above n never splits, so those points would all duplicate the
// single-leaf curve. Each grain line therefore starts at x == grain.

#include "../bench_util.hh"

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>

#include <cstdio>
#include <type_traits>

using cc::i32;
using cc::i64;
using cc::isize;
using cc::u32;
using cc::u64;

namespace
{
// make_async_lazy<i64>, but pinning the frame to the node's inline slot first. Mirrors
// async_node_base::frame_fits_inline (async_node.hh), which is protected and so cannot be named here — if that
// budget ever changes, this assert is what will (loudly) notice.
template <class F>
[[nodiscard]] cc::shared_async<i64> spawn(F&& f)
{
    static_assert(sizeof(std::decay_t<F>) <= 32 && alignof(std::decay_t<F>) <= 16,
                  "fork-join frame spilled out of the node's 32 B inline slot into a heap-boxed unique_function "
                  "— that is an allocation per task, and it would make this a benchmark of malloc rather than of "
                  "the scheduler. The grain is a global for exactly this reason; drop a capture rather than "
                  "relaxing this.");
    return cc::make_async_lazy<i64>(cc::forward<F>(f));
}

// --- sweep axes ------------------------------------------------------------------------------------------

constexpr isize max_n = 1 << 20;
constexpr isize max_grain = 1 << 13;

// The fork-floor sweep (run_fork_floor) reuses everything above on a much smaller n, at grain 1. Capped at 8
// workers: the floor's structure is all at the low end, and the full P sweep is minutes of 50 ms passes.
constexpr isize floor_max_n = 32;
constexpr int floor_max_workers = 8;

// Leaf cutoff of the graph currently being measured; see the header note on why this is not a capture.
isize g_grain = 1;

// --- leaf work -------------------------------------------------------------------------------------------

// lowbias32 (Chris Wellons' hash-prospector search): xor-shift/multiply, so it is not an affine map over
// Z_2^32 and cannot be folded into one multiply-add the way a chain of LCG steps can. Cheap on purpose —
// heavy leaf work would bury the per-node cost this file is trying to expose.
CC_FORCE_INLINE i32 mix(i32 x)
{
    u32 h = u32(x);
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return i32(h);
}

CC_FORCE_INLINE void mix_range(cc::span<i32> d)
{
    for (auto& x : d)
        x = mix(x);
}

CC_FORCE_INLINE i64 sum_range(cc::span<i32 const> d)
{
    i64 s = 0;
    for (auto x : d)
        s += mix(x);
    return s;
}

// --- async fork-join graphs ------------------------------------------------------------------------------
// Raw two-phase frames, same shape as pool-benchmark.cc: on the first poll either do the leaf work inline or
// spawn both children and park; on the re-poll, join. `l == nullptr` is the "have I spawned yet" test — using
// it instead of a step counter is what keeps these frames inside the 32 B inline slot. The children are
// captured, which is what keeps them alive: the pending-dependency list does not own anything.

cc::shared_async<i64> async_pfor(cc::span<i32> d)
{
    return spawn(
        [d, l = cc::shared_async<i64>(),
         r = cc::shared_async<i64>()](cc::async_context<i64>& actx) mutable -> cc::async_step_status
        {
            if (l == nullptr)
            {
                if (d.size() <= g_grain)
                {
                    mix_range(d);
                    return actx.success(i64(0));
                }
                isize const m = d.size() / 2;
                l = async_pfor(d.subspan(cc::start_end{0, m}));
                r = async_pfor(d.subspan(m));
                (void)actx.require(l);
                (void)actx.require(r);
                return actx.wait_for_dependencies();
            }
            return actx.success(i64(0));
        });
}

cc::shared_async<i64> async_reduce(cc::span<i32 const> d)
{
    return spawn(
        [d, l = cc::shared_async<i64>(),
         r = cc::shared_async<i64>()](cc::async_context<i64>& actx) mutable -> cc::async_step_status
        {
            if (l == nullptr)
            {
                if (d.size() <= g_grain)
                    return actx.success(sum_range(d));
                isize const m = d.size() / 2;
                l = async_reduce(d.subspan(cc::start_end{0, m}));
                r = async_reduce(d.subspan(m));
                (void)actx.require(l);
                (void)actx.require(r);
                return actx.wait_for_dependencies();
            }
            // by-value resolve: the sum is a temporary evaluated before the call, so it does not outlive the
            // frame that the resolve destroys
            return actx.success(*l->value_ptr() + *r->value_ptr());
        });
}

// --- harness ---------------------------------------------------------------------------------------------

cc::vector<i32> make_random(isize n)
{
    cc::vector<i32> v;
    v.reserve_exact(n);
    cc::random rng(12345);
    for (isize i = 0; i < n; ++i)
        v.push_back(i32(rng.next_u32()));
    return v;
}

// One CSV row per (case, n, grain), prefixed so it survives whatever else lands on stdout — grain-plot.py
// greps for the marker rather than trying to bound a table. Flushed per row so a long sweep shows progress.
void emit(char const* name, isize n, isize grain, double ns_per_elem)
{
    std::printf("GRAINCSV %s,%lld,%lld,%.6f\n", name, (long long)n, (long long)grain, ns_per_elem);
    std::fflush(stdout);
}

void run_all()
{
    // Hardware concurrency MINUS ONE: blocking_get makes this thread participate as a worker, so P workers
    // would put P+1 threads on P cores and measure the oversubscription rather than the grain.
    int const p = cc::async_thread_pool::default_worker_count();
    std::printf("\n### cc::async_thread_pool grain sweep (median of 5, %d workers + this thread) ###\n", p);
    std::printf("# ns per input element; grain <= n only; one pool reused across every point\n");

    cc::async_thread_pool pool(p);
    auto data = make_random(max_n);

    std::printf("GRAINCSV case,n,grain,ns_per_elem\n");

    for (isize grain = 1; grain <= max_grain; grain *= 2)
    {
        g_grain = grain;
        for (isize n = grain; n <= max_n; n *= 2)
        {
            auto const d = cc::span<i32>(data).subspan(cc::start_end{0, n});
            double const ns = 1e9
                            / bench::median_units_per_sec(double(n),
                                                          [&]
                                                          {
                                                              auto root = async_pfor(d);
                                                              (void)pool.blocking_get(root);
                                                              return u64(d[0]);
                                                          });
            emit("pfor", n, grain, ns);
        }
    }

    for (isize grain = 1; grain <= max_grain; grain *= 2)
    {
        g_grain = grain;
        for (isize n = grain; n <= max_n; n *= 2)
        {
            auto const d = cc::span<i32 const>(data).subspan(cc::start_end{0, n});
            double const ns = 1e9
                            / bench::median_units_per_sec(double(n),
                                                          [&]
                                                          {
                                                              auto root = async_reduce(d);
                                                              return u64(pool.blocking_get(root));
                                                          });
            emit("reduce", n, grain, ns);
        }
    }

    std::printf("\nPlot it: uv run libs/base/clean-core/tests/benchmarks/async/grain-plot.py\n");
    std::fflush(stdout);
}

// The fork floor: what does the SECOND thread cost?
//
// The grain sweep's total-time view says an un-split single node costs ~0.3 us, but a graph that forks even
// once jumps to ~11 us and stays on that plateau out to ~2^12 elements. That plateau, not per-node cost, is
// what makes small graphs expensive. It should be the pool waking a worker to take a published sibling.
//
// This sweeps the two axes that tell those apart, at grain 1 so every element is its own node:
//   pool size 1..8 — a FIXED handoff cost stays flat here; a contention effect grows.
//   n 1..32        — how the floor amortizes as the graph gets real work to do.
//
// Read the w=1 line first: one worker plus the participating caller is the minimum fork, so it is the floor's
// floor. Anything above it that grows with w is the pool getting in its own way.
void run_fork_floor()
{
    int const p = cc::num_hardware_threads();
    int const w_max = p < floor_max_workers ? p : floor_max_workers;
    std::printf("\n### fork floor: parallel-for, grain 1 (median of 5, %d workers max, %d hardware threads) ###\n",
                w_max, p);
    std::printf("# total ns per pass; one pool per worker count, reused across that count's n sweep\n");
    std::printf("FLOORCSV workers,n,ns_total\n");

    auto data = make_random(floor_max_n);
    g_grain = 1; // every element its own leaf: the graph is all fork, no work

    for (int w = 1; w <= w_max; ++w)
    {
        cc::async_thread_pool pool(w);
        for (isize n = 1; n <= floor_max_n; ++n)
        {
            auto const d = cc::span<i32>(data).subspan(cc::start_end{0, n});
            // units = 1 pass, so this reads back as total ns for the whole graph rather than per element
            double const ns = 1e9
                            / bench::median_units_per_sec(1.0,
                                                          [&]
                                                          {
                                                              auto root = async_pfor(d);
                                                              (void)pool.blocking_get(root);
                                                              return u64(d[0]);
                                                          });
            std::printf("FLOORCSV %d,%lld,%.3f\n", w, (long long)n, ns);
            std::fflush(stdout);
        }
    }

    std::printf("\nPlot it: uv run libs/base/clean-core/tests/benchmarks/async/fork-floor-plot.py\n");
    std::fflush(stdout);
}

// Why every grain line in the sweep converges to the same ~21 us at n == 1: that is not per-node cost (a node
// is ~50-100 ns inline), it is the foreign-thread round trip — submit, wake a worker, run, wake the caller.
// Sweeping the worker count separates the two candidate explanations, which is the whole point of the probe:
// two context switches would be a constant, whereas contention on the shared injection mutex grows with the
// number of idle workers scanning it.
void run_latency()
{
    std::printf("\n=== blocking_get round-trip, one trivial node (median of 5) ===\n");
    std::printf("%-10s %15s\n", "workers", "ns/roundtrip");
    std::printf("%-10s %15s\n", "-------", "------------");

    int const p = cc::num_hardware_threads();
    for (int w : {1, 2, 4, 8, 16, 32, 64})
    {
        if (w > p)
            continue;
        cc::async_thread_pool pool(w);
        double const ns
            = 1e9
            / bench::median_units_per_sec(1.0,
                                          [&]
                                          {
                                              auto root = cc::make_async_lazy<i64>([](cc::async_context<i64>& actx)
                                                                                   { return actx.success(i64(1)); });
                                              return u64(pool.blocking_get(root));
                                          });
        std::printf("%-10d %15.1f\n", w, ns);
        std::fflush(stdout);
    }
}
} // namespace

// Grain x size sweep for parallel-for and reduction. ~5 minutes, so --timeout 0 is not optional: dev.py kills
// a test binary at 60 s by default and would cut the table off mid-sweep. Run by exact name:
//   uv run dev.py --mirror-test-output test "bench-async-grain (sweep)" --preset release-clang --timeout 0
// or let grain-plot.py drive it and chart the result.
TEST("bench-async-grain (sweep)", nx::config::manual)
{
    run_all();
}

// The floor every grain line in the sweep hits at small n, isolated. Run by exact name:
//   uv run dev.py --mirror-test-output test "bench-async-latency (round-trip)" --preset release-clang
TEST("bench-async-latency (round-trip)", nx::config::manual)
{
    run_latency();
}

// Does the fork floor scale with pool size? ~4 min, so --timeout 0 again. Run by exact name:
//   uv run dev.py --mirror-test-output test "bench-async-fork-floor (thread sweep)" --preset release-clang --timeout 0
// or let fork-floor-plot.py drive it and chart the result.
TEST("bench-async-fork-floor (thread sweep)", nx::config::manual)
{
    run_fork_floor();
}

#endif // CC_HAS_THREADS
