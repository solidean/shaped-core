#pragma once

#include <clean-core/fwd.hh>

// Opaque, out-of-TU cold-path refill for the design benchmark. Its whole purpose is to be a call the
// benchmark TU's optimizer cannot see through, so the fast-path variants must reload their slab base each
// allocation instead of hoisting a single fixed slab into a register. That matches the real cc::node_allocator,
// whose cold path (allocate_node_bytes_non_fast) is likewise an opaque call in another TU and therefore forces
// a per-allocation reload of slab_base. Without this the variants report an idealized single-slab number the
// real allocator cannot hit. It never actually fires in the timed loop (the batch fits one slab).
namespace bench_design
{
cc::byte* cold_refill(cc::byte* base);
}
