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
/// whose resources the backend recycles per frame/epoch). See lifetime_scope.
///
/// A thin facade over its owning context: it forwards each create_* to the context's backend impl.
///
/// Every create comes in two flavors (see docs/error-handling.md): a `try_create_*` fallible core that
/// returns cc::result (for callers that stay exception-free or have a local fallback), and a
/// `create_*` throwing default that returns the handle directly and raises a typed sg exception on
/// failure (sg::allocation_exception / sg::pipeline_creation_exception / sg::binding_group_exception,
/// or sg::device_lost_exception if the device was lost). Contract violations (bad size, wrong scope,
/// null args) assert in either flavor — they are bugs, not runtime failures.
class context_persistent_scope
{
    // buffers
public:
    /// Allocates a GPU-resident buffer. Size must be >= 0 (0 is a valid empty buffer, no allocation).
    /// `alloc` selects the backing memory (see allocation_info). Throws sg::allocation_exception on
    /// allocation failure.
    [[nodiscard]] raw_buffer_handle create_raw_buffer(isize size_in_bytes,
                                                      buffer_usage usage,
                                                      allocation_info const& alloc = {});

    /// Fallible core of create_raw_buffer — returns an error instead of throwing.
    [[nodiscard]] cc::result<raw_buffer_handle> try_create_raw_buffer(isize size_in_bytes,
                                                                      buffer_usage usage,
                                                                      allocation_info const& alloc = {});

    // textures
public:
    /// Allocates a GPU-resident texture from a description. `alloc` selects the backing memory (dedicated
    /// by default). Returns the raw handle; wrap it in a `texture<Traits>` for shape-checked access.
    /// Throws sg::allocation_exception on allocation failure.
    [[nodiscard]] raw_texture_handle create_raw_texture(texture_description const& desc,
                                                        allocation_info const& alloc = {});

    /// Fallible core of create_raw_texture — returns an error instead of throwing.
    [[nodiscard]] cc::result<raw_texture_handle> try_create_raw_texture(texture_description const& desc,
                                                                        allocation_info const& alloc = {});

    // memory heaps
public:
    /// Allocates a GPU memory heap of `size_in_bytes` that placed resources sub-allocate into (query a
    /// resource's requirements from the heap, pick an offset, then create_* with that placement). Size
    /// must be >= 0 (0 is a valid empty heap). Throws sg::allocation_exception on allocation failure.
    [[nodiscard]] memory_heap_handle create_memory_heap(isize size_in_bytes);

    /// Fallible core of create_memory_heap — returns an error instead of throwing.
    [[nodiscard]] cc::result<memory_heap_handle> try_create_memory_heap(isize size_in_bytes);

    // bind path
public:
    /// Builds a binding_layout (the bindable-set schema) from a shader's reflected bindings. Throws
    /// sg::pipeline_creation_exception on failure.
    [[nodiscard]] binding_layout_handle create_binding_layout(cc::span<binding const> bindings);

    /// Fallible core of create_binding_layout — returns an error instead of throwing.
    [[nodiscard]] cc::result<binding_layout_handle> try_create_binding_layout(cc::span<binding const> bindings);

    /// Builds a compute_pipeline from a description (compute shader + layout). Throws
    /// sg::pipeline_creation_exception on failure.
    [[nodiscard]] compute_pipeline_handle create_compute_pipeline(compute_pipeline_description const& desc);

    /// Fallible core of create_compute_pipeline — returns an error instead of throwing.
    [[nodiscard]] cc::result<compute_pipeline_handle> try_create_compute_pipeline(compute_pipeline_description const& desc);

    /// Instantiates `layout` with the given name→view bindings, validating each against the layout.
    /// Throws sg::binding_group_exception on a wiring error (unknown/missing binding, kind mismatch) or
    /// descriptor-heap exhaustion.
    [[nodiscard]] binding_group_handle create_binding_group(binding_layout_handle layout,
                                                            cc::span<named_view const> views);

    /// Fallible core of create_binding_group — returns an error instead of throwing.
    [[nodiscard]] cc::result<binding_group_handle> try_create_binding_group(binding_layout_handle layout,
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
