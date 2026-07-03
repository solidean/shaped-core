#pragma once

#include <clean-core/container/span.hh>
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
/// A thin facade over its owning context: it forwards each create_* to the context's backend impl.
class context_persistent_scope
{
public:
    /// Allocates a GPU-resident buffer. Size must be >= 0 (0 is a valid empty buffer, no allocation).
    /// `alloc` selects the backing memory (see allocation_info).
    [[nodiscard]] cc::result<buffer_handle> create_buffer(isize size_in_bytes,
                                                          buffer_usage usage,
                                                          allocation_info const& alloc = {});

    /// Builds a binding_layout (the bindable-set schema) from a shader's reflected bindings.
    [[nodiscard]] cc::result<binding_layout_handle> create_binding_layout(cc::span<binding const> bindings);

    /// Builds a compute_pipeline from a compute `shader` compiled against `layout`.
    [[nodiscard]] cc::result<compute_pipeline_handle> create_compute_pipeline(compiled_shader const& shader,
                                                                              binding_layout_handle layout);

    /// Instantiates `layout` with the given name→view bindings, validating each against the layout.
    [[nodiscard]] cc::result<binding_group_handle> create_binding_group(binding_layout_handle layout,
                                                                        cc::span<named_view const> views);

    // Pinned to its owning context: neither copyable nor movable.
    context_persistent_scope(context_persistent_scope const&) = delete;
    context_persistent_scope(context_persistent_scope&&) = delete;
    context_persistent_scope& operator=(context_persistent_scope const&) = delete;
    context_persistent_scope& operator=(context_persistent_scope&&) = delete;

private:
    // Only a context constructs its own scope; the scope in turn reaches the context's protected
    // backend virtuals (mutual friendship).
    friend class context;
    explicit context_persistent_scope(context& ctx) : _ctx(ctx) {}

    context& _ctx;
};
} // namespace sg
