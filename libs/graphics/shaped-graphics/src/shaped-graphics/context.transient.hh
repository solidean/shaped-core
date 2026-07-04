#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/types.hh>

namespace sg
{
/// Resource factory for a context's *transient* lifetime scope, reached as `ctx.transient`. Transient
/// resources are tied to the current epoch and recycled when it retires (see lifetime_scope) — per-frame
/// scratch (intermediate buffers, one-shot binding groups) that never needs to outlive the work that
/// produced it. Using one past its epoch is a hard error.
///
/// A thin facade over its owning context, mirroring context_persistent_scope; it forwards each create_*
/// to the context's backend impl with a transient lifetime.
class context_transient_scope
{
public:
    /// Allocates a transient buffer of `size_in_bytes` (>= 0, 0 is a valid empty buffer). The backend
    /// sub-allocates it from the epoch's pool; the memory is recycled once this epoch retires.
    [[nodiscard]] cc::result<buffer_handle> create_buffer(isize size_in_bytes, buffer_usage usage);

    /// Instantiates `layout` with the given name->view bindings as a transient binding_group (validated
    /// against the layout), whose descriptors are recycled when this epoch retires.
    [[nodiscard]] cc::result<binding_group_handle> create_binding_group(binding_layout_handle layout,
                                                                        cc::span<named_view const> views);

    // Pinned to its owning context: neither copyable nor movable.
    context_transient_scope(context_transient_scope const&) = delete;
    context_transient_scope(context_transient_scope&&) = delete;
    context_transient_scope& operator=(context_transient_scope const&) = delete;
    context_transient_scope& operator=(context_transient_scope&&) = delete;

private:
    friend class context;
    explicit context_transient_scope(context& ctx) : _ctx(ctx) {}

    context& _ctx;
};
} // namespace sg
