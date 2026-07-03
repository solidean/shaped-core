#pragma once

#include <clean-core/error/result.hh>
#include <shaped-graphics/allocation_info.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/types.hh>

namespace sg
{
/// Resource factory for a context's *persistent* lifetime scope, reached as `ctx.persistent`.
/// Persistent resources live until their handles are released (as opposed to a future transient scope,
/// whose resources the backend recycles per frame/epoch). See allocation_scope.
///
/// A thin facade: it holds a back-reference to its context and forwards each create_* to the context's
/// backend implementation. A context owns its `persistent` member — you never construct one yourself,
/// and it stays valid as long as the context does.
class persistent_scope
{
public:
    /// Allocates a GPU-resident buffer in the persistent scope. Size must be >= 0 (0 is a valid empty
    /// buffer, no allocation). `alloc` chooses the backing memory: a dedicated allocation by default
    /// (its own committed resource), or a placement into a memory_heap when one is attached. Returns a
    /// shared buffer handle.
    [[nodiscard]] cc::result<buffer_handle> create_buffer(isize size_in_bytes,
                                                          buffer_usage usage,
                                                          allocation_info const& alloc = {});

    // Pinned to its owning context: neither copyable nor movable.
    persistent_scope(persistent_scope const&) = delete;
    persistent_scope(persistent_scope&&) = delete;
    persistent_scope& operator=(persistent_scope const&) = delete;
    persistent_scope& operator=(persistent_scope&&) = delete;

private:
    // Only a context constructs its own scope; the scope in turn reaches the context's protected
    // backend virtuals (mutual friendship).
    friend class context;
    explicit persistent_scope(context& ctx) : _ctx(ctx) {}

    context& _ctx;
};
} // namespace sg
