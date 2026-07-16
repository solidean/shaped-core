#include <clean-core/common/macros.hh> // CC_HAS_THREADS

// The concurrent scheduler needs OS threads; this file compiles to nothing where they are unavailable (wasm).
#if CC_HAS_THREADS

// cc::async_thread_pool benchmark — Phase 2 of the async performance gate.
//
// Phase 1 (async-benchmark.cc) answered the single-thread question. This one asks the two that decide the gate:
// does a graph get near-linear speedup on the regular cases, and what does a fork-join task cost? Every case is
// a hand-written fork-join graph over the raw primitives — the ergonomic parallel_for/reduce helpers are a
// post-gate concern and must not block measurement.
//
// The five canonical shapes:
//   parallel quicksort   recursive, irregular subproblem sizes -> the steal-quality stress
//   parallel-for         recursive bisection, perfectly balanced -> the near-linear-speedup case
//   reduction            bisect + join, values flow back up the tree
//   nested parallel-for  a parallel-for whose leaf is itself a parallel-for -> deque depth + nested spawn
//   spawn tree           trivial leaves, so per-node scheduling overhead IS the measurement
//
// Four things about the numbers, none obvious from a table and each one learned by getting it wrong:
//
// * A "w worker" row runs w+1 THREADS. blocking_get makes the calling thread participate as a worker for the
//   duration (see async_thread_pool.hh), so the sweep tops out at hardware-concurrency MINUS ONE, and the "vs
//   1w" column is anchored on a 2-thread config rather than a serial one. Compare rows, not absolutes.
// * The machine matters and this file cannot know it. A heterogeneous part (P+E cores, SMT) bends the curve at
//   the E-core and hyperthread boundaries, which is a property of the machine and not a defect in the
//   scheduler. Judge the knee, not the top, against the count of *performance* cores on whatever you ran.
// * Leaf work must stay compute-bound, and that is a live constraint, not a note: a memory-bound leaf caps
//   speedup at DRAM bandwidth and measures the machine instead of the scheduler. It broke exactly once already
//   — see mix(), whose eight LCG rounds were folded into one multiply-add by clang until it was made
//   non-affine. If a case stops scaling past ~8 workers, check the codegen before blaming the pool.
// * The spawn tree is the only case whose leaf does nothing, so it is the only one where per-node scheduling
//   cost IS the measurement — and correspondingly the one most sensitive to idle-worker policy.
//
// Every fork-join frame is kept at or under the node's 32 B inline frame slot: a closure over 32 B falls back
// to a heap-boxed cc::unique_function, which would put an allocation in every task. That is why the grain sizes
// are namespace-scope constants rather than captures, and why a two-child frame captures exactly
// span(16) + two shared_async(8+8). Adding one capture to any of these silently changes what is measured.

#include "../bench_util.hh"

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/guide.hh>
#include <nexus/test.hh>

#include <algorithm>
#include <cstdio>
#include <type_traits>

using cc::i32;
using cc::i64;
using cc::isize;
using cc::u64;

namespace
{
// make_async_lazy<i64>, but pinning the frame to the node's inline slot first.
//
// Mirrors async_node_base::frame_fits_inline (async_node.hh), which is protected and so cannot be named here —
// if that budget ever changes, this assert is what will (loudly) notice. The zero-dependency make_async_lazy
// form wraps the frame in a lambda whose only capture is the frame itself, so sizeof(F) IS the installed frame
// size; the variadic dep form would additionally store the dep handles, and none of these use it.
template <class F>
[[nodiscard]] cc::shared_async<i64> spawn(F&& f)
{
    static_assert(sizeof(std::decay_t<F>) <= 32 && alignof(std::decay_t<F>) <= 16,
                  "fork-join frame spilled out of the node's 32 B inline slot into a heap-boxed unique_function "
                  "— that is an allocation per task, and it would make this a benchmark of malloc rather than "
                  "of the scheduler. Drop a capture (see the grain constants) rather than relaxing this.");
    return cc::make_async_lazy<i64>(cc::forward<F>(f));
}

// --- workload constants (namespace scope so frames stay <= 32 B; see the header note) --------------------

constexpr isize qsort_n = 1 << 20;   // 4 MiB of i32
constexpr isize qsort_cutoff = 1024; // below this a subproblem is sorted serially

constexpr isize pfor_n = 1 << 22;
constexpr isize pfor_grain = 8192;

constexpr isize reduce_n = 1 << 22;
constexpr isize reduce_grain = 8192;

// nested: the outer bisects down to outer_grain, then each leaf spawns an inner parallel-for over its own
// range. Two frame types, each with its own grain, so neither has to capture one.
constexpr isize nested_n = 1 << 22;
constexpr isize nested_outer_grain = 1 << 16;
constexpr isize nested_inner_grain = 4096;

constexpr int tree_depth = 16; // 2^17 - 1 = 131071 nodes, trivial leaves

// --- leaf work -------------------------------------------------------------------------------------------

// An empty inline-asm barrier: makes `v` un-analyzable, and (being volatile) gives its containing function
// side effects. The second half is the one that matters here — without it a CC_DONT_INLINE serial baseline is
// still a pure function of unchanging arguments, so clang hoists the whole call out of the timing loop and the
// baseline measures 0.00 ns. Same barrier, same reason, as async-benchmark.cc.
CC_FORCE_INLINE void opaque(i64& v)
{
#if defined(__clang__) || defined(__GNUC__)
    __asm__ volatile("" : "+r"(v));
#else
    i64 volatile tmp = v; // MSVC fallback: a volatile round-trip the optimizer cannot see through
    v = tmp;
#endif
}

// Cheap, data-dependent, compute-bound mixing, so the benchmark measures scheduling rather than DRAM
// bandwidth. Shared verbatim by the serial baselines and the async leaves — the comparison is only honest if
// both do exactly the same arithmetic.
//
// It must NOT be affine. This was eight rounds of `x = x*1664525 + 1013904223`, and clang composed all eight
// into a single multiply-add — the emitted loop was one `imul` by 0xea890021 plus one `add` of 0xa3d95fa8,
// which are exactly 1664525^8 and the matching addend mod 2^32. That silently turned the parallel-for and
// reduction leaves into memory-bandwidth streamers, i.e. the precise failure this file's header warns against:
// they measured the machine, not the scheduler, and capped out around 8 workers. lowbias32 (Chris Wellons'
// hash-prospector search) is xor-shift/multiply and so is not affine over Z_2^32; rounds of it cannot collapse.
CC_FORCE_INLINE i32 mix(i32 x)
{
    cc::u32 h = cc::u32(x);
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

// Hoare partition around the middle element; returns the split point, so the halves are [0, s) and [s, n).
// Only called on spans larger than qsort_cutoff.
isize hoare_partition(cc::span<i32> d)
{
    i32 const pivot = d[d.size() / 2];
    isize i = -1;
    isize j = d.size();
    for (;;)
    {
        do
            ++i;
        while (d[i] < pivot);
        do
            --j;
        while (d[j] > pivot);
        if (i >= j)
            return j + 1;
        std::swap(d[i], d[j]);
    }
}

// --- serial baselines ------------------------------------------------------------------------------------
// noinline so the compiler cannot fold a whole workload away or inline it into a shape the async version has
// no counterpart for. These are the speedup denominators.

CC_DONT_INLINE void serial_quicksort(cc::span<i32> d)
{
    if (d.size() <= qsort_cutoff)
    {
        std::sort(d.begin(), d.end());
        return;
    }
    isize const s = hoare_partition(d);
    if (s <= 0 || s >= d.size()) // degenerate split (all-equal run): fall back rather than recurse forever
    {
        std::sort(d.begin(), d.end());
        return;
    }
    serial_quicksort(d.subspan(cc::start_end{0, s}));
    serial_quicksort(d.subspan(s));
}

CC_DONT_INLINE void serial_pfor(cc::span<i32> d)
{
    mix_range(d);
}

// The reduce and tree baselines take the opaque() barrier because they are otherwise pure functions of
// unchanging inputs: their mutating siblings (quicksort, pfor) cannot be hoisted out of the timing loop, but
// these two can, and did — they measured 0.00 ns until this was added.
CC_DONT_INLINE i64 serial_reduce(cc::span<i32 const> d)
{
    i64 s = sum_range(d);
    opaque(s);
    return s;
}

// Mirrors async_tree's shape: two recursive calls per internal node, a trivial leaf. So the comparison is
// per-node call overhead vs per-node scheduling overhead, which is the point of the case.
CC_DONT_INLINE i64 serial_tree(int depth)
{
    if (depth == 0)
    {
        i64 one = 1;
        opaque(one);
        return one;
    }
    return serial_tree(depth - 1) + serial_tree(depth - 1);
}

// --- async fork-join graphs ------------------------------------------------------------------------------
// Each is a raw two-phase frame: on the first poll it either does the leaf work inline or spawns its children
// and parks; on the re-poll it joins. `l == nullptr` is the "have I spawned yet" test — using it instead of a
// step counter is what keeps these frames inside the 32 B inline slot.
//
// The children are captured, which is what keeps them alive: the pending-dependency list does not own anything.

cc::shared_async<i64> async_quicksort(cc::span<i32> d)
{
    return spawn(
        [d, l = cc::shared_async<i64>(),
         r = cc::shared_async<i64>()](cc::async_context<i64>& actx) mutable -> cc::async_step_status
        {
            if (l == nullptr)
            {
                if (d.size() <= qsort_cutoff)
                {
                    std::sort(d.begin(), d.end());
                    return actx.success(i64(0));
                }
                isize const s = hoare_partition(d);
                if (s <= 0 || s >= d.size())
                {
                    std::sort(d.begin(), d.end());
                    return actx.success(i64(0));
                }
                l = async_quicksort(d.subspan(cc::start_end{0, s}));
                r = async_quicksort(d.subspan(s));
                (void)actx.require(l);
                (void)actx.require(r);
                return actx.wait_for_dependencies();
            }
            return actx.success(i64(0));
        });
}

cc::shared_async<i64> async_pfor(cc::span<i32> d)
{
    return spawn(
        [d, l = cc::shared_async<i64>(),
         r = cc::shared_async<i64>()](cc::async_context<i64>& actx) mutable -> cc::async_step_status
        {
            if (l == nullptr)
            {
                if (d.size() <= pfor_grain)
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
                if (d.size() <= reduce_grain)
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

cc::shared_async<i64> async_nested_inner(cc::span<i32> d)
{
    return spawn(
        [d, l = cc::shared_async<i64>(),
         r = cc::shared_async<i64>()](cc::async_context<i64>& actx) mutable -> cc::async_step_status
        {
            if (l == nullptr)
            {
                if (d.size() <= nested_inner_grain)
                {
                    mix_range(d);
                    return actx.success(i64(0));
                }
                isize const m = d.size() / 2;
                l = async_nested_inner(d.subspan(cc::start_end{0, m}));
                r = async_nested_inner(d.subspan(m));
                (void)actx.require(l);
                (void)actx.require(r);
                return actx.wait_for_dependencies();
            }
            return actx.success(i64(0));
        });
}

// The outer level: bisects to nested_outer_grain, then each leaf spawns ONE child — a whole inner parallel-for
// over its range — and joins on it. `r` stays null in that case.
cc::shared_async<i64> async_nested_outer(cc::span<i32> d)
{
    return spawn(
        [d, l = cc::shared_async<i64>(),
         r = cc::shared_async<i64>()](cc::async_context<i64>& actx) mutable -> cc::async_step_status
        {
            if (l == nullptr)
            {
                if (d.size() <= nested_outer_grain)
                {
                    l = async_nested_inner(d);
                    (void)actx.require(l);
                    return actx.wait_for_dependencies();
                }
                isize const m = d.size() / 2;
                l = async_nested_outer(d.subspan(cc::start_end{0, m}));
                r = async_nested_outer(d.subspan(m));
                (void)actx.require(l);
                (void)actx.require(r);
                return actx.wait_for_dependencies();
            }
            return actx.success(i64(0));
        });
}

// Spawned dynamically rather than built up front: a serially-built tree would put ~131k node constructions on
// the calling thread and measure that instead of the schedule. This is the fork-join spawn shape, and with
// trivial leaves it isolates per-node scheduling overhead.
cc::shared_async<i64> async_tree(int depth)
{
    return spawn(
        [depth, l = cc::shared_async<i64>(),
         r = cc::shared_async<i64>()](cc::async_context<i64>& actx) mutable -> cc::async_step_status
        {
            if (depth == 0)
                return actx.success(i64(1));
            if (l == nullptr)
            {
                l = async_tree(depth - 1);
                r = async_tree(depth - 1);
                (void)actx.require(l);
                (void)actx.require(r);
                return actx.wait_for_dependencies();
            }
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

// The worker counts the full table sweeps. The top entry is P-1, NOT P: blocking_get makes the calling thread
// participate as a worker, so a w-worker pool runs w+1 threads and P would oversubscribe the machine. That is
// not a rounding detail — at 32 workers on 32 hardware threads the extra thread cost the reduction case 2x
// (0.04 vs 0.02 ns/elem) purely in scheduler thrash, which reads as a scheduler defect and is not one.
cc::vector<int> sweep_workers()
{
    int const p = cc::num_hardware_threads() - 1;
    cc::vector<int> ws;
    for (int w : {1, 2, 4, 8})
        if (w < p)
            ws.push_back(w);
    ws.push_back(p < 1 ? 1 : p);
    return ws;
}

// What the guide benchmark measures: just the two ends. 1 worker anchors the scaling ratio, P-1 is the number
// that matters; the intermediate points only shape the human-facing curve.
cc::vector<int> guide_workers()
{
    int const p = cc::num_hardware_threads() - 1;
    cc::vector<int> ws;
    ws.push_back(1);
    ws.push_back(p < 2 ? 2 : p);
    return ws;
}

// What one swept case yields. Two different questions, so two numbers:
//   vs_serial  — "is using the pool worth it at all", the user-facing speedup. Meaningless where the serial
//                analog is trivially cheaper by construction (the spawn tree, whose serial form is a bare
//                recursive call against a whole scheduled node) — read ns_at_p there instead.
//   vs_one     — "does the SCHEDULER scale", pool at P vs pool at 1. Independent of how heavy the leaf work is,
//                so it is the honest scaling number for every case, including the tree.
struct sweep_result
{
    double vs_serial = 0;
    double vs_one = 0;
    double ns_at_p = 0;
};

void print_header(char const* title, char const* unit)
{
    std::printf("\n=== %s ===\n", title);
    std::printf("%-10s %13s %13s %12s %12s\n", "workers", "serial", unit, "vs serial", "vs 1w");
    std::printf("%-10s %13s %13s %12s %12s\n", "-------", "------", "------", "---------", "-----");
}

// Measure one case across `workers` against its serial baseline; print the table.
//
// Two deliberate choices, both learned from wrong numbers:
//
// * The serial baseline is re-measured on EVERY row, immediately next to that row's pool run, and printed as
//   its own column. Measuring it once up front made the speedup ratio a lie: this is a laptop, sustained
//   all-core load downclocks it, so a baseline taken seconds earlier on a cool chip was being divided by a pool
//   time taken on a hot one. The first version of this file read 0.65 ns/elem serial in the full sweep and 0.26
//   in the guide run — same code, same machine, 2.5x apart, purely from what had run before it. Measuring
//   adjacent makes both halves of the ratio see the same machine, and the serial column then doubles as a
//   per-row contamination canary: nothing in this file touches it, so if it drifts down the table, the run is
//   dirty and the ratios in it are not comparable to any other run.
// * The pool is constructed OUTSIDE the timed pass: spawning threads is ~100 us and would otherwise land inside
//   measure_units_per_sec's adaptive loop, where it would dominate every short pass.
template <class SerialPass, class AsyncPass>
sweep_result run_sweep(char const* title,
                       char const* unit,
                       double units,
                       cc::span<int const> workers,
                       SerialPass&& serial,
                       AsyncPass&& async_pass)
{
    print_header(title, unit);

    sweep_result res;
    double one_ns = 0;
    for (int w : workers)
    {
        double const serial_ns = 1e9 / bench::median_units_per_sec(units, serial);

        cc::async_thread_pool pool(w);
        double const ns = 1e9 / bench::median_units_per_sec(units, [&] { return async_pass(pool); });
        if (w == 1)
            one_ns = ns;

        res.ns_at_p = ns;
        res.vs_serial = serial_ns / ns;
        res.vs_one = one_ns > 0 ? one_ns / ns : 0;
        std::printf("%-10d %13.2f %13.2f %11.2fx %11.2fx\n", w, serial_ns, ns, res.vs_serial, res.vs_one);
    }
    std::fflush(stdout);
    return res;
}

// --- cases -----------------------------------------------------------------------------------------------

// Each pass refills the working buffer from a pristine random source: sorting already-sorted data is a
// different (and far easier) workload, so a reused buffer would measure a fiction. The refill is a ~4 MiB
// memcpy against a multi-ms sort, and it is paid identically by the serial and async passes, so it biases the
// absolute ns/elem slightly and the speedup ratio almost not at all.
sweep_result case_quicksort(cc::span<int const> workers)
{
    auto const src = make_random(qsort_n);
    cc::vector<i32> work = src;

    auto const refill = [&] { std::copy(src.begin(), src.end(), work.begin()); };

    return run_sweep(
        "parallel quicksort (1<<20 i32)", "ns/elem", double(qsort_n), workers,
        [&]
        {
            refill();
            serial_quicksort(cc::span<i32>(work));
            return u64(work[0]);
        },
        [&](cc::async_thread_pool& pool)
        {
            refill();
            auto root = async_quicksort(cc::span<i32>(work));
            (void)pool.blocking_get(root);
            return u64(work[0]);
        });
}

sweep_result case_pfor(cc::span<int const> workers)
{
    auto work = make_random(pfor_n);

    return run_sweep(
        "parallel-for transform (1<<22 i32)", "ns/elem", double(pfor_n), workers,
        [&]
        {
            serial_pfor(cc::span<i32>(work));
            return u64(work[0]);
        },
        [&](cc::async_thread_pool& pool)
        {
            auto root = async_pfor(cc::span<i32>(work));
            (void)pool.blocking_get(root);
            return u64(work[0]);
        });
}

sweep_result case_reduce(cc::span<int const> workers)
{
    auto const src = make_random(reduce_n);

    return run_sweep(
        "reduction (1<<22 i32)", "ns/elem", double(reduce_n), workers,
        [&] { return u64(serial_reduce(cc::span<i32 const>(src))); },
        [&](cc::async_thread_pool& pool)
        {
            auto root = async_reduce(cc::span<i32 const>(src));
            return u64(pool.blocking_get(root));
        });
}

sweep_result case_nested(cc::span<int const> workers)
{
    auto work = make_random(nested_n);

    return run_sweep(
        "nested parallel-for (1<<22 i32)", "ns/elem", double(nested_n), workers,
        [&]
        {
            serial_pfor(cc::span<i32>(work));
            return u64(work[0]);
        },
        [&](cc::async_thread_pool& pool)
        {
            auto root = async_nested_outer(cc::span<i32>(work));
            (void)pool.blocking_get(root);
            return u64(work[0]);
        });
}

sweep_result case_tree(cc::span<int const> workers)
{
    constexpr double nodes = double((isize(1) << (tree_depth + 1)) - 1);

    return run_sweep(
        "spawn tree (depth 16, 131071 nodes)", "ns/node", nodes, workers, [&] { return u64(serial_tree(tree_depth)); },
        [&](cc::async_thread_pool& pool)
        {
            auto root = async_tree(tree_depth);
            return u64(pool.blocking_get(root));
        });
}

void run_all()
{
    std::printf("\n### cc::async_thread_pool sweep (median of 5, %d hardware threads; +1 participating caller per row) "
                "###\n",
                cc::num_hardware_threads());

    auto const ws = sweep_workers();
    (void)case_quicksort(ws);
    (void)case_pfor(ws);
    (void)case_reduce(ws);
    (void)case_nested(ws);
    (void)case_tree(ws);

    std::printf("\nHow to read this (the columns are not equally trustworthy):\n");
    std::printf("  serial    re-measured on every row, next to that row's pool run. Nothing here changes it, so\n");
    std::printf("            it is the canary: FLAT down a case = clean; DRIFTING = the machine throttled under\n");
    std::printf("            sustained load and that case's cross-row numbers are not comparable.\n");
    std::printf("  vs serial serial ns / pool ns, an ADJACENT pair -- valid even when the canary drifts.\n");
    std::printf("  vs 1w     the scheduler's own scaling, but 1w and Pw are rows apart in time -- so only trust\n");
    std::printf("            it where the serial column above it is flat.\n");
    std::printf("A 'w worker' row runs w+1 THREADS: blocking_get makes the calling thread participate, so the\n");
    std::printf("sweep tops out at hardware concurrency MINUS ONE and '1w' is a 2-thread config, not a serial\n");
    std::printf("one. Judge near-linearity against the P-core count, not the thread count: the curve bends at\n");
    std::printf("the E-core and SMT boundaries by design. The spawn tree's 'vs serial' is expected to be ~0 and\n");
    std::printf("is not a defect -- its serial analog is a bare recursive call; read its ns/node and 'vs 1w'.\n");
    std::fflush(stdout);
}
} // namespace

// The points that decide the gate, recorded for the perf tracker, at 1 and P workers only.
//
// Deliberately not "speedup" for both: parallel-for is the regular case where speedup-vs-serial is the question
// a user would actually ask, whereas the spawn tree's leaves do nothing at all — so its cost per node IS the
// pool's overhead, and its scaling only means anything against itself at one worker. The full sweep below is
// the human-facing table.
GUIDE_BENCHMARK("bench-async-pool (work-stealing)")
{
    auto const ws = guide_workers();
    auto const pfor = case_pfor(ws);
    auto const tree = case_tree(ws);

    nx::guide::report_raw("parallel-for speedup@P", pfor.vs_serial, "x", /*higher_is_better*/ true);
    nx::guide::report_raw("parallel-for scaling@P (vs 1w)", pfor.vs_one, "x", /*higher_is_better*/ true);
    nx::guide::report_raw("spawn-tree ns/node@P", tree.ns_at_p, "ns/node", /*higher_is_better*/ false);
    nx::guide::report_raw("spawn-tree scaling@P (vs 1w)", tree.vs_one, "x", /*higher_is_better*/ true);
}

// The full canonical set across the worker sweep. Run by exact name:
//   uv run dev.py --mirror-test-output test "bench-async-pool (worker sweep)"
TEST("bench-async-pool (worker sweep)", nx::config::manual)
{
    run_all();
}

#endif // CC_HAS_THREADS
