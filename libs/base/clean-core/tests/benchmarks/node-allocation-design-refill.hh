#pragma once

#include <clean-core/fwd.hh>

// Opaque, out-of-TU cold-path refill for the design benchmark.
// Its whole purpose is to be a call the benchmark TU's optimizer cannot see through.
// So the fast-path variants must reload their slab base on each allocation, instead of hoisting a single fixed slab into a register.
// That matches the real cc::node_allocator, whose cold path (allocate_node_bytes_non_fast) is likewise an opaque call in another TU and forces the same per-allocation reload.
// Without it the variants report an idealized single-slab number the real allocator cannot hit.
// It never actually fires in the timed loop, since the batch fits one slab.
namespace bench_design
{
// Vocabulary types (i32/u32/u64/isize/byte/...) available bare inside bench_design, not leaked globally.
using namespace cc::primitive_defines;

byte* cold_refill(byte* base);
} // namespace bench_design
