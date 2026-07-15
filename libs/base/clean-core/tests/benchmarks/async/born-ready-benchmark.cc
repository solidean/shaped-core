// Born-ready cost decomposition — where the ~40 ns of a born-ready async goes.
//
// A born-ready async (make_async_from_value -> try_value -> destroy) is exactly ONE node-allocator
// alloc + free plus the node's own control/payload/state bookkeeping. The full-ladder benchmark
// (async-benchmark.cc) measures the ~40 ns total; the 64 B node allocator alloc+free pair is ~2.9 ns
// (node-allocation-design-benchmark.cc), so ~37 ns is non-allocator overhead. This benchmark splits that
// out by measuring a cumulative ladder of increasingly-complete operations on the SAME harness (median of
// 5, same machine/timer), so each row's delta isolates one component:
//
//   raw node alloc+free           the allocator floor (async<int>'s own node class)
//   + make_async_manual           adds make_shared control init (intrusive refcount) + node ctor/dtor
//   + make_async_from_value       adds push_value: build the value slot + state -> ready_value + teardown
//   + try_value                   adds the zero-copy value read
//   cold make_async_lazy          separately: a lazy node's frame/closure build+destroy (never driven)
//
// The manual and cold-lazy rows drop an UNRESOLVED node on purpose — that must be safe (frame torn down,
// no leak, no assert); if it ever isn't, that is a bug in async teardown, not in this benchmark.
//
// Anti-fold: each node is heap-allocated (unelidable) and its address/value is XOR-folded into the u64
// returned from each pass -> bench::sink, so nothing is optimized away.
//
// Manual test (nx::config::manual): run by exact name.

#include "../bench_util.hh"

#include <clean-core/common/macros.hh>
#include <clean-core/memory/node_allocation.hh>
#include <clean-core/thread/async.hh>
#include <nexus/test.hh>

#include <cstdio>

using cc::isize;
using cc::u64;

namespace
{
// A single make_async_manual<int> create + destroy, pinned as one searchable symbol so its codegen can be
// disassembled directly: `dev.py assembly show make_async_manual_probe`. This is the empty-node path — the
// ~17 ns beyond the raw node alloc/free (make_shared control init + node ctor, then dec_strong ->
// destroy_object + teardown_payload + node free). Kept alive by a reference from the test below (a TU-local
// noinline function is otherwise dead-code-eliminated). Returns the node address so the create is not elided.
CC_DONT_INLINE u64 make_async_manual_probe()
{
    auto n = cc::make_async_manual<int>();
    return reinterpret_cast<u64>(n.get());
}

// A single make_async_lazy<int> create + destroy of a cold (undriven) node, pinned for disassembly:
// `dev.py assembly show make_async_lazy_probe`. Used to count node allocations — if the frame closure is a
// separate unique_function node alloc (no SSO), this shows TWO default_node_allocator/allocate paths, not one.
CC_DONT_INLINE u64 make_async_lazy_probe()
{
    int const s = 7;
    auto n = cc::make_async_lazy<int>([s] { return s; });
    return reinterpret_cast<u64>(n.get());
}

// One ladder row: ns/op and the delta from a reference stage (prev_ns < 0 prints "-" for the floor row).
void report(char const* label, double ops_per_sec, double prev_ns, char const* note)
{
    double const ns = 1e9 / ops_per_sec;
    if (prev_ns < 0)
        std::printf("%-34s %9.2f %12s   %s\n", label, ns, "-", note);
    else
        std::printf("%-34s %9.2f %+12.2f   %s\n", label, ns, ns - prev_ns, note);
}

void run()
{
    constexpr int G = 1024; // ops per timed pass; each op is one create(+read)+destroy cycle
    double const units = double(G);

    // The node class async<int> actually allocates from — mirror it exactly for the floor.
    constexpr isize node_size = sizeof(cc::async<int>);
    constexpr isize node_align = alignof(cc::async<int>);
    constexpr cc::node_class_index cls = cc::node_class_index_from_size_and_align(node_size, node_align);
    auto& na = cc::default_node_allocator();

    // Floor: raw node alloc + free of async<int>'s class, single-live interleaved (matches the born-ready
    // loop shape: create, use, destroy each iteration).
    double const floor = bench::median_units_per_sec(units,
                                                     [&]
                                                     {
                                                         u64 acc = 0;
                                                         for (int g = 0; g < G; ++g)
                                                         {
                                                             cc::byte* p
                                                                 = na.allocate_node_bytes(cls, node_size, node_align);
                                                             acc ^= reinterpret_cast<u64>(p);
                                                             cc::node_allocation_free(p, cls);
                                                         }
                                                         return acc;
                                                     });

    // Empty node: make_shared control init + node base ctor/dtor, no payload (manual node dropped unresolved).
    double const manual = bench::median_units_per_sec(units,
                                                      [&]
                                                      {
                                                          u64 acc = 0;
                                                          for (int g = 0; g < G; ++g)
                                                          {
                                                              auto n = cc::make_async_manual<int>();
                                                              acc ^= reinterpret_cast<u64>(n.get());
                                                          }
                                                          return acc;
                                                      });

    // + push_value: a born-ready value node, not read.
    double const value = bench::median_units_per_sec(units,
                                                     [&]
                                                     {
                                                         u64 acc = 0;
                                                         for (int g = 0; g < G; ++g)
                                                         {
                                                             auto n = cc::make_async_from_value(int(g));
                                                             acc ^= reinterpret_cast<u64>(n.get());
                                                         }
                                                         return acc;
                                                     });

    // + try_value: the full born-ready read path.
    double const full = bench::median_units_per_sec(units,
                                                    [&]
                                                    {
                                                        u64 acc = 0;
                                                        for (int g = 0; g < G; ++g)
                                                        {
                                                            auto n = cc::make_async_from_value(int(g));
                                                            acc ^= u64(*n->try_value());
                                                        }
                                                        return acc;
                                                    });

    // Separate: a cold lazy node's frame/closure build + destroy, never scheduled/driven.
    double const lazy = bench::median_units_per_sec(units,
                                                    [&]
                                                    {
                                                        u64 acc = 0;
                                                        for (int g = 0; g < G; ++g)
                                                        {
                                                            int const s = g;
                                                            auto n = cc::make_async_lazy<int>([s] { return s; });
                                                            acc ^= reinterpret_cast<u64>(n.get());
                                                        }
                                                        return acc;
                                                    });

    std::printf("\n=== born-ready cost decomposition (median of 5; async<int> = %lld B, node class %d) ===\n",
                (long long)node_size, int(cls));
    std::printf("%-34s %9s %12s   %s\n", "stage", "ns/op", "delta", "isolates");
    std::printf("%-34s %9s %12s   %s\n", "-----", "-----", "-----", "--------");

    double const floor_ns = 1e9 / floor;
    double const manual_ns = 1e9 / manual;
    double const value_ns = 1e9 / value;

    report("raw node alloc+free (floor)", floor, -1, "allocator only");
    report("+ make_async_manual", manual, floor_ns, "control init + node ctor/dtor");
    report("+ make_async_from_value", value, manual_ns, "push_value: build slot + ready + teardown");
    report("+ try_value (full born-ready)", full, value_ns, "value read");
    report("cold make_async_lazy (undriven)", lazy, manual_ns, "frame/closure build + teardown");

    std::printf("\nladder deltas are cumulative (vs the row above); the lazy row is vs the empty node.\n");
    std::printf("op = one create(+read)+destroy cycle. full born-ready total = the '+ try_value' ns/op.\n");
    std::fflush(stdout);
}
} // namespace

// Breakdown of the born-ready async overhead over the raw node alloc+free. Run by exact name:
//   uv run dev.py test "bench-async born-ready decomposition" --mirror-output
TEST("bench-async born-ready decomposition", nx::config::manual)
{
    run();

    // Keep the disassembly probes alive (TU-local + noinline would otherwise be dead-code-eliminated).
    bench::sink ^= make_async_manual_probe();
    bench::sink ^= make_async_lazy_probe();
}
