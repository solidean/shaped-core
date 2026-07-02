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

/// Frame-level GPU lifetime token and direct-queue timeline value: a monotonic counter where
/// reaching value N on the queue's epoch fence means all GPU work of epoch N has finished. See
/// libs/graphics/shaped-graphics/docs/concepts/epochs.md.
enum class epoch : u64
{
    invalid = 0,   ///< null sentinel — "not meaningfully set"
    first = 10000, ///< first live value; deliberately high so an accidental zero-init is obviously wrong
};

/// Finer-grained per-command-list completion token on the direct queue's submission fence.
/// Monotonic, so "is this one list done?" is a single compare against the fence's completed value.
enum class submission_token : u64
{
    invalid = 0,             ///< null sentinel
    first = 30000,           ///< first live value (see epoch::first for the high-value rationale)
    not_submitted = u64(-1), ///< sentinel that always compares "not yet complete"
};

/// A `*_handle` is a std::shared_ptr to a shared-lifetime sg type. context and buffer get handles;
/// command_list does not — it's a single-use temporary held by std::unique_ptr, passed by reference.
/// std::shared_ptr is a placeholder for a future cc::shared_ptr.
using context_handle = std::shared_ptr<context>;
using buffer_handle = std::shared_ptr<buffer>;
} // namespace sg
