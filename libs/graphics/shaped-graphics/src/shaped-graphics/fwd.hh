#pragma once

#include <clean-core/fwd.hh>

#include <memory>

/// Forward declarations and `*_handle` typedefs for shaped-graphics. Include when a forward decl is
/// all you need.

namespace sg
{
// Vocabulary types (i32/u32/u64/isize/byte/...) available bare inside sg, not leaked globally.
using namespace cc::primitive_defines;

class context;
class command_list;
class buffer;

/// A `*_handle` is a std::shared_ptr to a shared-lifetime sg type. context and buffer get handles;
/// command_list does not — it's a single-use temporary held by std::unique_ptr, passed by reference.
/// std::shared_ptr is a placeholder for a future cc::shared_ptr.
using context_handle = std::shared_ptr<context>;
using buffer_handle = std::shared_ptr<buffer>;
} // namespace sg
