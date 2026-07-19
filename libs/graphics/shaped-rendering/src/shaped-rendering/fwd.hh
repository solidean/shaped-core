#pragma once

#include <clean-core/fwd.hh>
#include <shaped-graphics/fwd.hh>

/// Aggregate forward declarations for shaped-rendering. Include when a forward decl is all you need.

namespace sr
{
// Vocabulary types (i32/u32/f32/isize/...) available bare inside sr, not leaked globally.
using namespace cc::primitive_defines;

// Concrete render routines land here as they are implemented; the routine framework itself lives in
// shaped-graphics (sg::render_routine / ctx.routines).
} // namespace sr
