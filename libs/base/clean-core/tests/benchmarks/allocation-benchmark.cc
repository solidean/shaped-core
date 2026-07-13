// Standalone allocation throughput benchmark.
//
// Compares the mimalloc-backed default resource (cc::default_memory_resource) against the platform
// malloc/free resource (cc::system_memory_resource) across allocation sizes. The pattern is a churn: a small
// ring of concurrently-live allocations, each cycle freeing the oldest and allocating a fresh block (the
// common short-lived-object workload). Reports millions of alloc+free cycles per second.
//
// Companion to the xxHash investigation: checks whether the vendored mimalloc suffers the same RelWithDebInfo
// /Ob1 under-inlining penalty. It does, but mildly — ~1.6x on small allocations vs xxHash's ~11x — since its
// hot malloc/free are force-inlined upstream. The project-wide /Ob2 promotion (root CMakeLists) covers it.
// See libs/base/clean-core/docs/benchmarks/allocation-benchmark.md.
//
// Guide benchmark (GUIDE_BENCHMARK): prints the table and records mimalloc/system throughput at 64 B and
// 4 KiB via nx::guide for the PGO speedup report.

#include "bench_util.hh"

#include <clean-core/container/span.hh>
#include <clean-core/math/random.hh>
#include <clean-core/memory/allocation.hh>
#include <clean-core/memory/node_allocation.hh>
#include <clean-core/string/string.hh>
#include <nexus/guide.hh>
#include <nexus/test.hh>

#include <chrono>
#include <cstdio>

using namespace cc::primitive_defines;

namespace
{
// Millions of alloc+free cycles per second for `res`, keeping `live` blocks of `size` bytes alive at once.
double mops(cc::memory_resource const& res, isize size, isize align)
{
    constexpr isize live = 64;       // working set of concurrently-live allocations
    constexpr isize per_pass = 4096; // alloc/free cycles per timed pass

    cc::byte* slots[live] = {};

    // Prime the ring so every cycle frees a real block.
    for (isize i = 0; i < live; ++i)
        res.allocate_bytes(&slots[i], size, size, align, res.userdata);

    isize idx = 0;
    double const ops_per_sec = bench::measure_units_per_sec(
        double(per_pass),
        [&]
        {
            u64 acc = 0;
            for (isize n = 0; n < per_pass; ++n)
            {
                cc::byte*& slot = slots[idx];
                res.deallocate_bytes(slot, size, align, res.userdata);
                res.allocate_bytes(&slot, size, size, align, res.userdata);
                slot[0] = cc::byte(n);        // touch both ends to fault the pages, like real use
                slot[size - 1] = cc::byte(n); //
                acc ^= reinterpret_cast<u64>(slot);
                idx = (idx + 1) % live;
            }
            return acc;
        });

    for (isize i = 0; i < live; ++i)
        res.deallocate_bytes(slots[i], size, align, res.userdata);

    return ops_per_sec / 1e6;
}

// Millions of create+destroy cycles per second for the owning cc::allocation<byte> handle on `res`.
// Same churn as mops(), but through the container-facing handle (create_uninitialized + move-assign +
// dtor) instead of the bare resource, so the delta is the handle's per-cycle bookkeeping overhead.
double alloc_mops(cc::memory_resource const& res, isize size)
{
    constexpr isize live = 64;
    constexpr isize per_pass = 4096;

    using alloc_t = cc::allocation<cc::byte>;
    alloc_t slots[live];

    for (isize i = 0; i < live; ++i)
        slots[i] = alloc_t::create_uninitialized(size, &res);

    isize idx = 0;
    double const ops_per_sec = bench::measure_units_per_sec(
        double(per_pass),
        [&]
        {
            u64 acc = 0;
            for (isize n = 0; n < per_pass; ++n)
            {
                slots[idx] = alloc_t::create_uninitialized(size, &res); // move-assign frees the old block
                cc::byte* const p = slots[idx].obj_start;
                p[0] = cc::byte(n);        // touch both ends, like real use
                p[size - 1] = cc::byte(n); //
                acc ^= reinterpret_cast<u64>(p);
                idx = (idx + 1) % live;
            }
            return acc;
        });

    return ops_per_sec / 1e6;
}

// Millions of alloc+free cycles per second for the thread-local node allocator at `Size` bytes.
// `Size` is a template parameter so the class index is a compile-time constant, which is how the node
// allocator is actually used (node_allocation<T> derives its class from the type) and what the force-inlined
// fast path needs to fold the size-class branch and the shift away — passing a runtime index badly
// undersells it. Alignment is 8 (natural node/pointer alignment), so class = bit_width(max(Size, 8) - 1);
// sizes above 256 B fall off the small-class fast path onto the header-backed large path.
//
// The live set is kept small (32) on purpose: a single slab holds 62-63 usable slots for these classes,
// so the whole working set fits one slab and every cycle stays on the wait-free fast path. Growing the
// live set past a slab would trigger refill, which currently leaks the old slab
// (see libs/base/clean-core/docs/systems/node-allocation.md) — not what this benchmark is measuring.
template <isize Size>
double node_mops()
{
    constexpr isize live = 32;
    constexpr isize per_pass = 4096;
    constexpr isize align = 8;
    constexpr cc::node_class_index class_idx = cc::node_class_index_from_size_and_align(Size, align);

    auto& alloc = cc::default_node_allocator();

    cc::byte* slots[live] = {};
    for (isize i = 0; i < live; ++i)
        slots[i] = alloc.allocate_node_bytes(class_idx, Size, align);

    isize idx = 0;
    double const ops_per_sec
        = bench::measure_units_per_sec(double(per_pass),
                                       [&]
                                       {
                                           u64 acc = 0;
                                           for (isize n = 0; n < per_pass; ++n)
                                           {
                                               cc::byte*& slot = slots[idx];
                                               cc::node_allocation_free(slot, class_idx);
                                               slot = alloc.allocate_node_bytes(class_idx, Size, align);
                                               slot[0] = cc::byte(n);        // touch both ends, like real use
                                               slot[Size - 1] = cc::byte(n); //
                                               acc ^= reinterpret_cast<u64>(slot);
                                               idx = (idx + 1) % live;
                                           }
                                           return acc;
                                       });

    for (isize i = 0; i < live; ++i)
        cc::node_allocation_free(slots[i], class_idx);

    return ops_per_sec / 1e6;
}

// --- Steady-state small-batch benchmark -----------------------------------------------------------------
//
// The intended strongest case for the node allocator: a hot thread-local batch that never exceeds one slab.
// Each iteration allocates a fixed batch of N nodes, then frees all N in a fixed *permuted* order; this
// repeats `iters` times. Only the alloc+free work is timed — the permutation is precomputed once and reused
// for the whole run, so no shuffling/RNG cost leaks into the measurement. Because every batch is fully freed
// before the next, the slab returns to the same freemap each iteration: fully deterministic, cache-hot, and
// entirely on the wait-free fast path (no refill, no rolling index, no page-touch). This isolates the raw
// per-op allocator cost. We run it 3x and print all three numbers as the simplest possible noise measure.
//
// Metric: millions of alloc+free PAIRS per second (one pair = one allocate + one matching free). Each
// iteration contributes N pairs, so a run of `iters` iterations is iters*N pairs.
namespace steady
{
using clock = std::chrono::steady_clock;

constexpr cc::isize batch_n = 10;      // nodes allocated/freed per iteration
constexpr cc::isize iters = 1'000'000; // iterations per timed run
constexpr int runs = 3;                // repeated timed runs (noise measure)

// A fixed permutation of 0..batch_n-1: the order in which the batch is freed. Non-sequential on purpose so
// the free order is neither pure-LIFO nor pure-FIFO, which could unrealistically flatter one allocator.
constexpr int free_order[batch_n] = {4, 0, 8, 2, 6, 9, 1, 5, 3, 7};

// Runs the batch loop 3x with the given alloc/free callables and prints one labeled result row.
// alloc_one(i) -> cc::byte* stores nothing itself; free_one(p) releases a pointer previously returned.
template <class AllocOne, class FreeOne>
void run3(char const* label, cc::isize size, AllocOne alloc_one, FreeOne free_one)
{
    cc::byte* nodes[batch_n] = {};

    // Warmup: materialize the slab / warm the thread-local free list and caches before timing.
    for (cc::isize w = 0; w < 2000; ++w)
    {
        for (cc::isize i = 0; i < batch_n; ++i)
            nodes[i] = alloc_one();
        for (cc::isize i = 0; i < batch_n; ++i)
            free_one(nodes[free_order[i]]);
    }

    std::printf("%-14s %6lld B :", label, (long long)size);
    for (int r = 0; r < runs; ++r)
    {
        u64 acc = 0;
        auto const t0 = clock::now();
        for (cc::isize it = 0; it < iters; ++it)
        {
            for (cc::isize i = 0; i < batch_n; ++i)
                nodes[i] = alloc_one();
            for (cc::isize i = 0; i < batch_n; ++i)
            {
                cc::byte* const p = nodes[free_order[i]];
                acc ^= reinterpret_cast<u64>(p); // keep the pointer live so nothing is elided
                free_one(p);
            }
        }
        double const seconds = std::chrono::duration<double>(clock::now() - t0).count();
        bench::sink ^= acc;

        double const mpairs = double(iters * batch_n) / seconds / 1e6;
        std::printf(" %8.1f", mpairs);
    }
    std::printf("   M pairs/s\n");
}

// Same 3x harness, but interleaved: a primed batch of N is kept live and each step frees one node and
// immediately re-allocates into that slot (free -> alloc dependency on the same slab cache line, back to
// back). This is the pattern the older churn benchmark used; isolating it here (identical no-touch
// conditions, only the access order differs from run3) shows how much of the node allocator's measured
// speed is pattern-dependent — the free->alloc round-trip cannot pipeline the two locked RMWs, whereas
// the batch pattern lets a run of allocs (then a run of frees) overlap.
// `touch`: after each allocation, write the first and last payload byte — exactly what the older churn
// benchmark did "to fault the pages, like real use". Isolates whether the payload store (which for small
// classes lands in or near the freemap's cache line and could stall the next locked freemap RMW) matters.
// Measured effect on Zen 4: negligible — node and mimalloc are within noise of the no-touch column.
template <class AllocOne, class FreeOne>
void run3_interleaved(char const* label, cc::isize size, bool touch, AllocOne alloc_one, FreeOne free_one)
{
    cc::byte* nodes[batch_n] = {};
    for (cc::isize i = 0; i < batch_n; ++i)
        nodes[i] = alloc_one();

    for (cc::isize w = 0; w < 2000; ++w)
        for (cc::isize i = 0; i < batch_n; ++i)
        {
            free_one(nodes[i]);
            nodes[i] = alloc_one();
        }

    std::printf("%-14s %6lld B :", label, (long long)size);
    for (int r = 0; r < runs; ++r)
    {
        u64 acc = 0;
        auto const t0 = clock::now();
        for (cc::isize it = 0; it < iters; ++it)
            for (cc::isize i = 0; i < batch_n; ++i)
            {
                free_one(nodes[i]);
                cc::byte* const p = alloc_one();
                nodes[i] = p;
                if (touch)
                {
                    p[0] = cc::byte(i);
                    p[size - 1] = cc::byte(i);
                }
                acc ^= reinterpret_cast<u64>(p);
            }
        double const seconds = std::chrono::duration<double>(clock::now() - t0).count();
        bench::sink ^= acc;

        double const mpairs = double(iters * batch_n) / seconds / 1e6;
        std::printf(" %8.1f", mpairs);
    }
    std::printf("   M pairs/s\n");

    for (cc::isize i = 0; i < batch_n; ++i)
        free_one(nodes[i]);
}

// The two allocators as they are actually used: the inlined node fast path, and mimalloc through the
// polymorphic memory_resource (indirect call + size/alignment bookkeeping — the same path cc::allocation
// and every general container take).
template <isize Size, class Body>
void for_each_allocator(Body body)
{
    constexpr isize align = 8;
    constexpr cc::node_class_index class_idx = cc::node_class_index_from_size_and_align(Size, align);

    auto& node_alloc = cc::default_node_allocator();
    body(
        "node", //
        [&] { return node_alloc.allocate_node_bytes(class_idx, Size, align); },
        [&](cc::byte* p) { cc::node_allocation_free(p, class_idx); });

    cc::memory_resource const& mi = *cc::default_memory_resource;
    body(
        "mimalloc",
        [&]
        {
            cc::byte* p = nullptr;
            mi.allocate_bytes(&p, Size, Size, align, mi.userdata);
            return p;
        },
        [&](cc::byte* p) { mi.deallocate_bytes(p, Size, align, mi.userdata); });
}

template <isize Size>
void row_batch()
{
    for_each_allocator<Size>([](char const* label, auto alloc_one, auto free_one)
                             { run3(label, Size, alloc_one, free_one); });
}

template <isize Size>
void row_interleaved(bool touch)
{
    for_each_allocator<Size>([&](char const* label, auto alloc_one, auto free_one)
                             { run3_interleaved(label, Size, touch, alloc_one, free_one); });
}

// A uniquely-named, non-inlined copy of one hot iteration of the node fast path, extracted so it compiles
// to a single searchable symbol: `dev.py assembly show node_alloc_free_hotloop_probe` lands exactly on the
// alloc (atomic bitmap load + fetch_and) and free (atomic_or) codegen — the metal this whole investigation
// is about. Not part of any timing; it exists purely as a disassembly target and is kept alive by a
// reference from the steady-state test below (TU-local + noinline would otherwise be dead-code-eliminated).
CC_DONT_INLINE u64 node_alloc_free_hotloop_probe(cc::node_allocator& alloc, cc::byte** nodes, int const* free_perm)
{
    constexpr isize Size = 16;
    constexpr isize align = 8;
    constexpr cc::node_class_index class_idx = cc::node_class_index_from_size_and_align(Size, align);

    for (isize i = 0; i < batch_n; ++i)
        nodes[i] = alloc.allocate_node_bytes(class_idx, Size, align);

    u64 acc = 0;
    for (isize i = 0; i < batch_n; ++i)
    {
        cc::byte* const p = nodes[free_perm[i]];
        acc ^= reinterpret_cast<u64>(p);
        cc::node_allocation_free(p, class_idx);
    }
    return acc;
}

template <isize... Sizes>
void run_all()
{
    std::printf("\n=== steady-state small batch: alloc %lld, free %lld (permuted), then repeat, x%lld iters, %d runs "
                "===\n",
                (long long)batch_n, (long long)batch_n, (long long)iters, runs);
    std::printf("%-14s %8s : %8s %8s %8s\n", "allocator", "size", "run1", "run2", "run3");
    (row_batch<Sizes>(), ...);

    std::printf("\n=== steady-state interleaved: free one, alloc one (%lld live), no payload touch, x%lld iters, %d "
                "runs ===\n",
                (long long)batch_n, (long long)iters, runs);
    std::printf("%-14s %8s : %8s %8s %8s\n", "allocator", "size", "run1", "run2", "run3");
    (row_interleaved<Sizes>(/*touch*/ false), ...);

    std::printf("\n=== steady-state interleaved + payload touch (reproduces old churn inner loop), x%lld iters, %d "
                "runs ===\n",
                (long long)iters, runs);
    std::printf("%-14s %8s : %8s %8s %8s\n", "allocator", "size", "run1", "run2", "run3");
    (row_interleaved<Sizes>(/*touch*/ true), ...);
    std::fflush(stdout);
}
} // namespace steady

// Sweeps `sizes`, printing one mimalloc/system row each. When `record`, the 64 B and 4 KiB points are also
// reported as guide metrics — pass the representative-only sizes for a fast guide benchmark, or the full set
// (record=false) for the human analysis table.
void run(cc::span<isize const> sizes, bool record)
{
    isize const align = 16; // typical malloc alignment; both resources honor it

    std::printf("\n=== allocation throughput (M alloc+free / s) — 64 live, churn ===\n");
    std::printf("%10s %14s %14s\n", "size", "mimalloc", "system");
    std::printf("%10s %14s %14s\n", "----", "--------", "------");
    for (isize const size : sizes)
    {
        double const mi = mops(*cc::default_memory_resource, size, align);
        double const sys = mops(cc::system_memory_resource, size, align);
        std::printf("%10lld %14.1f %14.1f\n", (long long)size, mi, sys);

        if (record && (size == 64 || size == 4096))
        {
            char const* const label = size == 64 ? "64B" : "4KiB";
            nx::guide::report_raw(cc::string("mimalloc@") + label, mi, "M ops/s", true);
            nx::guide::report_raw(cc::string("system@") + label, sys, "M ops/s", true);
        }
    }
    std::fflush(stdout);
}

// One comparison row at compile-time size `Size`: bare mimalloc, the cc::allocation<byte> handle over it,
// and the node allocator (which needs the compile-time class index, hence the template).
template <isize Size>
void comparison_row()
{
    constexpr isize align = 16; // for the raw + handle columns; node uses its own 8 B-aligned class model
    double const mi = mops(*cc::default_memory_resource, Size, align);
    double const al = alloc_mops(*cc::default_memory_resource, Size);
    double const nd = node_mops<Size>();
    std::printf("%10lld %14.1f %16.1f %14.1f\n", (long long)Size, mi, al, nd);
}

// Compares the three allocation paths across `Sizes`: the bare mimalloc resource, the owning
// cc::allocation<byte> handle over it, and the thread-local node allocator. Node small classes (<= 256 B)
// are the point of interest; the larger rows show the node header-path cliff (its large path is just
// mimalloc + a 24 B header) and the handle overhead converging with the raw resource once the allocation
// itself dominates.
template <isize... Sizes>
void run_comparison()
{
    std::printf("\n=== allocation throughput (M alloc+free / s) — churn ===\n");
    std::printf("%10s %14s %16s %14s\n", "size", "mimalloc raw", "cc::allocation", "node");
    std::printf("%10s %14s %16s %14s\n", "----", "------------", "--------------", "----");
    (comparison_row<Sizes>(), ...);
    std::fflush(stdout);
}

// The representative sizes the guide benchmark sweeps: one small block (64 B) and one page-sized (4 KiB).
constexpr isize guide_sizes[] = {64, 4096};
isize const full_sizes[] = {16, 32, 64, 128, 256, 512, 1024, 4096, 16384, 65536};
} // namespace

// Lean guide benchmark: just the representative sizes, recorded for the PGO speedup report.
GUIDE_BENCHMARK("bench-alloc (mimalloc vs system)")
{
    run(guide_sizes, /*record*/ true);
}

// Full human-facing sweep (manual): the complete size table the docs analyze. Run by exact name.
TEST("bench-alloc (mimalloc vs system, full sweep)", nx::config::manual)
{
    run(full_sizes, /*record*/ false);
}

// Raw resource vs the cc::allocation<byte> handle vs the node allocator, across realistic sizes.
// Manual (not recorded as a guide metric): the table analyzed in
// libs/base/clean-core/docs/systems/allocation.md and libs/base/clean-core/docs/systems/node-allocation.md.
TEST("bench-alloc (handle & node comparison)", nx::config::manual)
{
    // Small node classes (<= 256 B), then a few larger blocks that expose the node large-path cliff.
    run_comparison<8, 16, 32, 64, 128, 256, 512, 1024, 4096>();
}

// Steady-state small-batch: alloc 10 nodes, free 10 in a fixed permuted order, x1M iters, node vs mimalloc.
// The single cleanest measurement of the node allocator's raw fast-path cost — the case it exists to win.
TEST("bench-alloc (steady-state small batch)", nx::config::manual)
{
    steady::run_all<8, 16, 32, 64>();

    // Keep the disassembly probe alive (TU-local + noinline) without perturbing the timings above.
    cc::byte* probe_nodes[steady::batch_n] = {};
    bench::sink ^= steady::node_alloc_free_hotloop_probe(cc::default_node_allocator(), probe_nodes, steady::free_order);
}
