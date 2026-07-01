#pragma once

#include <clean-core/fwd.hh>
#include <shaped-graphics/backend/fwd.hh>

#include <memory>

/// Aggregate forward declarations for shaped-graphics, plus the `*_handle` shared-pointer
/// typedefs. Include this when a fwd decl is all you need.

namespace sg
{
// Vocabulary types (i32/u32/u64/isize/byte/...) available bare inside sg, not leaked globally.
using namespace cc::primitive_defines;

class context;
class command_list;
class buffer;

/// A `*_handle` is a std::shared_ptr to an sg type. `context` and `command_list` are the
/// mutable drivers; `buffer` (and later `texture`) handles are shared-immutable views over
/// GPU-resident data — the shape is fixed, the underlying GPU memory is not.
///
/// std::shared_ptr is deliberate for now; a future `cc::shared_ptr` would replace it (see the
/// sg coding guidelines' note on surfacing that clean-core extension).
using context_handle = std::shared_ptr<context>;
using command_list_handle = std::shared_ptr<command_list>;
using buffer_handle = std::shared_ptr<buffer>;
} // namespace sg
