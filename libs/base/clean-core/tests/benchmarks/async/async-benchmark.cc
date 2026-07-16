// Single-thread cc::async drive benchmark — Phase 1 of the async performance gate.
//
// Measures the "async tax": the per-async overhead of create -> drive-to-completion -> destroy on ONE
// thread (a singlethreaded_scheduler), with nothing multithreaded involved. If this is expensive, no
// work-stealing work matters, so it is measured first.
//
// For each case we build a small async graph, drive it to completion with a reused singlethreaded_scheduler +
// async_worker_scope, and read the result zero-copy via try_value(). Every case is reported next to a
// hand-written DIRECT baseline that computes the same thing, so the tax (async ns / direct ns) is
// explicit. One "op" = one async node processed; units are chosen so the reported rate is nodes/second,
// hence Mop/s = nodes/s / 1e6 and ns/op = 1e9 / (nodes/s). Each number is the median of 5 measurements
// (each measurement does its own prewarm via bench::measure_units_per_sec).
//
// Anti-constant-fold discipline (so the tax is not skewed by the compiler collapsing the baseline to a
// constant): (1) every graph's leaf value is seeded from the runtime loop index g, so each graph
// computes different, data-dependent output; (2) the direct baseline mirrors the graph shape through
// CC_DONT_INLINE step functions, so a chain of x+1 cannot strength-reduce to x+N. The baseline is thus a
// realistic hand-written call-based analog, NOT maximally-optimized straight-line code — that is the
// honest per-node comparison. Results are XOR-folded into the u64 returned from each pass -> bench::sink.
//
// Two entry points: a GUIDE_BENCHMARK recording three representative points for the perf gate (this is what
// guards Phase 1's result against regression), and a manual full sweep printing the whole table. Neither runs
// in the normal test sweep; both are reachable by exact name.

#include "../bench_util.hh"

#include <clean-core/common/macros.hh>
#include <clean-core/thread/async.hh>
#include <nexus/guide.hh>
#include <nexus/test.hh>

#include <cstdio>

using cc::i64;
using cc::isize;
using cc::u64;

namespace
{
// --- direct (hand-written) baselines --------------------------------------------------------------------
// noinline is not enough on its own: clang infers these are pure and propagates their return values, so a
// run of steps still folds to a closed form (x+N) or a compile-time constant — the exact skew we must
// avoid. `opaque` is an empty inline-asm barrier that makes its argument un-analyzable, so the compiler
// can neither fold the arithmetic nor prove the return relationship: each direct_* stays a genuine,
// non-collapsing call — the honest cost of the equivalent hand-written call-based computation per node.
CC_FORCE_INLINE void opaque(i64& v)
{
#if defined(__clang__) || defined(__GNUC__)
    __asm__ volatile("" : "+r"(v));
#else
    i64 volatile tmp = v; // MSVC fallback: a volatile round-trip the optimizer cannot see through
    v = tmp;
#endif
}

CC_DONT_INLINE i64 direct_leaf(i64 seed)
{
    opaque(seed);
    return seed;
}
CC_DONT_INLINE i64 direct_step(i64 x)
{
    opaque(x);
    return x + 1;
}
CC_DONT_INLINE i64 direct_add(i64 a, i64 b)
{
    opaque(a);
    opaque(b);
    return a + b;
}

// Direct analog of the balanced sum-tree: leaf/add are noinline, so the whole tree is not folded to a
// constant. Call count ~= node count, matching the async tree.
i64 direct_sum_tree(int depth, i64 seed)
{
    if (depth == 0)
        return direct_leaf(seed);
    return direct_add(direct_sum_tree(depth - 1, seed), direct_sum_tree(depth - 1, seed));
}

// --- async builders -------------------------------------------------------------------------------------

// A cold chain of N nodes: leaf(seed) then (N-1) x+1 transforms. Result == seed + (N-1).
cc::shared_async<i64> build_chain(int n, i64 seed)
{
    auto node = cc::make_async_lazy<i64>([seed] { return seed; });
    for (int i = 1; i < n; ++i)
        node = cc::make_async_lazy([](i64 x) { return x + 1; }, cc::move(node));
    return node;
}

// A balanced binary sum-tree of the given depth (2^(depth+1) - 1 nodes). Leaves return `seed`, internal
// nodes sum their two children. Mirrors the correctness test's build_sum_tree, without the leaf counter.
cc::shared_async<i64> build_sum_tree(int depth, i64 seed)
{
    if (depth == 0)
        return cc::make_async_lazy<i64>([seed] { return seed; });
    auto left = build_sum_tree(depth - 1, seed);
    auto right = build_sum_tree(depth - 1, seed);
    return cc::make_async_lazy([](i64 l, i64 r) { return l + r; }, left, right);
}

// --- measurement plumbing -------------------------------------------------------------------------------

// One table row: async vs direct, in Mop/s (nodes/s) and ns/op (per node), plus the tax = async / direct.
// `record` also files the async ns/node as a guide metric — see run_guide().
void report(char const* label, isize nodes, double async_ops_per_sec, double direct_ops_per_sec, bool record)
{
    double const a_mops = async_ops_per_sec / 1e6;
    double const a_ns = 1e9 / async_ops_per_sec;
    double const d_ns = 1e9 / direct_ops_per_sec;
    double const tax = a_ns / d_ns;
    std::printf("%-22s %8lld %13.1f %13.2f %14.2f %9.1fx\n", label, (long long)nodes, a_mops, a_ns, d_ns, tax);

    if (record)
        nx::guide::report_raw(label, a_ns, "ns/node", /*higher_is_better*/ false);
}

// Drive a freshly-built root to completion on the calling thread's scheduler, return its value.
//
// The scheduler is reused across every iteration, so this only measures the graph if the queue settles on its
// own. It does: a singlethreaded_scheduler has no steal-capable peers, so the poll loop never publishes a
// dependency it is about to drive inline, and nothing is left queued to pin nodes alive. Were that not so, the
// live-node set would grow without bound and this would degrade into a memory-growth benchmark.
i64 drive(cc::singlethreaded_scheduler& sched, cc::shared_async<i64> const& root)
{
    root->schedule();
    sched.run_until([&] { return root->is_ready(); });
    return *root->try_value(); // zero-copy; ready by construction
}

// One full single-lazy cycle — create, schedule, drive, read, destroy — pinned as one searchable symbol so its
// codegen can be disassembled or traced directly:
//   dev.py assembly trace --target clean-core-test --symbol single_lazy_probe --skip 2 \
//     -- "bench-async (single-thread drive)"
// This is the "single lazy inline" row: the born-ready floor plus the scheduler push/pop, try_begin_running,
// one poll turn through the (inline) frame, finish_value, and teardown. `sched` must already be bound by an
// async_worker_scope. Kept alive by a reference from the test below (a TU-local noinline function is otherwise
// dead-code-eliminated).
CC_DONT_INLINE u64 single_lazy_probe(cc::singlethreaded_scheduler& sched, i64 seed)
{
    auto n = cc::make_async_lazy<i64>([seed] { return seed; });
    n->schedule();
    sched.run_until([&] { return n->is_ready(); });
    return u64(*n->try_value());
}

// graphs per timed pass, chosen so a pass is comfortably above timer noise while keeping the live set to a
// single in-flight graph (each is built, driven, read, and destroyed within the loop body).
int graphs_for(isize nodes)
{
    isize const g = 4096 / nodes;
    return int(g < 1 ? 1 : g);
}

// --- cases (simplest -> up) -----------------------------------------------------------------------------

// Floor: born-ready node (no scheduler, no frame) — node alloc + one finish + teardown.
void case_born_ready(bool record)
{
    constexpr isize nodes = 1;
    int const G = graphs_for(nodes);
    double const units = double(nodes) * G;

    double const a = bench::median_units_per_sec(units,
                                                 [&]
                                                 {
                                                     u64 acc = 0;
                                                     for (int g = 0; g < G; ++g)
                                                     {
                                                         auto n = cc::make_async_from_value(i64(g));
                                                         acc ^= u64(*n->try_value());
                                                     }
                                                     return acc;
                                                 });
    double const d = bench::median_units_per_sec(units,
                                                 [&]
                                                 {
                                                     u64 acc = 0;
                                                     for (int g = 0; g < G; ++g)
                                                         acc ^= u64(direct_leaf(i64(g)));
                                                     return acc;
                                                 });
    report("born-ready read", nodes, a, d, record);
}

// Single lazy node driven inline: node alloc + closure + one poll + finish + teardown + scheduler push/pop.
void case_single_lazy(cc::singlethreaded_scheduler& sched, bool record)
{
    constexpr isize nodes = 1;
    int const G = graphs_for(nodes);
    double const units = double(nodes) * G;

    double const a = bench::median_units_per_sec(units,
                                                 [&]
                                                 {
                                                     u64 acc = 0;
                                                     for (int g = 0; g < G; ++g)
                                                     {
                                                         i64 const seed = i64(g);
                                                         auto n = cc::make_async_lazy<i64>([seed] { return seed; });
                                                         acc ^= u64(drive(sched, n));
                                                     }
                                                     return acc;
                                                 });
    double const d = bench::median_units_per_sec(units,
                                                 [&]
                                                 {
                                                     u64 acc = 0;
                                                     for (int g = 0; g < G; ++g)
                                                         acc ^= u64(direct_leaf(i64(g)));
                                                     return acc;
                                                 });
    report("single lazy inline", nodes, a, d, record);
}

// Single-dependency transform a -> b: the two-phase frame (register dep, wait, compute).
void case_single_dep(cc::singlethreaded_scheduler& sched, bool record)
{
    constexpr isize nodes = 2;
    int const G = graphs_for(nodes);
    double const units = double(nodes) * G;

    double const a
        = bench::median_units_per_sec(units,
                                      [&]
                                      {
                                          u64 acc = 0;
                                          for (int g = 0; g < G; ++g)
                                          {
                                              i64 const seed = i64(g);
                                              auto n0 = cc::make_async_lazy<i64>([seed] { return seed; });
                                              auto n1 = cc::make_async_lazy([](i64 x) { return x + 1; }, cc::move(n0));
                                              acc ^= u64(drive(sched, n1));
                                          }
                                          return acc;
                                      });
    double const d = bench::median_units_per_sec(units,
                                                 [&]
                                                 {
                                                     u64 acc = 0;
                                                     for (int g = 0; g < G; ++g)
                                                         acc ^= u64(direct_step(direct_leaf(i64(g))));
                                                     return acc;
                                                 });
    report("single-dep a->b", nodes, a, d, record);
}

// Deep linear chain: amortized per-node cost. `n` straddles the inline depth cap (async_max_inline_depth
// == 128): below it the drive is depth-first inline, above it the poll loop falls back to subscribe+park.
void case_chain(cc::singlethreaded_scheduler& sched, char const* label, int n, bool record)
{
    isize const nodes = n;
    int const G = graphs_for(nodes);
    double const units = double(nodes) * G;

    double const a = bench::median_units_per_sec(units,
                                                 [&]
                                                 {
                                                     u64 acc = 0;
                                                     for (int g = 0; g < G; ++g)
                                                         acc ^= u64(drive(sched, build_chain(n, i64(g))));
                                                     return acc;
                                                 });
    double const d = bench::median_units_per_sec(units,
                                                 [&]
                                                 {
                                                     u64 acc = 0;
                                                     for (int g = 0; g < G; ++g)
                                                     {
                                                         i64 x = direct_leaf(i64(g));
                                                         for (int i = 1; i < n; ++i)
                                                             x = direct_step(x);
                                                         acc ^= u64(x);
                                                     }
                                                     return acc;
                                                 });
    report(label, nodes, a, d, record);
}

// Fan-in c = f(a, b): per-dep unwrap + short-circuit on a two-leaf sum.
void case_fan_in(cc::singlethreaded_scheduler& sched, bool record)
{
    constexpr isize nodes = 3;
    int const G = graphs_for(nodes);
    double const units = double(nodes) * G;

    double const a
        = bench::median_units_per_sec(units,
                                      [&]
                                      {
                                          u64 acc = 0;
                                          for (int g = 0; g < G; ++g)
                                          {
                                              i64 const s = i64(g);
                                              auto la = cc::make_async_lazy<i64>([s] { return s; });
                                              auto lb = cc::make_async_lazy<i64>([s] { return s + 1; });
                                              auto c = cc::make_async_lazy([](i64 l, i64 r) { return l + r; }, la, lb);
                                              acc ^= u64(drive(sched, c));
                                          }
                                          return acc;
                                      });
    double const d
        = bench::median_units_per_sec(units,
                                      [&]
                                      {
                                          u64 acc = 0;
                                          for (int g = 0; g < G; ++g)
                                              acc ^= u64(direct_add(direct_leaf(i64(g)), direct_leaf(i64(g) + 1)));
                                          return acc;
                                      });
    report("fan-in c=f(a,b)", nodes, a, d, record);
}

// Balanced sum-tree driven single-threaded: per-node cost at scale (depth 13 -> 16383 nodes).
void case_sum_tree(cc::singlethreaded_scheduler& sched, int depth, bool record)
{
    isize const nodes = (isize(1) << (depth + 1)) - 1;
    int const G = graphs_for(nodes); // == 1 for depth 13
    double const units = double(nodes) * G;

    double const a = bench::median_units_per_sec(units,
                                                 [&]
                                                 {
                                                     u64 acc = 0;
                                                     for (int g = 0; g < G; ++g)
                                                         acc ^= u64(drive(sched, build_sum_tree(depth, i64(g))));
                                                     return acc;
                                                 });
    double const d = bench::median_units_per_sec(units,
                                                 [&]
                                                 {
                                                     u64 acc = 0;
                                                     for (int g = 0; g < G; ++g)
                                                         acc ^= u64(direct_sum_tree(depth, i64(g)));
                                                     return acc;
                                                 });
    report("sum-tree (depth 13)", nodes, a, d, record);
}

void print_header()
{
    std::printf("%-22s %8s %13s %13s %14s %10s\n", "case", "nodes", "async Mop/s", "async ns/op", "direct ns/op", "tax");
    std::printf("%-22s %8s %13s %13s %14s %10s\n", "----", "-----", "-----------", "-----------", "------------", "---");
}

void run_all(bool record)
{
    std::printf("\n=== cc::async single-thread drive (median of 5) ===\n");
    print_header();

    case_born_ready(record);

    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope scope(sched); // bind once; reused across every driven case

    case_single_lazy(sched, record);
    case_single_dep(sched, record);
    case_chain(sched, "chain N=64 (in-cap)", 64, record);
    case_chain(sched, "chain N=512 (>cap)", 512, record);
    case_fan_in(sched, record);
    case_sum_tree(sched, 13, record);

    std::printf("\nop = one async node (create -> drive -> destroy). tax = async ns/op / direct ns/op\n");
    std::printf("direct = hand-written non-inlined analog (not folded); baseline must read non-zero.\n");
    std::fflush(stdout);
}

// The three points the guide benchmark records. Chosen to cover the distinct cost shapes with the fewest
// measurements: the undriven floor, one full scheduler round-trip, and the amortized per-node cost at scale
// (which is also the only case that exercises the dependency path). The full sweep's other rows are
// interpolations between these, so they add runtime without adding regression coverage — and guide benchmarks
// are swept across every binary by dev.py pgo.
void run_guide()
{
    std::printf("\n=== cc::async single-thread drive (guide points, median of 5) ===\n");
    print_header();

    case_born_ready(/*record*/ true);

    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope const scope(sched);

    case_single_lazy(sched, /*record*/ true);
    case_sum_tree(sched, 13, /*record*/ true);
    std::fflush(stdout);
}
} // namespace

// The regression guard for Phase 1's single-thread result: three representative points, recorded for the perf
// gate. Also hosts the disassembly probe, so the documented trace command targets this (leaner) test:
//   uv run dev.py assembly trace --target clean-core-test --symbol single_lazy_probe --skip 2 --stats \
//     -- "bench-async (single-thread drive)"
// An exact (non-wildcard) name runs a test regardless of bucket, so this is also reachable via a plain
// `dev.py test "bench-async (single-thread drive)"`.
GUIDE_BENCHMARK("bench-async (single-thread drive)")
{
    run_guide();

    // Keep the disassembly probe alive (TU-local + noinline would otherwise be dead-code-eliminated). Called
    // repeatedly on ONE scheduler so a trace can skip past the cold hits: the first enqueue grows the
    // scheduler's queue vector from zero capacity (a real mimalloc call), which the reused-scheduler steady
    // state never pays. Trace the settled path with --skip 2.
    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope const scope(sched);
    for (i64 i = 0; i < 3; ++i)
        bench::sink ^= single_lazy_probe(sched, 7 + i);
}

// The full human-facing table: every case, no recording. Run by exact name, e.g.
//   uv run dev.py --mirror-test-output test "bench-async (single-thread drive - full sweep)"
//
// No comma in the name: nexus splits a filter on commas, so "a, b" is two filters and would also run whatever
// else happens to contain " b" (a comma here matched bench-hash's own "..., full sweep" and ran that instead).
TEST("bench-async (single-thread drive - full sweep)", nx::config::manual)
{
    run_all(/*record*/ false);
}
