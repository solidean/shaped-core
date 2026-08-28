// Born-ready cost decomposition — where the ~40 ns of a born-ready async goes.
//
// A born-ready async (make_async_from_value -> try_value -> destroy) is exactly ONE node-allocator alloc + free plus the node's own control/payload/state bookkeeping.
// This measures a cumulative ladder of increasingly-complete operations, so each row's delta isolates one component:
//
//   raw node alloc+free
//   + make_async_manual
//   + make_async_from_value
//   + try_value
//   cold make_async_lazy          separately, and against the empty node rather than the row above
//
// What each delta isolates, and the numbers, are in libs/base/clean-core/docs/benchmarks/async-benchmark.md.
//
// The manual and cold-lazy rows drop an UNRESOLVED node on purpose, and that must be safe: frame torn down, no leak, no assert.
// If it ever isn't, that is a bug in async teardown rather than in this benchmark.
//
// Anti-fold: each node is heap-allocated (unelidable) and its address/value is XOR-folded into a u64 that reaches
// nx::bench::sink, so nothing is optimized away.
//
// The ladder is one comparison table with the allocator floor as its baseline, so every row reads against the floor.
// The CUMULATIVE delta each row isolates is the difference between adjacent ns/op values, which the table prints.

#include <clean-core/common/macros.hh>
#include <clean-core/memory/node_allocation.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <nexus/bench/run.hh>
#include <nexus/bench/units.hh>
#include <nexus/pgo.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using cc::isize;
using cc::u64;

namespace
{
// A single make_async_manual<int> create + destroy, pinned as one searchable symbol so its codegen can be disassembled directly: `dev.py assembly show make_async_manual_probe`.
// This is the empty-node path, the ~17 ns beyond the raw node alloc/free: make_shared control init + node ctor, then dec_strong -> destroy_object + teardown_payload + node free.
// Kept alive by a reference from the test below — a TU-local noinline function is otherwise dead-code-eliminated.
// Returns the node address so the create is not elided.
CC_DONT_INLINE u64 make_async_manual_probe()
{
    auto n = cc::make_async_manual<int>();
    return reinterpret_cast<u64>(n.get());
}

// A single make_async_lazy<int> create + destroy of a cold (undriven) node, pinned for disassembly: `dev.py assembly show make_async_lazy_probe`.
// Used to count node allocations, and it must show exactly ONE alloc path.
// The frame is stored inline in the node's payload, so a second alloc means a closure that no longer fits the 32 B slot, or that lost its inline storage.
// The alloc is an inline TLS load of impl::default_node_alloc plus the slab fast path; an out-of-line allocator fetch would be a regression (see default_node_allocator).
CC_DONT_INLINE u64 make_async_lazy_probe()
{
    int const s = 7;
    auto n = cc::make_async_lazy<int>([s] { return s; });
    return reinterpret_cast<u64>(n.get());
}

/// One ladder rung: `G` create(+read)+destroy cycles per iteration, so the per-item figure is one op.
template <class Body>
nx::bench::result rung(cc::string_view name, nx::bench::run_config const& cfg, Body&& body)
{
    constexpr int G = 1024; // ops per iteration; each op is one create(+read)+destroy cycle

    return nx::bench::run(name, cfg,
                          [&](nx::bench::iteration& it)
                          {
                              u64 acc = 0;
                              for (int g = 0; g < G; ++g)
                                  acc ^= body(g);
                              nx::bench::sink(acc);
                              it.items(G);
                          });
}

/// Seconds per op off a measured rung, which is what the PGO report tracks.
double seconds_per_op(nx::bench::result const& r)
{
    return r.items_per_second > 0 ? 1.0 / r.items_per_second : 0.0;
}

void run(bool record)
{
    // The node class async<int> actually allocates from — mirror it exactly for the floor.
    constexpr isize node_size = sizeof(cc::async<int>);
    constexpr isize node_align = alignof(cc::async<int>);
    constexpr cc::node_class_index cls = cc::node_class_index_from_size_and_align(node_size, node_align);
    auto& na = cc::default_node_allocator();

    constexpr auto cfg = nx::bench::run_config{.min_time_secs = 0.1, .max_samples = 512};

    // Floor: raw node alloc + free of async<int>'s class, single-live interleaved (matches the born-ready
    // loop shape: create, use, destroy each iteration).
    // Declared first, so it is the table's baseline — and it doubles as the contamination canary, since nothing else
    // here touches the allocator directly: if it moves, the run is dirty.
    auto const floor = rung("raw node alloc+free (floor)", cfg,
                            [&](int)
                            {
                                byte* p = na.allocate_node_bytes(cls, node_size, node_align);
                                auto const bits = reinterpret_cast<u64>(p);
                                cc::node_allocation_free(p, cls);
                                return bits;
                            });

    // Empty node: make_shared control init + node base ctor/dtor, no payload (manual node dropped unresolved).
    auto const manual = rung("+ make_async_manual", cfg,
                             [](int)
                             {
                                 auto n = cc::make_async_manual<int>();
                                 return reinterpret_cast<u64>(n.get());
                             });

    // + push_value: a born-ready value node, not read.
    rung("+ make_async_from_value", cfg,
         [](int g)
         {
             auto n = cc::make_async_from_value(int(g));
             return reinterpret_cast<u64>(n.get());
         });

    // + try_value: the full born-ready read path.
    auto const full = rung("+ try_value (full born-ready)", cfg,
                           [](int g)
                           {
                               auto n = cc::make_async_from_value(int(g));
                               return u64(*n->try_value());
                           });

    // Separate: a cold lazy node's frame/closure build + destroy, never scheduled/driven.
    // Read against the empty node rather than the row above it.
    auto const lazy = rung("cold make_async_lazy (undriven)", cfg,
                           [](int g)
                           {
                               int const s = g;
                               auto n = cc::make_async_lazy<int>([s] { return s; });
                               return reinterpret_cast<u64>(n.get());
                           });

    // The recorded rows: the floor canary, the two structural stages, and the full born-ready total.
    // The intermediate value row is only interesting as a delta, so recording it would add gate noise without
    // adding coverage.
    if (record)
    {
        auto const* const per_op = &nx::bench::unit_seconds_per_item;
        nx::pgo::report("node-alloc-free (canary)", seconds_per_op(floor), *per_op);
        nx::pgo::report("make_async_manual", seconds_per_op(manual), *per_op);
        nx::pgo::report("born-ready full", seconds_per_op(full), *per_op);
        nx::pgo::report("cold make_async_lazy", seconds_per_op(lazy), *per_op);
    }
}
} // namespace

// Breakdown of the born-ready async overhead over the raw node alloc+free, recorded for the perf gate.
// The whole ladder runs either way, since the rows are cumulative deltas and you cannot measure one without the ones below it.
// `record` only selects which rows are filed as metrics.
// Also hosts the disassembly probes; the trace command targeting this name is in libs/base/clean-core/docs/benchmarks/async-benchmark.md.
PGO_BENCHMARK("bench-async born-ready decomposition")
{
    run(/*record*/ true);

    // Keep the disassembly probes alive (TU-local + noinline would otherwise be dead-code-eliminated).
    nx::bench::sink(make_async_manual_probe());
    nx::bench::sink(make_async_lazy_probe());
}

// The same ladder, as the table a person reads.
//
// The PGO benchmark above files four numbers and prints none: nx::bench::run reports into a BENCHMARK and hands its
// result back everywhere else, which is what keeps a tracked metric from dragging a table along behind it.
BENCHMARK("bench-async - born-ready decomposition")
{
    run(/*record*/ false);
}
