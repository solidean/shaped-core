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
// Manual test (nx::config::manual): prints only, no CHECK.

#include "bench_util.hh"

#include <clean-core/math/random.hh>
#include <clean-core/memory/allocation.hh>
#include <nexus/test.hh>

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

void run()
{
    isize const sizes[] = {16, 32, 64, 128, 256, 512, 1024, 4096, 16384, 65536};
    isize const align = 16; // typical malloc alignment; both resources honor it

    std::printf("\n=== allocation throughput (M alloc+free / s) — 64 live, churn ===\n");
    std::printf("%10s %14s %14s\n", "size", "mimalloc", "system");
    std::printf("%10s %14s %14s\n", "----", "--------", "------");
    for (isize const size : sizes)
    {
        double const mi = mops(*cc::default_memory_resource, size, align);
        double const sys = mops(cc::system_memory_resource, size, align);
        std::printf("%10lld %14.1f %14.1f\n", (long long)size, mi, sys);
    }
    std::fflush(stdout);
}
} // namespace

TEST("bench-alloc (mimalloc vs system)", nx::config::manual)
{
    run();
}
