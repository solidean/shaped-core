// Single-thread cc::async drive benchmark: the per-async cost of create -> drive-to-completion -> destroy on ONE thread, with nothing multithreaded involved.
// It is measured first because if this is expensive, no amount of work-stealing quality can rescue the concurrent case.
// What each case is for, the anti-fold discipline behind the baselines, and the numbers are in libs/base/clean-core/docs/benchmarks/async-benchmark.md.
//
// Each case builds a small async graph, drives it on a reused singlethreaded_scheduler + async_worker_scope, and reads the result zero-copy via try_value().
// Every case is reported next to a hand-written DIRECT baseline computing the same thing, so the tax (async ns / direct ns) is explicit.
// One "op" = one async node processed, so the reported rate is nodes/second: Mop/s = nodes/s / 1e6, ns/op = 1e9 / (nodes/s).
// Each case is its own BENCHMARK of two loops — the async graph and the equivalent direct call — so the
// comparison column IS the tax, against the direct row as baseline.
//
// Two entry points, neither in the normal test sweep and both reachable by exact name.
// A PGO_BENCHMARK recording three representative points for the perf gate, and a manual full sweep printing the whole table.


#include <clean-core/common/macros.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <nexus/bench/run.hh>
#include <nexus/bench/units.hh>
#include <nexus/pgo.hh>
#include <nexus/test.hh>

#include <cstdio>

using cc::i64;
using cc::isize;
using cc::u64;

namespace
{
// --- direct (hand-written) baselines --------------------------------------------------------------------
// noinline is not enough on its own: clang infers these are pure and propagates their return values, so a run of steps still folds to a closed form (x+N) or a constant.
// `opaque` is an empty inline-asm barrier that makes its argument un-analyzable, so the compiler can neither fold the arithmetic nor prove the return relationship.
// Each direct_* therefore stays a genuine, non-collapsing call — the honest cost of the equivalent hand-written call-based computation per node.
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

// Direct analog of the balanced sum-tree: leaf/add are noinline, so the whole tree is not folded to a constant.
// Call count ~= node count, matching the async tree.
i64 direct_sum_tree(int depth, i64 seed)
{
    if (depth == 0)
        return direct_leaf(seed);
    return direct_add(direct_sum_tree(depth - 1, seed), direct_sum_tree(depth - 1, seed));
}

// --- async builders -------------------------------------------------------------------------------------

// A cold chain of N nodes: leaf(seed) then (N-1) x+1 transforms.
// Result == seed + (N-1).
cc::shared_async<i64> build_chain(int n, i64 seed)
{
    auto node = cc::make_async_lazy<i64>([seed] { return seed; });
    for (int i = 1; i < n; ++i)
        node = cc::make_async_lazy([](i64 x) { return x + 1; }, cc::move(node));
    return node;
}

// A balanced binary sum-tree of the given depth (2^(depth+1) - 1 nodes).
// Leaves return `seed`, internal nodes sum their two children.
// Mirrors the correctness test's build_sum_tree, without the leaf counter.
cc::shared_async<i64> build_sum_tree(int depth, i64 seed)
{
    if (depth == 0)
        return cc::make_async_lazy<i64>([seed] { return seed; });
    auto left = build_sum_tree(depth - 1, seed);
    auto right = build_sum_tree(depth - 1, seed);
    return cc::make_async_lazy([](i64 l, i64 r) { return l + r; }, left, right);
}

// --- measurement plumbing -------------------------------------------------------------------------------

/// One case: the equivalent direct call, then the async graph.
///
/// Direct is declared first, so it is the table's baseline and the comparison column is the TAX — what the async
/// machinery costs over doing the same work by hand.
/// That is the number this benchmark exists for, and it is a comparison between two loops rather than a property of
/// either, which is exactly what the column expresses.
///
/// `record` files the async seconds-per-node, and the tax, as PGO metrics.
template <class AsyncBody, class DirectBody>
void compare(cc::string_view label, isize nodes, bool record, AsyncBody&& async_body, DirectBody&& direct_body)
{
    // Graphs per iteration, chosen so an iteration is comfortably above timer noise while keeping the live set to a
    // single in-flight graph (each is built, driven, read and destroyed within the body).
    int const graphs = int(4096 / nodes < 1 ? 1 : 4096 / nodes);
    auto const items = nodes * isize(graphs);

    constexpr auto cfg = nx::bench::run_config{.min_time_secs = 0.1, .max_samples = 512};

    auto const direct = nx::bench::run(cc::format("{} direct", label), cfg,
                                       [&](nx::bench::iteration& it)
                                       {
                                           u64 acc = 0;
                                           for (int g = 0; g < graphs; ++g)
                                               acc ^= direct_body(g);
                                           nx::bench::sink(acc);
                                           it.items(items);
                                       });

    auto const async_result = nx::bench::run(cc::format("{} async", label), cfg,
                                             [&](nx::bench::iteration& it)
                                             {
                                                 u64 acc = 0;
                                                 for (int g = 0; g < graphs; ++g)
                                                     acc ^= async_body(g);
                                                 nx::bench::sink(acc);
                                                 it.items(items);
                                             });

    if (!record)
        return;

    auto const per_node
        = [](nx::bench::result const& r) { return r.items_per_second > 0 ? 1.0 / r.items_per_second : 0.0; };
    auto const a = per_node(async_result);
    auto const d = per_node(direct);

    nx::pgo::report(label, a, nx::bench::unit_seconds_per_item);
    if (d > 0)
        nx::pgo::report(cc::format("{} tax", label), a / d, nx::bench::unit_overhead);
}

// Drive a freshly-built root to completion on the calling thread's scheduler, and return its value.
//
// The scheduler is reused across every iteration, so this only measures the graph if the queue settles on its own.
// It does: a singlethreaded_scheduler has no steal-capable peers, so the poll loop never publishes a dependency it is about to drive inline.
// Nothing is left queued to pin nodes alive.
// Were that not so, the live-node set would grow without bound and this would degrade into a memory-growth benchmark.
i64 drive(cc::singlethreaded_scheduler& sched, cc::shared_async<i64> const& root)
{
    root->schedule();
    sched.run_until([&] { return root->is_ready(); });
    return *root->try_value(); // zero-copy; ready by construction
}

// One full single-lazy cycle — create, schedule, drive, read, destroy — pinned as one searchable symbol so its codegen can be disassembled or traced directly.
// This is the "single lazy inline" row: the born-ready floor plus the scheduler push/pop, try_begin_running, one poll turn through the (inline) frame, finish_value, and teardown.
// `sched` must already be bound by an async_worker_scope.
// Kept alive by a reference from the test below — a TU-local noinline function is otherwise dead-code-eliminated.
// The trace command that targets it is in libs/base/clean-core/docs/benchmarks/async-benchmark.md.
CC_DONT_INLINE u64 single_lazy_probe(cc::singlethreaded_scheduler& sched, i64 seed)
{
    auto n = cc::make_async_lazy<i64>([seed] { return seed; });
    n->schedule();
    sched.run_until([&] { return n->is_ready(); });
    return u64(*n->try_value());
}

// --- cases (simplest -> up) -----------------------------------------------------------------------------

// Floor: born-ready node (no scheduler, no frame) — node alloc + one finish + teardown.
void case_born_ready(bool record)
{
    compare(
        "born-ready read", 1, record,
        [](int g)
        {
            auto n = cc::make_async_from_value(i64(g));
            return u64(*n->try_value());
        },
        [](int g) { return u64(direct_leaf(i64(g))); });
}

// Single lazy node driven inline: node alloc + closure + one poll + finish + teardown + scheduler push/pop.
void case_single_lazy(cc::singlethreaded_scheduler& sched, bool record)
{
    compare(
        "single lazy inline", 1, record,
        [&](int g)
        {
            i64 const seed = i64(g);
            auto n = cc::make_async_lazy<i64>([seed] { return seed; });
            return u64(drive(sched, n));
        },
        [](int g) { return u64(direct_leaf(i64(g))); });
}

// Single-dependency transform a -> b: the two-phase frame (register dep, wait, compute).
void case_single_dep(cc::singlethreaded_scheduler& sched, bool record)
{
    compare(
        "single-dep a->b", 2, record,
        [&](int g)
        {
            i64 const seed = i64(g);
            auto n0 = cc::make_async_lazy<i64>([seed] { return seed; });
            auto n1 = cc::make_async_lazy([](i64 x) { return x + 1; }, cc::move(n0));
            return u64(drive(sched, n1));
        },
        [](int g) { return u64(direct_step(direct_leaf(i64(g)))); });
}

// Deep linear chain: amortized per-node cost.
// `n` straddles the inline depth cap (async_max_inline_depth == 128): below it the drive is depth-first inline, above it the poll loop falls back to subscribe+park.
void case_chain(cc::singlethreaded_scheduler& sched, char const* label, int n, bool record)
{
    compare(
        label, n, record, [&](int g) { return u64(drive(sched, build_chain(n, i64(g)))); },
        [n](int g)
        {
            i64 x = direct_leaf(i64(g));
            for (int i = 1; i < n; ++i)
                x = direct_step(x);
            return u64(x);
        });
}

// Fan-in c = f(a, b): per-dep unwrap + short-circuit on a two-leaf sum.
void case_fan_in(cc::singlethreaded_scheduler& sched, bool record)
{
    compare(
        "fan-in c=f(a,b)", 3, record,
        [&](int g)
        {
            i64 const s = i64(g);
            auto la = cc::make_async_lazy<i64>([s] { return s; });
            auto lb = cc::make_async_lazy<i64>([s] { return s + 1; });
            auto c = cc::make_async_lazy([](i64 l, i64 r) { return l + r; }, la, lb);
            return u64(drive(sched, c));
        },
        [](int g) { return u64(direct_add(direct_leaf(i64(g)), direct_leaf(i64(g) + 1))); });
}

// Balanced sum-tree driven single-threaded: per-node cost at scale (depth 13 -> 16383 nodes).
void case_sum_tree(cc::singlethreaded_scheduler& sched, int depth, bool record)
{
    isize const nodes = (isize(1) << (depth + 1)) - 1;
    compare(
        "sum-tree (depth 13)", nodes, record,
        [&, depth](int g) { return u64(drive(sched, build_sum_tree(depth, i64(g)))); },
        [depth](int g) { return u64(direct_sum_tree(depth, i64(g))); });
}

} // namespace

// The points the PGO benchmark records.
// The first three cover the distinct cost shapes with the fewest measurements: the undriven floor, one full scheduler round-trip, and the amortized per-node cost at scale.
//
// The last two are here because the "other rows merely interpolate" argument stops holding once a change lands cost on a path none of the first three reach:
//
//   * chain N=512 is past the inline depth cap, so it is the ONLY recorded point that subscribes and parks.
//   * fan-in is the only one whose frame captures more than one dependency handle, which is what the node's inline frame budget is sized against.
//
// Both are cheap next to the sum tree.
// Resist growing this further — PGO benchmarks are swept across every binary by dev.py pgo, so each point is paid for repeatedly.
PGO_BENCHMARK("bench-async (single-thread drive)")
{
    case_born_ready(/*record*/ true);

    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope scope(sched); // bind once; reused across every driven case

    case_single_lazy(sched, /*record*/ true);
    case_chain(sched, "chain N=512 (>cap)", 512, /*record*/ true);
    case_fan_in(sched, /*record*/ true);
    case_sum_tree(sched, 13, /*record*/ true);

    // Keep the disassembly probe alive (TU-local + noinline would otherwise be dead-code-eliminated).
    for (i64 i = 0; i < 4; ++i)
        nx::bench::sink(single_lazy_probe(sched, 7 + i));
}

// One BENCHMARK per case rather than one table of all of them.
//
// The comparison column is the TAX — async over direct — and that only means something within a case: comparing a
// born-ready read against a 16383-node sum tree is a statement about graph size, not about the machinery.
BENCHMARK("bench-async - born-ready read")
{
    case_born_ready(/*record*/ false);
}

BENCHMARK("bench-async - single lazy inline")
{
    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope scope(sched);
    case_single_lazy(sched, /*record*/ false);
}

BENCHMARK("bench-async - single-dep a->b")
{
    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope scope(sched);
    case_single_dep(sched, /*record*/ false);
}

// The two chain lengths are separate benchmarks, not two pairs in one table: the comparison column is the tax
// against that pair's own direct row, and a table holding both would compare N=512 against N=64's direct baseline.
// What the doc reads across them is the two taxes, which are one table apart.
BENCHMARK("bench-async - chain N=64, within the inline cap")
{
    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope scope(sched);
    case_chain(sched, "chain N=64 (in-cap)", 64, /*record*/ false);
}

BENCHMARK("bench-async - chain N=512, past the inline cap")
{
    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope scope(sched);
    case_chain(sched, "chain N=512 (>cap)", 512, /*record*/ false);
}

BENCHMARK("bench-async - fan-in")
{
    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope scope(sched);
    case_fan_in(sched, /*record*/ false);
}

BENCHMARK("bench-async - sum-tree at scale")
{
    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope scope(sched);
    case_sum_tree(sched, 13, /*record*/ false);
}
