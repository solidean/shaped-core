#pragma once

#include <clean-core/fwd.hh>

/// Bare primitive names (`isize`, `u8`, `u32`, `u64`, …) inside `scl`, matching the rest of the tree.
/// `isize` is the signed size/index type everywhere; byte offsets into source are stored as `u32`.
namespace scl
{
using namespace cc::primitive_defines;
} // namespace scl
