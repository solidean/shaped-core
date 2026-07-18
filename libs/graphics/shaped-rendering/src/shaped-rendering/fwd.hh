#pragma once

#include <clean-core/fwd.hh>
#include <shaped-graphics/fwd.hh>

/// Aggregate forward declarations for shaped-rendering. Include when a forward decl is all you need.

namespace sr
{
// Vocabulary types (i32/u32/f32/isize/...) available bare inside sr, not leaked globally.
using namespace cc::primitive_defines;

// render routines
class render_routine;
class render_routine_package;
class render_routine_library;

template <class RoutineT>
class routine_handle;
} // namespace sr
