#include "node-allocation-design-refill.hh"

// Deliberately out-of-line and in its own TU (see the header): the benchmark cannot see this body, so it
// must assume any call may change the slab base, defeating base-hoisting on the fast path.
cc::byte* bench_design::cold_refill(cc::byte* base)
{
    // identity refill: reset the local bitmap to all-free and hand the same slab back. The behavior is
    // irrelevant (this never runs in the timed loop) -- opacity to the caller's TU is the entire point.
    *reinterpret_cast<cc::u64*>(base) = ~cc::u64(0);
    return base;
}
