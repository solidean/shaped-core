#pragma once

#include <clean-core/fwd.hh>
#include <shaped-graphics/fwd.hh>

/// Aggregate forward declarations for shaped-rendering. Empty for now — the library is an
/// early-stage skeleton; render routines and their types land here as they are implemented.

namespace sr
{
// Vocabulary types (i32/u32/f32/isize/...) available bare inside sr, not leaked globally.
using namespace cc::primitive_defines;
} // namespace sr
