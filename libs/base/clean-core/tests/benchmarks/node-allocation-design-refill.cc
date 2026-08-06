#include "node-allocation-design-refill.hh"

using namespace cc::primitive_defines;

// Deliberately out-of-line and in its own TU (see the header): the benchmark cannot see this body, so it
// must assume any call may change the slab base, defeating base-hoisting on the fast path.
byte* bench_design::cold_refill(byte* base)
{
    // identity refill: reset the local bitmap to all-free and hand the same slab back.
    // the behavior is irrelevant, since this never runs in the timed loop — opacity to the caller's TU is the entire point.
    *reinterpret_cast<u64*>(base) = ~u64(0);
    return base;
}
