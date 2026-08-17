#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/memory/allocation_info.hh>
#include <shaped-graphics/resource/buffer.hh>               // typed buffer<T> wrapper (returned by create_buffer below)
#include <shaped-graphics/resource/texture_descriptions.hh> // shape-specific descriptions + the typed factories below
#include <shaped-graphics/types.hh>

/// Resource factory for a context's *persistent* lifetime scope, reached as `ctx.persistent`.
/// Persistent resources live until their handles are released, unlike ctx.transient's, which the backend recycles per epoch.
/// See lifetime_scope.
///
/// Every create comes in a `try_create_*` fallible core and a throwing `create_*` default — the pattern in docs/error-handling.md.
/// The throwing flavor raises sg::allocation_exception or sg::binding_group_exception, and sg::device_lost_exception if the device was lost.
/// Contract violations assert in either flavor — they are bugs, not runtime failures.
class sg::context_persistent_scope
{
    // buffers
public:
    /// Allocates a GPU-resident buffer; size must be >= 0, and 0 is a valid empty buffer with no allocation.
    /// `alloc` selects the backing memory (see allocation_info) and must name the persistent scope.
    /// Throws sg::allocation_exception on allocation failure.
    [[nodiscard]] raw_buffer_handle create_raw_buffer(isize size_in_bytes,
                                                      buffer_usages usage,
                                                      allocation_info const& alloc = {});

    [[nodiscard]] cc::result<raw_buffer_handle> try_create_raw_buffer(isize size_in_bytes,
                                                                      buffer_usages usage,
                                                                      allocation_info const& alloc = {});

    // Typed buffer factory — allocates `element_count` elements of `T`, so element_count * sizeof(T) bytes.
    // Returns the wrapped `buffer<T>`, whose view factories are typed by `T`.
    // `element_count` must be >= 0, and 0 is a valid empty buffer.
    // Error behaviour mirrors create_raw_buffer.

    template <class T>
    [[nodiscard]] buffer<T> create_buffer(isize element_count, buffer_usages usage, allocation_info const& alloc = {})
    {
        return buffer<T>::from_raw(create_raw_buffer(element_count * isize(sizeof(T)), usage, alloc));
    }

    template <class T>
    [[nodiscard]] cc::result<buffer<T>> try_create_buffer(isize element_count,
                                                          buffer_usages usage,
                                                          allocation_info const& alloc = {})
    {
        auto r = try_create_raw_buffer(element_count * isize(sizeof(T)), usage, alloc);
        if (r.has_value())
            return buffer<T>::from_raw(cc::move(r).value());
        return cc::error(cc::move(r).error());
    }

    // textures
public:
    /// Allocates a GPU-resident texture from a description; `alloc` selects the backing memory, dedicated by default.
    /// Returns the raw handle — wrap it in a `texture<Traits>` for shape-checked access.
    /// Throws sg::allocation_exception on allocation failure.
    [[nodiscard]] raw_texture_handle create_raw_texture(texture_description const& desc,
                                                        allocation_info const& alloc = {});

    [[nodiscard]] cc::result<raw_texture_handle> try_create_raw_texture(texture_description const& desc,
                                                                        allocation_info const& alloc = {});

    // Typed texture factories — take a shape-specific description (see texture_descriptions.hh), expand it to a full texture_description, and return the wrapped `texture<Traits>`.
    // `create_texture` / `try_create_texture` are the generic core, deducing the shape from the description.
    // The named `create_texture_2d` / … wrappers exist so the description can be brace-initialized at the call site: `create_texture_2d({.width = 256, ...})`.
    // The deduced `create_texture` template cannot do that.
    // Error behaviour mirrors create_raw_texture.

    template <class Desc>
    [[nodiscard]] typename Desc::texture_type create_texture(Desc const& desc, allocation_info const& alloc = {})
    {
        return Desc::texture_type::from_raw(create_raw_texture(desc.to_texture_description(), alloc));
    }

    template <class Desc>
    [[nodiscard]] cc::result<typename Desc::texture_type> try_create_texture(Desc const& desc,
                                                                             allocation_info const& alloc = {})
    {
        auto r = try_create_raw_texture(desc.to_texture_description(), alloc);
        if (r.has_value())
            return Desc::texture_type::from_raw(cc::move(r).value());
        return cc::error(cc::move(r).error());
    }

    [[nodiscard]] texture_1d create_texture_1d(texture_1d_description const& d, allocation_info const& alloc = {})
    {
        return create_texture(d, alloc);
    }
    [[nodiscard]] cc::result<texture_1d> try_create_texture_1d(texture_1d_description const& d,
                                                               allocation_info const& alloc = {})
    {
        return try_create_texture(d, alloc);
    }

    [[nodiscard]] texture_2d create_texture_2d(texture_2d_description const& d, allocation_info const& alloc = {})
    {
        return create_texture(d, alloc);
    }
    [[nodiscard]] cc::result<texture_2d> try_create_texture_2d(texture_2d_description const& d,
                                                               allocation_info const& alloc = {})
    {
        return try_create_texture(d, alloc);
    }

    [[nodiscard]] texture_3d create_texture_3d(texture_3d_description const& d, allocation_info const& alloc = {})
    {
        return create_texture(d, alloc);
    }
    [[nodiscard]] cc::result<texture_3d> try_create_texture_3d(texture_3d_description const& d,
                                                               allocation_info const& alloc = {})
    {
        return try_create_texture(d, alloc);
    }

    [[nodiscard]] texture_cube create_texture_cube(texture_cube_description const& d, allocation_info const& alloc = {})
    {
        return create_texture(d, alloc);
    }
    [[nodiscard]] cc::result<texture_cube> try_create_texture_cube(texture_cube_description const& d,
                                                                   allocation_info const& alloc = {})
    {
        return try_create_texture(d, alloc);
    }

    [[nodiscard]] texture_1d_array create_texture_1d_array(texture_1d_array_description const& d,
                                                           allocation_info const& alloc = {})
    {
        return create_texture(d, alloc);
    }
    [[nodiscard]] cc::result<texture_1d_array> try_create_texture_1d_array(texture_1d_array_description const& d,
                                                                           allocation_info const& alloc = {})
    {
        return try_create_texture(d, alloc);
    }

    [[nodiscard]] texture_2d_array create_texture_2d_array(texture_2d_array_description const& d,
                                                           allocation_info const& alloc = {})
    {
        return create_texture(d, alloc);
    }
    [[nodiscard]] cc::result<texture_2d_array> try_create_texture_2d_array(texture_2d_array_description const& d,
                                                                           allocation_info const& alloc = {})
    {
        return try_create_texture(d, alloc);
    }

    [[nodiscard]] texture_cube_array create_texture_cube_array(texture_cube_array_description const& d,
                                                               allocation_info const& alloc = {})
    {
        return create_texture(d, alloc);
    }
    [[nodiscard]] cc::result<texture_cube_array> try_create_texture_cube_array(texture_cube_array_description const& d,
                                                                               allocation_info const& alloc = {})
    {
        return try_create_texture(d, alloc);
    }

    [[nodiscard]] texture_2d_ms create_texture_2d_ms(texture_2d_ms_description const& d,
                                                     allocation_info const& alloc = {})
    {
        return create_texture(d, alloc);
    }
    [[nodiscard]] cc::result<texture_2d_ms> try_create_texture_2d_ms(texture_2d_ms_description const& d,
                                                                     allocation_info const& alloc = {})
    {
        return try_create_texture(d, alloc);
    }

    [[nodiscard]] texture_2d_array_ms create_texture_2d_array_ms(texture_2d_array_ms_description const& d,
                                                                 allocation_info const& alloc = {})
    {
        return create_texture(d, alloc);
    }
    [[nodiscard]] cc::result<texture_2d_array_ms> try_create_texture_2d_array_ms(texture_2d_array_ms_description const& d,
                                                                                 allocation_info const& alloc = {})
    {
        return try_create_texture(d, alloc);
    }

    [[nodiscard]] texture_cube_ms create_texture_cube_ms(texture_cube_ms_description const& d,
                                                         allocation_info const& alloc = {})
    {
        return create_texture(d, alloc);
    }
    [[nodiscard]] cc::result<texture_cube_ms> try_create_texture_cube_ms(texture_cube_ms_description const& d,
                                                                         allocation_info const& alloc = {})
    {
        return try_create_texture(d, alloc);
    }

    [[nodiscard]] texture_cube_array_ms create_texture_cube_array_ms(texture_cube_array_ms_description const& d,
                                                                     allocation_info const& alloc = {})
    {
        return create_texture(d, alloc);
    }
    [[nodiscard]] cc::result<texture_cube_array_ms> try_create_texture_cube_array_ms(
        texture_cube_array_ms_description const& d,
        allocation_info const& alloc = {})
    {
        return try_create_texture(d, alloc);
    }

    // memory heaps
public:
    /// Allocates a GPU memory heap of `size_in_bytes` that placed resources sub-allocate into.
    /// Query a resource's requirements from the heap, pick an offset, then create_* with that placement.
    /// Size must be >= 0, and 0 is a valid empty heap; throws sg::allocation_exception on allocation failure.
    [[nodiscard]] memory_heap_handle create_memory_heap(isize size_in_bytes);

    [[nodiscard]] cc::result<memory_heap_handle> try_create_memory_heap(isize size_in_bytes);

    // bind path
    // binding_group_layout / pipeline_layout / compute_pipeline creation is not here — those are schemas / PSOs, not lifetime-scoped GPU resources.
    // They live on the raw `ctx.uncached` scope; prefer `ctx.cached`.
public:
    /// Instantiates `layout` with the given name→view bindings, validating each against the layout.
    /// Throws sg::binding_group_exception on a wiring error (unknown/missing binding, kind mismatch) or descriptor-heap exhaustion.
    [[nodiscard]] binding_group_handle create_binding_group(binding_group_layout_handle layout,
                                                            cc::span<named_view const> views,
                                                            cc::span<named_sampler const> samplers = {});

    [[nodiscard]] cc::result<binding_group_handle> try_create_binding_group(binding_group_layout_handle layout,
                                                                            cc::span<named_view const> views,
                                                                            cc::span<named_sampler const> samplers = {});

    // Pinned to its owning context: neither copyable nor movable.
    context_persistent_scope(context_persistent_scope const&) = delete;
    context_persistent_scope(context_persistent_scope&&) = delete;
    context_persistent_scope& operator=(context_persistent_scope const&) = delete;
    context_persistent_scope& operator=(context_persistent_scope&&) = delete;

private:
    // Only a context constructs its own scope; the scope in turn reaches the context's protected backend virtuals (mutual friendship).
    friend class context;
    explicit context_persistent_scope(context& ctx) : _ctx(ctx) {}

    context& _ctx;
};
