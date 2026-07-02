#pragma once

#include <clean-core/fwd.hh>
#include <shaped-rendering/fwd.hh>

/// Aggregate forward declarations for shaped-viewer. Empty for now — the library is an
/// early-stage skeleton; the visualization renderer's types land here as they are implemented.

namespace sv
{
// Vocabulary types (i32/u32/f32/isize/...) available bare inside sv, not leaked globally.
using namespace cc::primitive_defines;
} // namespace sv
