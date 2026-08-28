// Standalone allocation throughput benchmark.
//
// Compares the mimalloc-backed default resource (cc::default_memory_resource) against the platform malloc/free resource (cc::system_memory_resource) across allocation sizes.
// The pattern is a churn: a small ring of concurrently-live allocations, each cycle freeing the oldest and allocating a fresh block.
// The results, the machine they were taken on, and the /Ob1 inlining story are in libs/base/clean-core/docs/benchmarks/allocation-benchmark.md.
//
// PGO benchmark (PGO_BENCHMARK): prints the table and records mimalloc/system throughput at 64 B and
// 4 KiB via nx::pgo for the PGO speedup report.


#include <clean-core/container/span.hh>
#include <clean-core/math/random.hh>
#include <clean-core/memory/allocation.hh>
#include <clean-core/memory/node_allocation.hh>
#include <clean-core/string/string.hh>
#include <nexus/bench/run.hh>
#include <nexus/bench/units.hh>
#include <nexus/pgo.hh>
#include <nexus/test.hh>


using namespace cc::primitive_defines;

namespace
{
/// Sweep points measure different amounts of work, so they carry no baseline.
///
/// 5% rather than the default 2%: allocator churn moves between samples as the allocator's own state does — a slab
/// refill, a page fault, a size class crossing a boundary — and no sample count irons that out.
constexpr auto sweep_config = nx::bench::run_config{
    .min_time_secs = 0.1,
    .max_samples = 512,
    .target_relative_error = 0.05,
    .no_baseline = true,
};

/// One alloc+free churn loop for `res`, keeping `live` blocks of `size` bytes alive at once.
nx::bench::result mops(cc::string_view name,
                       nx::bench::run_config const& cfg,
                       cc::memory_resource const& res,
                       isize size,
                       isize align)
{
    constexpr isize live = 64;       // working set of concurrently-live allocations
    constexpr isize per_pass = 4096; // alloc/free cycles per timed pass

    byte* slots[live] = {};

    // Prime the ring so every cycle frees a real block.
    for (isize i = 0; i < live; ++i)
        res.allocate_bytes(&slots[i], size, size, align, res.userdata);

    isize idx = 0;
    auto const r = nx::bench::run(name, cfg,
                                  [&](nx::bench::iteration& it)
                                  {
                                      u64 acc = 0;
                                      for (isize n = 0; n < per_pass; ++n)
                                      {
                                          byte*& slot = slots[idx];
                                          res.deallocate_bytes(slot, size, align, res.userdata);
                                          res.allocate_bytes(&slot, size, size, align, res.userdata);
                                          slot[0] = byte(n);        // touch both ends to fault the pages, like real use
                                          slot[size - 1] = byte(n); //
                                          acc ^= reinterpret_cast<u64>(slot);
                                          idx = (idx + 1) % live;
                                      }
                                      nx::bench::sink(acc);
                                      it.items(per_pass); // alloc+free cycles
                                  });

    for (isize i = 0; i < live; ++i)
        res.deallocate_bytes(slots[i], size, align, res.userdata);

    return r;
}

// Millions of create+destroy cycles per second for the owning cc::allocation<byte> handle on `res`.
// Same churn as mops(), but through the container-facing handle (create_uninitialized + move-assign +
// dtor) instead of the bare resource, so the delta is the handle's per-cycle bookkeeping overhead.
nx::bench::result alloc_mops(cc::string_view name,
                             nx::bench::run_config const& cfg,
                             cc::memory_resource const& res,
                             isize size)
{
    constexpr isize live = 64;
    constexpr isize per_pass = 4096;

    using alloc_t = cc::allocation<byte>;
    alloc_t slots[live];

    for (isize i = 0; i < live; ++i)
        slots[i] = alloc_t::create_uninitialized(size, &res);

    isize idx = 0;
    return nx::bench::run(name, cfg,
                          [&](nx::bench::iteration& it)
                          {
                              u64 acc = 0;
                              for (isize n = 0; n < per_pass; ++n)
                              {
                                  slots[idx] = alloc_t::create_uninitialized(size, &res); // move-assign frees the old
                                  byte* const p = slots[idx].obj_start;
                                  p[0] = byte(n);        // touch both ends, like real use
                                  p[size - 1] = byte(n); //
                                  acc ^= reinterpret_cast<u64>(p);
                                  idx = (idx + 1) % live;
                              }
                              nx::bench::sink(acc);
                              it.items(per_pass);
                          });
}

// Millions of alloc+free cycles per second for the thread-local node allocator at `Size` bytes.
// `Size` is a template parameter so the class index is a compile-time constant, which is how the node allocator is actually used — node_allocation<T> derives its class from the type.
// That is also what the force-inlined fast path needs in order to fold the size-class branch and the shift away; passing a runtime index badly undersells it.
// Alignment is 8, the natural node/pointer alignment, so class = bit_width(max(Size, 8) - 1).
// Sizes above 256 B fall off the small-class fast path onto the header-backed large path.
//
// The live set is kept small (32) on purpose: a single slab holds 62-63 usable slots for these classes, so the whole working set fits one slab.
// Every cycle therefore stays on the wait-free fast path, and growing the live set past a slab would trigger refill, which is not what this benchmark is measuring.
template <isize Size>
nx::bench::result node_mops(cc::string_view name, nx::bench::run_config const& cfg)
{
    constexpr isize live = 32;
    constexpr isize per_pass = 4096;
    constexpr isize align = 8;
    constexpr cc::node_class_index class_idx = cc::node_class_index_from_size_and_align(Size, align);

    auto& alloc = cc::default_node_allocator();

    byte* slots[live] = {};
    for (isize i = 0; i < live; ++i)
        slots[i] = alloc.allocate_node_bytes(class_idx, Size, align);

    isize idx = 0;
    auto const r = nx::bench::run(name, cfg,
                                  [&](nx::bench::iteration& it)
                                  {
                                      u64 acc = 0;
                                      for (isize n = 0; n < per_pass; ++n)
                                      {
                                          byte*& slot = slots[idx];
                                          cc::node_allocation_free(slot, class_idx);
                                          slot = alloc.allocate_node_bytes(class_idx, Size, align);
                                          slot[0] = byte(n);        // touch both ends, like real use
                                          slot[Size - 1] = byte(n); //
                                          acc ^= reinterpret_cast<u64>(slot);
                                          idx = (idx + 1) % live;
                                      }
                                      nx::bench::sink(acc);
                                      it.items(per_pass);
                                  });

    for (isize i = 0; i < live; ++i)
        cc::node_allocation_free(slots[i], class_idx);

    return r;
}

// --- Steady-state small-batch benchmark -----------------------------------------------------------------
//
// The intended strongest case for the node allocator: a hot thread-local batch that never exceeds one slab.
// Each iteration allocates a fixed batch of N nodes, then frees all N in a fixed *permuted* order, and this repeats `iters` times.
// Only the alloc+free work is timed: the permutation is precomputed once and reused for the whole run, so no shuffling or RNG cost leaks into the measurement.
// Because every batch is fully freed before the next, the slab returns to the same freemap each iteration — deterministic, cache-hot, and entirely on the wait-free fast path.
// That isolates the raw per-op allocator cost.
// Noise is reported as the harness's interval rather than by printing three timed runs side by side.
//
// Metric: millions of alloc+free PAIRS per second (one pair = one allocate + one matching free). Each
// Each iteration contributes N alloc+free pairs, which is what it declares through iteration::items.
namespace steady
{
constexpr isize batch_n = 10; // nodes allocated/freed per iteration

// A fixed permutation of 0..batch_n-1: the order in which the batch is freed.
// Non-sequential on purpose, so the free order is neither pure-LIFO nor pure-FIFO, either of which could unrealistically flatter one allocator.
constexpr int free_order[batch_n] = {4, 0, 8, 2, 6, 9, 1, 5, 3, 7};

// Runs the batch loop 3x with the given alloc/free callables and prints one labeled result row.
// alloc_one(i) -> cc::byte* stores nothing itself; free_one(p) releases a pointer previously returned.
template <class AllocOne, class FreeOne>
void run3(char const* label, isize size, AllocOne alloc_one, FreeOne free_one)
{
    byte* nodes[batch_n] = {};

    // Warmup: materialize the slab / warm the thread-local free list and caches before timing.
    for (isize w = 0; w < 2000; ++w)
    {
        for (isize i = 0; i < batch_n; ++i)
            nodes[i] = alloc_one();
        for (isize i = 0; i < batch_n; ++i)
            free_one(nodes[free_order[i]]);
    }

    // One loop rather than three timed runs printed side by side: the harness's interval says what those three
    // columns were there to let a reader eyeball, and says it from hundreds of samples rather than three.
    nx::bench::run(cc::format("{} @{}B batch", label, size), sweep_config,
                   [&](nx::bench::iteration& it)
                   {
                       u64 acc = 0;
                       for (isize i = 0; i < batch_n; ++i)
                           nodes[i] = alloc_one();
                       for (isize i = 0; i < batch_n; ++i)
                       {
                           byte* const p = nodes[free_order[i]];
                           acc ^= reinterpret_cast<u64>(p); // keep the pointer live so nothing is elided
                           free_one(p);
                       }
                       nx::bench::sink(acc);
                       it.items(batch_n); // alloc+free pairs
                   });
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
void run3_interleaved(char const* label, isize size, bool touch, AllocOne alloc_one, FreeOne free_one)
{
    byte* nodes[batch_n] = {};
    for (isize i = 0; i < batch_n; ++i)
        nodes[i] = alloc_one();

    for (isize w = 0; w < 2000; ++w)
        for (isize i = 0; i < batch_n; ++i)
        {
            free_one(nodes[i]);
            nodes[i] = alloc_one();
        }

    nx::bench::run(cc::format("{} @{}B interleaved{}", label, size, touch ? " + touch" : ""), sweep_config,
                   [&](nx::bench::iteration& it)
                   {
                       u64 acc = 0;
                       for (isize i = 0; i < batch_n; ++i)
                       {
                           free_one(nodes[i]);
                           byte* const p = alloc_one();
                           nodes[i] = p;
                           if (touch)
                           {
                               p[0] = byte(i);
                               p[size - 1] = byte(i);
                           }
                           acc ^= reinterpret_cast<u64>(p);
                       }
                       nx::bench::sink(acc);
                       it.items(batch_n);
                   });

    for (isize i = 0; i < batch_n; ++i)
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
        [&](byte* p) { cc::node_allocation_free(p, class_idx); });

    cc::memory_resource const& mi = *cc::default_memory_resource;
    body(
        "mimalloc",
        [&]
        {
            byte* p = nullptr;
            mi.allocate_bytes(&p, Size, Size, align, mi.userdata);
            return p;
        },
        [&](byte* p) { mi.deallocate_bytes(p, Size, align, mi.userdata); });
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

// A uniquely-named, non-inlined copy of one hot iteration of the node fast path, extracted so it compiles to a single searchable symbol.
// `dev.py assembly show node_alloc_free_hotloop_probe` then lands exactly on the owner's alloc and free codegen, which is non-atomic on the threaded frontend.
// Not part of any timing; it exists purely as a disassembly target.
// A reference from the steady-state test below keeps it alive, since TU-local plus noinline would otherwise be dead-code-eliminated.
CC_DONT_INLINE u64 node_alloc_free_hotloop_probe(cc::node_allocator& alloc, byte** nodes, int const* free_perm)
{
    constexpr isize Size = 16;
    constexpr isize align = 8;
    constexpr cc::node_class_index class_idx = cc::node_class_index_from_size_and_align(Size, align);

    for (isize i = 0; i < batch_n; ++i)
        nodes[i] = alloc.allocate_node_bytes(class_idx, Size, align);

    u64 acc = 0;
    for (isize i = 0; i < batch_n; ++i)
    {
        byte* const p = nodes[free_perm[i]];
        acc ^= reinterpret_cast<u64>(p);
        cc::node_allocation_free(p, class_idx);
    }
    return acc;
}

template <isize... Sizes>
void run_all()
{
    // Three access patterns, each as its own set of loops: batched alloc-then-free, interleaved free-then-alloc, and
    // the same interleaved with a payload touch.
    // The names carry which is which, so the harness's own table replaces the three printed section headers.
    (row_batch<Sizes>(), ...);
    (row_interleaved<Sizes>(/*touch*/ false), ...);
    (row_interleaved<Sizes>(/*touch*/ true), ...);
}
} // namespace steady

/// Sweeps `sizes`, two loops each — mimalloc against the system resource.
///
/// A size sweep, so the rows carry no baseline: comparing a 64 B churn against a 64 KiB one is a statement about the
/// size rather than about the allocator, and items/s is what stays comparable.
void run(cc::span<isize const> sizes, nx::bench::run_config const& cfg, bool record)
{
    isize const align = 16; // typical malloc alignment; both resources honor it

    for (isize const size : sizes)
    {
        auto const mi = mops(cc::format("mimalloc @{}B", size), cfg, *cc::default_memory_resource, size, align);
        auto const sys = mops(cc::format("system @{}B", size), cfg, cc::system_memory_resource, size, align);

        if (record && (size == 64 || size == 4096))
        {
            char const* const label = size == 64 ? "64B" : "4KiB";
            nx::pgo::report(cc::string("mimalloc@") + label, mi.items_per_second, nx::bench::unit_items_per_second);
            nx::pgo::report(cc::string("system@") + label, sys.items_per_second, nx::bench::unit_items_per_second);
        }
    }
}

// One comparison row at compile-time size `Size`: bare mimalloc, the cc::allocation<byte> handle over it,
// and the node allocator (which needs the compile-time class index, hence the template).
template <isize Size>
void comparison_row(nx::bench::run_config const& cfg)
{
    constexpr isize align = 16; // for the raw + handle columns; node uses its own 8 B-aligned class model
    mops(cc::format("mimalloc raw @{}B", Size), cfg, *cc::default_memory_resource, Size, align);
    alloc_mops(cc::format("cc::allocation @{}B", Size), cfg, *cc::default_memory_resource, Size);
    node_mops<Size>(cc::format("node @{}B", Size), cfg);
}

// Compares the three allocation paths across `Sizes`: the bare mimalloc resource, the owning cc::allocation<byte> handle over it, and the thread-local node allocator.
// Node small classes (<= 256 B) are the point of interest.
// The larger rows show the node header-path cliff — its large path is just mimalloc plus a 24 B header — and the handle overhead converging with the raw resource once the allocation itself dominates.
template <isize... Sizes>
void run_comparison(nx::bench::run_config const& cfg)
{
    (comparison_row<Sizes>(cfg), ...);
}

// The representative sizes the PGO benchmark sweeps: one small block (64 B) and one page-sized (4 KiB).
constexpr isize guide_sizes[] = {64, 4096};
isize const full_sizes[] = {16, 32, 64, 128, 256, 512, 1024, 4096, 16384, 65536};
} // namespace

// Lean PGO benchmark: just the representative sizes, recorded for the PGO speedup report.
PGO_BENCHMARK("bench-alloc (mimalloc vs system)")
{
    run(guide_sizes, {.min_time_secs = 0.1, .max_samples = 512}, /*record*/ true);
}

// The complete size table the docs analyze.
BENCHMARK("bench-alloc - mimalloc vs system, full sweep")
{
    run(full_sizes, sweep_config, /*record*/ false);
}

// Raw resource vs the cc::allocation<byte> handle vs the node allocator, across realistic sizes.
// The table analyzed in libs/base/clean-core/docs/systems/allocation.md and
// libs/base/clean-core/docs/systems/node-allocation.md.
BENCHMARK("bench-alloc - handle and node comparison")
{
    // Small node classes (<= 256 B), then a few larger blocks that expose the node large-path cliff.
    run_comparison<8, 16, 32, 64, 128, 256, 512, 1024, 4096>(sweep_config);
}

// Steady-state small-batch: alloc 10 nodes, free 10 in a fixed permuted order, node vs mimalloc.
// The single cleanest measurement of the node allocator's raw fast-path cost — the case it exists to win.
BENCHMARK("bench-alloc - steady-state small batch")
{
    steady::run_all<8, 16, 32, 64>();

    // Keep the disassembly probe alive (TU-local + noinline) without perturbing the timings above.
    byte* probe_nodes[steady::batch_n] = {};
    nx::bench::sink(steady::node_alloc_free_hotloop_probe(cc::default_node_allocator(), probe_nodes, steady::free_order));
}
