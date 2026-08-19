#include <clean-core/common/macros.hh> // CC_HAS_THREADS

// The concurrent scheduler needs OS threads; this file compiles to nothing where they are unavailable (wasm).
#if CC_HAS_THREADS

// cc::async_thread_pool benchmark: the concurrent half of the async performance picture.
//
// async-benchmark.cc answers the single-thread question; this one asks whether a graph gets near-linear speedup on the regular cases, and what a fork-join task costs.
// Every case is a hand-written fork-join graph over the raw primitives, since no ergonomic parallel_for / reduce helpers exist yet.
// libs/base/clean-core/docs/benchmarks/async-benchmark.md has the method, the numbers, and how much to trust each column of a run.
//
// The five canonical shapes:
//   parallel quicksort   recursive, irregular subproblem sizes -> the steal-quality stress
//   parallel-for         recursive bisection, perfectly balanced -> the near-linear-speedup case
//   reduction            bisect + join, values flow back up the tree
//   nested parallel-for  a parallel-for whose leaf is itself a parallel-for -> deque depth + nested spawn
//   spawn tree           trivial leaves, so per-node scheduling overhead IS the measurement
//
// Two constraints bind every edit here, and breaking either corrupts the numbers silently.
// Leaf work must stay compute-bound — see mix(), which clang folded into a single multiply-add until it was made non-affine.
// And every fork-join frame must stay at or under the node's 32 B inline slot, which is why the grain sizes are namespace-scope constants rather than captures.
// A two-child frame captures exactly span(16) + two shared_async(8+8), so adding one capture to any of these silently changes what is measured.

#include "../bench_util.hh"

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/guide.hh>
#include <nexus/test.hh>

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
// Asks async<i64> for the real predicate rather than restating the budget: a hand-copied number goes stale silently,
// and a silent spill is exactly the failure this assert exists to prevent.
// The zero-dependency make_async_lazy form wraps the frame in a lambda whose only capture is the frame itself,
// so sizeof(F) IS the installed frame size.
// The variadic dep form would additionally store the dep handles, and none of these use it.
template <class F>
[[nodiscard]] cc::shared_async<i64> spawn(F&& f)
{
    static_assert(cc::async<i64>::frame_fits_inline<std::decay_t<F>>,
                  "fork-join frame spilled out of the node's inline slot into a heap-boxed unique_function "
                  "— that is an allocation per task, and it would make this a benchmark of malloc rather than "
                  "of the scheduler. Drop a capture (see the grain constants) rather than relaxing this.");
    return cc::make_async_lazy<i64>(cc::forward<F>(f));
}

// Base of the buffer the case currently running works on, set by that case.
// A global for the same reason the grain constants are: it keeps the fork-join frames inside the node's inline slot.
// Cases run one at a time, so one slot is enough.
i32* g_data = nullptr;

/// A subrange of g_data as two i32 — deliberately not a cc::span.
/// A span is 16 B, which alongside the two child handles would make the frame 32 B and spill it out of the node's 24 B inline slot; see spawn().
struct range
{
    i32 off = 0;
    i32 count = 0;

    [[nodiscard]] cc::span<i32> mut() const { return cc::span<i32>(g_data + off, count); }
    [[nodiscard]] cc::span<i32 const> get() const { return cc::span<i32 const>(g_data + off, count); }

    /// Split at `k`, which must be in [0, count] — the two halves partition this range exactly.
    [[nodiscard]] range head(i32 k) const { return {.off = off, .count = k}; }
    [[nodiscard]] range tail(i32 k) const { return {.off = off + k, .count = count - k}; }
};
static_assert(sizeof(range) == 8, "the fork-join frames are sized around this being one word");

// --- workload constants (namespace scope so frames stay inside the inline slot; see the header note) -----

constexpr isize qsort_n = 1 << 20;   // 4 MiB of i32
constexpr isize qsort_cutoff = 1024; // below this a subproblem is sorted serially

constexpr isize pfor_n = 1 << 22;
constexpr isize pfor_grain = 8192;

constexpr isize reduce_n = 1 << 22;
constexpr isize reduce_grain = 8192;

// nested: the outer bisects down to outer_grain, then each leaf spawns an inner parallel-for over its own range.
// Two frame types, each with its own grain, so neither has to capture one.
constexpr isize nested_n = 1 << 22;
constexpr isize nested_outer_grain = 1 << 16;
constexpr isize nested_inner_grain = 4096;

constexpr int tree_depth = 16; // 2^17 - 1 = 131071 nodes, trivial leaves

// --- leaf work -------------------------------------------------------------------------------------------

// An empty inline-asm barrier: it makes `v` un-analyzable and, being volatile, gives its containing function side effects.
// The side effects are the load-bearing half.
// Without them a CC_DONT_INLINE serial baseline is still a pure function of unchanging arguments, so clang hoists the whole call out of the timing loop.
// The baseline then measures 0.00 ns.
CC_FORCE_INLINE void opaque(i64& v)
{
#if defined(__clang__) || defined(__GNUC__)
    __asm__ volatile("" : "+r"(v));
#else
    i64 volatile tmp = v; // MSVC fallback: a volatile round-trip the optimizer cannot see through
    v = tmp;
#endif
}

// Cheap, data-dependent, compute-bound mixing, so the benchmark measures scheduling rather than DRAM bandwidth.
// Shared verbatim by the serial baselines and the async leaves — the comparison is only honest if both do exactly the same arithmetic.
//
// It must NOT be affine, and that is the one constraint to check before changing it.
// An affine mix composes: a chain of `x = x*a + b` rounds collapses into a single multiply-add, which turns the parallel-for and reduction leaves into memory-bandwidth streamers.
// lowbias32 (Chris Wellons' hash-prospector search) is xor-shift/multiply, so it is not affine over Z_2^32 and its rounds cannot collapse.
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
// noinline so the compiler cannot fold a whole workload away, or inline it into a shape the async version has no counterpart for.
// These are the speedup denominators.

CC_DONT_INLINE void serial_quicksort(cc::span<i32> d)
{
    if (d.size() <= qsort_cutoff)
    {
        cc::sort(d);
        return;
    }
    isize const s = hoare_partition(d);
    if (s <= 0 || s >= d.size()) // degenerate split (all-equal run): fall back rather than recurse forever
    {
        cc::sort(d);
        return;
    }
    serial_quicksort(d.subspan(cc::start_end{0, s}));
    serial_quicksort(d.subspan(s));
}

CC_DONT_INLINE void serial_pfor(cc::span<i32> d)
{
    mix_range(d);
}

// The reduce and tree baselines take the opaque() barrier because they are otherwise pure functions of unchanging inputs.
// Their mutating siblings, quicksort and pfor, cannot be hoisted out of the timing loop, but these two can --
// and measured 0.00 ns until the barrier was added.
CC_DONT_INLINE i64 serial_reduce(cc::span<i32 const> d)
{
    i64 s = sum_range(d);
    opaque(s);
    return s;
}

// Mirrors async_tree's shape: two recursive calls per internal node, a trivial leaf.
// So the comparison is per-node call overhead vs per-node scheduling overhead, which is the point of the case.
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

// Each is a raw two-phase frame: on the first poll it either does the leaf work inline or spawns its children and parks; on the re-poll it joins.
// `l == nullptr` is the "have I spawned yet" test, and using it instead of a step counter is what keeps these frames inside the 32 B inline slot.
//
// The children are captured, which is what keeps them alive: the pending-dependency list does not own anything.

cc::shared_async<i64> async_quicksort(range rg)
{
    return spawn(
        [rg, l = cc::shared_async<i64>(),
         r = cc::shared_async<i64>()](cc::async_context<i64>& actx) mutable -> cc::async_step_status
        {
            if (l == nullptr)
            {
                auto const d = rg.mut();
                if (rg.count <= qsort_cutoff)
                {
                    cc::sort(d);
                    return actx.success(i64(0));
                }
                auto const s = i32(hoare_partition(d));
                if (s <= 0 || s >= rg.count)
                {
                    cc::sort(d);
                    return actx.success(i64(0));
                }
                l = async_quicksort(rg.head(s));
                r = async_quicksort(rg.tail(s));
                (void)actx.require(l);
                (void)actx.require(r);
                return actx.wait_for_dependencies();
            }
            return actx.success(i64(0));
        });
}

cc::shared_async<i64> async_pfor(range rg)
{
    return spawn(
        [rg, l = cc::shared_async<i64>(),
         r = cc::shared_async<i64>()](cc::async_context<i64>& actx) mutable -> cc::async_step_status
        {
            if (l == nullptr)
            {
                if (rg.count <= pfor_grain)
                {
                    mix_range(rg.mut());
                    return actx.success(i64(0));
                }
                auto const m = rg.count / 2;
                l = async_pfor(rg.head(m));
                r = async_pfor(rg.tail(m));
                (void)actx.require(l);
                (void)actx.require(r);
                return actx.wait_for_dependencies();
            }
            return actx.success(i64(0));
        });
}

cc::shared_async<i64> async_reduce(range rg)
{
    return spawn(
        [rg, l = cc::shared_async<i64>(),
         r = cc::shared_async<i64>()](cc::async_context<i64>& actx) mutable -> cc::async_step_status
        {
            if (l == nullptr)
            {
                if (rg.count <= reduce_grain)
                    return actx.success(sum_range(rg.get()));
                auto const m = rg.count / 2;
                l = async_reduce(rg.head(m));
                r = async_reduce(rg.tail(m));
                (void)actx.require(l);
                (void)actx.require(r);
                return actx.wait_for_dependencies();
            }
            // by-value resolve: the sum is a temporary evaluated before the call, so it does not outlive the
            // frame that the resolve destroys
            return actx.success(*l->value_ptr() + *r->value_ptr());
        });
}

cc::shared_async<i64> async_nested_inner(range rg)
{
    return spawn(
        [rg, l = cc::shared_async<i64>(),
         r = cc::shared_async<i64>()](cc::async_context<i64>& actx) mutable -> cc::async_step_status
        {
            if (l == nullptr)
            {
                if (rg.count <= nested_inner_grain)
                {
                    mix_range(rg.mut());
                    return actx.success(i64(0));
                }
                auto const m = rg.count / 2;
                l = async_nested_inner(rg.head(m));
                r = async_nested_inner(rg.tail(m));
                (void)actx.require(l);
                (void)actx.require(r);
                return actx.wait_for_dependencies();
            }
            return actx.success(i64(0));
        });
}

// The outer level: bisects to nested_outer_grain, then each leaf spawns ONE child — a whole inner parallel-for over its range — and joins on it.
// `r` stays null in that case.
cc::shared_async<i64> async_nested_outer(range rg)
{
    return spawn(
        [rg, l = cc::shared_async<i64>(),
         r = cc::shared_async<i64>()](cc::async_context<i64>& actx) mutable -> cc::async_step_status
        {
            if (l == nullptr)
            {
                if (rg.count <= nested_outer_grain)
                {
                    l = async_nested_inner(rg);
                    (void)actx.require(l);
                    return actx.wait_for_dependencies();
                }
                auto const m = rg.count / 2;
                l = async_nested_outer(rg.head(m));
                r = async_nested_outer(rg.tail(m));
                (void)actx.require(l);
                (void)actx.require(r);
                return actx.wait_for_dependencies();
            }
            return actx.success(i64(0));
        });
}

// Spawned dynamically rather than built up front: a serially-built tree would put ~131k node constructions on the calling thread and measure that instead of the schedule.
// This is the fork-join spawn shape, and with trivial leaves it isolates per-node scheduling overhead.
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

// The worker counts the full table sweeps.
// The top entry is P-1, NOT P: driving makes the calling thread participate as a worker, so a w-worker pool runs w+1 threads and P would oversubscribe the machine.
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

// What the guide benchmark measures: just the two ends.
// 1 worker anchors the scaling ratio and P-1 is the number that matters, so the intermediate points only shape the human-facing curve.
cc::vector<int> guide_workers()
{
    int const p = cc::num_hardware_threads() - 1;
    cc::vector<int> ws;
    ws.push_back(1);
    ws.push_back(p < 2 ? 2 : p);
    return ws;
}

// What one swept case yields.
// Two different questions, so two numbers:
//   vs_serial  — "is using the pool worth it at all", the user-facing speedup.
//                Meaningless where the serial analog is trivially cheaper by construction, so read ns_at_p there instead.
//                That is the spawn tree, whose serial form is a bare recursive call against a whole scheduled node.
//   vs_one     — "does the SCHEDULER scale", pool at P vs pool at 1.
//                Independent of how heavy the leaf work is, so it is the honest scaling number for every case, including the tree.
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

// Measure one case across `workers` against its serial baseline, and print the table.
//
// Two deliberate choices, both learned from wrong numbers and both explained in the benchmark doc:
//   * the serial baseline is re-measured on EVERY row, immediately next to that row's pool run, and printed as its own column, where it doubles as the run's contamination canary;
//   * the pool is constructed OUTSIDE the timed pass, since spawning threads is ~100 us and would otherwise land inside measure_units_per_sec's adaptive loop.
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

// Each pass refills the working buffer from a pristine random source.
// Sorting already-sorted data is a different, and far easier, workload, so a reused buffer would measure a fiction.
// The refill is a ~4 MiB memcpy against a multi-ms sort, and it is paid identically by the serial and async passes.
// So it biases the absolute ns/elem slightly, and the speedup ratio almost not at all.
sweep_result case_quicksort(cc::span<int const> workers)
{
    auto const src = make_random(qsort_n);
    cc::vector<i32> work = src;
    g_data = work.data(); // the frames address the buffer through this; see `range`

    auto const refill = [&] { std::copy(src.begin(), src.end(), work.begin()); };
    auto const whole = range{.off = 0, .count = i32(qsort_n)};

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
            auto root = async_quicksort(whole);
            pool.participate_until_ready(*root);
            return u64(work[0]);
        });
}

sweep_result case_pfor(cc::span<int const> workers)
{
    auto work = make_random(pfor_n);
    g_data = work.data();
    auto const whole = range{.off = 0, .count = i32(pfor_n)};

    return run_sweep(
        "parallel-for transform (1<<22 i32)", "ns/elem", double(pfor_n), workers,
        [&]
        {
            serial_pfor(cc::span<i32>(work));
            return u64(work[0]);
        },
        [&](cc::async_thread_pool& pool)
        {
            auto root = async_pfor(whole);
            pool.participate_until_ready(*root);
            return u64(work[0]);
        });
}

sweep_result case_reduce(cc::span<int const> workers)
{
    auto src = make_random(reduce_n);
    g_data = src.data();
    auto const whole = range{.off = 0, .count = i32(reduce_n)};

    return run_sweep(
        "reduction (1<<22 i32)", "ns/elem", double(reduce_n), workers,
        [&] { return u64(serial_reduce(cc::span<i32 const>(src))); },
        [&](cc::async_thread_pool& pool)
        {
            auto root = async_reduce(whole);
            pool.participate_until_ready(*root);
            return u64(*root->try_value());
        });
}

sweep_result case_nested(cc::span<int const> workers)
{
    auto work = make_random(nested_n);
    g_data = work.data();
    auto const whole = range{.off = 0, .count = i32(nested_n)};

    return run_sweep(
        "nested parallel-for (1<<22 i32)", "ns/elem", double(nested_n), workers,
        [&]
        {
            serial_pfor(cc::span<i32>(work));
            return u64(work[0]);
        },
        [&](cc::async_thread_pool& pool)
        {
            auto root = async_nested_outer(whole);
            pool.participate_until_ready(*root);
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
            pool.participate_until_ready(*root);
            return u64(*root->try_value());
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
    std::printf("A 'w worker' row runs w+1 THREADS: driving makes the calling thread participate, so the\n");
    std::printf("sweep tops out at hardware concurrency MINUS ONE and '1w' is a 2-thread config, not a serial\n");
    std::printf("one. Judge near-linearity against the P-core count, not the thread count: the curve bends at\n");
    std::printf("the E-core and SMT boundaries by design. The spawn tree's 'vs serial' is expected to be ~0 and\n");
    std::printf("is not a defect -- its serial analog is a bare recursive call; read its ns/node and 'vs 1w'.\n");
    std::fflush(stdout);
}
} // namespace

// The points that decide the gate, recorded for the perf tracker, at 1 and P workers only.
//
// Deliberately not "speedup" for both: parallel-for is the regular case, where speedup-vs-serial is the question a user would actually ask.
// The spawn tree's leaves do nothing at all, so its cost per node IS the pool's overhead, and its scaling only means anything against itself at one worker.
// The full sweep below is the human-facing table.
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

// The full canonical set across the worker sweep.
// Run by exact name:
//   uv run dev.py --mirror-test-output test "bench-async-pool (worker sweep)"
TEST("bench-async-pool (worker sweep)", nx::config::manual)
{
    run_all();
}

#endif // CC_HAS_THREADS
