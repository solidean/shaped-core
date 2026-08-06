#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/result.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/buffer.hh>               // typed buffer<T> wrapper (returned by create_buffer below)
#include <shaped-graphics/resource/texture_descriptions.hh> // shape-specific descriptions + the typed factories below
#include <shaped-graphics/types.hh>

namespace sg
{
/// Resource factory for a context's *transient* lifetime scope, reached as `ctx.transient`.
/// Transient resources are tied to the current epoch and recycled when it retires (see lifetime_scope) — per-frame scratch that never needs to outlive the work that produced it.
/// Using one past its epoch is a hard error.
///
/// Buffers are sub-allocated by a per-epoch bump allocator over one persistent memory_heap the scope owns.
/// The head resets to 0 whenever the epoch changes, so successive epochs alias the same storage — safe because a direct queue executes each epoch's GPU work before the next's.
/// Requests larger than the budget fall back to a dedicated (committed) allocation.
///
/// Every create comes in a `try_create_*` fallible core and a throwing `create_*` default, as on context_persistent_scope — the pattern in docs/error-handling.md.
class context_transient_scope
{
    // buffers
public:
    /// Allocates a transient buffer of `size_in_bytes` from the current epoch's bump window; the storage is reused once the epoch changes.
    /// Size must be >= 0, and 0 is a valid empty buffer.
    /// Throws sg::allocation_exception on allocation failure.
    [[nodiscard]] raw_buffer_handle create_raw_buffer(isize size_in_bytes, buffer_usage usage);

    [[nodiscard]] cc::result<raw_buffer_handle> try_create_raw_buffer(isize size_in_bytes, buffer_usage usage);

    // Typed buffer factory — allocates `element_count` elements of `T`, so element_count * sizeof(T) bytes.
    // Returns the wrapped `buffer<T>`, whose view factories are typed by `T`.
    // `element_count` must be >= 0, and 0 is a valid empty buffer.
    // Error behaviour mirrors create_raw_buffer.

    template <class T>
    [[nodiscard]] buffer<T> create_buffer(isize element_count, buffer_usage usage)
    {
        return buffer<T>::from_raw(create_raw_buffer(element_count * isize(sizeof(T)), usage));
    }

    template <class T>
    [[nodiscard]] cc::result<buffer<T>> try_create_buffer(isize element_count, buffer_usage usage)
    {
        auto r = try_create_raw_buffer(element_count * isize(sizeof(T)), usage);
        if (r.has_value())
            return buffer<T>::from_raw(cc::move(r).value());
        return cc::error(cc::move(r).error());
    }

    // textures
public:
    /// Allocates a transient texture, recycled once this epoch retires.
    /// Throws sg::allocation_exception on allocation failure.
    /// The transient bump-heap is buffers-only today, so a transient texture is a *dedicated* allocation auto-expired at the next epoch, not bump-suballocated.
    /// Placed transient textures wait on a texture-capable transient memory_heap.
    [[nodiscard]] raw_texture_handle create_raw_texture(texture_description const& desc);

    [[nodiscard]] cc::result<raw_texture_handle> try_create_raw_texture(texture_description const& desc);

    // Typed texture factories — take a shape-specific description (see texture_descriptions.hh), expand it to a full texture_description, and return the wrapped `texture<Traits>`.
    // `create_texture` / `try_create_texture` are the generic core, deducing the shape from the description.
    // The named `create_texture_2d` / … wrappers exist so the description can be brace-initialized at the call site: `create_texture_2d({.width = 256, ...})`.
    // The deduced `create_texture` template cannot do that.
    // Error behaviour mirrors create_raw_texture.

    template <class Desc>
    [[nodiscard]] typename Desc::texture_type create_texture(Desc const& desc)
    {
        return Desc::texture_type::from_raw(create_raw_texture(desc.to_texture_description()));
    }

    template <class Desc>
    [[nodiscard]] cc::result<typename Desc::texture_type> try_create_texture(Desc const& desc)
    {
        auto r = try_create_raw_texture(desc.to_texture_description());
        if (r.has_value())
            return Desc::texture_type::from_raw(cc::move(r).value());
        return cc::error(cc::move(r).error());
    }

    [[nodiscard]] texture_1d create_texture_1d(texture_1d_description const& d) { return create_texture(d); }
    [[nodiscard]] cc::result<texture_1d> try_create_texture_1d(texture_1d_description const& d)
    {
        return try_create_texture(d);
    }

    [[nodiscard]] texture_2d create_texture_2d(texture_2d_description const& d) { return create_texture(d); }
    [[nodiscard]] cc::result<texture_2d> try_create_texture_2d(texture_2d_description const& d)
    {
        return try_create_texture(d);
    }

    [[nodiscard]] texture_3d create_texture_3d(texture_3d_description const& d) { return create_texture(d); }
    [[nodiscard]] cc::result<texture_3d> try_create_texture_3d(texture_3d_description const& d)
    {
        return try_create_texture(d);
    }

    [[nodiscard]] texture_cube create_texture_cube(texture_cube_description const& d) { return create_texture(d); }
    [[nodiscard]] cc::result<texture_cube> try_create_texture_cube(texture_cube_description const& d)
    {
        return try_create_texture(d);
    }

    [[nodiscard]] texture_1d_array create_texture_1d_array(texture_1d_array_description const& d)
    {
        return create_texture(d);
    }
    [[nodiscard]] cc::result<texture_1d_array> try_create_texture_1d_array(texture_1d_array_description const& d)
    {
        return try_create_texture(d);
    }

    [[nodiscard]] texture_2d_array create_texture_2d_array(texture_2d_array_description const& d)
    {
        return create_texture(d);
    }
    [[nodiscard]] cc::result<texture_2d_array> try_create_texture_2d_array(texture_2d_array_description const& d)
    {
        return try_create_texture(d);
    }

    [[nodiscard]] texture_cube_array create_texture_cube_array(texture_cube_array_description const& d)
    {
        return create_texture(d);
    }
    [[nodiscard]] cc::result<texture_cube_array> try_create_texture_cube_array(texture_cube_array_description const& d)
    {
        return try_create_texture(d);
    }

    [[nodiscard]] texture_2d_ms create_texture_2d_ms(texture_2d_ms_description const& d) { return create_texture(d); }
    [[nodiscard]] cc::result<texture_2d_ms> try_create_texture_2d_ms(texture_2d_ms_description const& d)
    {
        return try_create_texture(d);
    }

    [[nodiscard]] texture_2d_array_ms create_texture_2d_array_ms(texture_2d_array_ms_description const& d)
    {
        return create_texture(d);
    }
    [[nodiscard]] cc::result<texture_2d_array_ms> try_create_texture_2d_array_ms(texture_2d_array_ms_description const& d)
    {
        return try_create_texture(d);
    }

    [[nodiscard]] texture_cube_ms create_texture_cube_ms(texture_cube_ms_description const& d)
    {
        return create_texture(d);
    }
    [[nodiscard]] cc::result<texture_cube_ms> try_create_texture_cube_ms(texture_cube_ms_description const& d)
    {
        return try_create_texture(d);
    }

    [[nodiscard]] texture_cube_array_ms create_texture_cube_array_ms(texture_cube_array_ms_description const& d)
    {
        return create_texture(d);
    }
    [[nodiscard]] cc::result<texture_cube_array_ms> try_create_texture_cube_array_ms(
        texture_cube_array_ms_description const& d)
    {
        return try_create_texture(d);
    }

    // bind path
public:
    /// Instantiates `layout` with the given name->view bindings as a transient binding_group, validated against the layout.
    /// Its descriptors are recycled when this epoch retires.
    /// Throws sg::binding_group_exception on a wiring error or descriptor-heap exhaustion.
    [[nodiscard]] binding_group_handle create_binding_group(binding_group_layout_handle layout,
                                                            cc::span<named_view const> views,
                                                            cc::span<named_sampler const> samplers = {});

    [[nodiscard]] cc::result<binding_group_handle> try_create_binding_group(binding_group_layout_handle layout,
                                                                            cc::span<named_view const> views,
                                                                            cc::span<named_sampler const> samplers = {});

    /// Sets the shared transient memory budget in bytes — the one heap backs all transient resources (buffers today, textures in future).
    /// May be called any time, repeatedly: it records a *pending* budget and returns immediately without touching the GPU.
    /// The change takes effect at the next advance_epoch, which drains in-flight work and resizes the transient heap; until then the current budget stays in force.
    /// Default: 128 MiB.
    void set_budget(isize size_in_bytes);

    // Pinned to its owning context: neither copyable nor movable.
    context_transient_scope(context_transient_scope const&) = delete;
    context_transient_scope(context_transient_scope&&) = delete;
    context_transient_scope& operator=(context_transient_scope const&) = delete;
    context_transient_scope& operator=(context_transient_scope&&) = delete;

private:
    friend class context;
    explicit context_transient_scope(context& ctx) : _ctx(ctx) {}

    // Applies a pending set_budget() at an epoch boundary, draining all in-flight epochs first so nothing still references the current transient heap.
    // It then drops the heap and adopts the new budget; the heap is lazily recreated at the new size on the next transient allocation.
    // No-op if nothing is pending; reached via context::apply_pending_transient_budget from a backend's advance_epoch.
    void apply_pending_budget_at_epoch_boundary();

    context& _ctx;

    // The bump allocator state: the heap (lazy), its budget, the current head, the epoch the head was last reset for, and a budget change awaiting the next epoch boundary.
    // Guarded — create_raw_buffer may run on any thread.
    struct bump_state
    {
        memory_heap_handle heap = nullptr;
        isize budget = isize(128) * 1024 * 1024;
        isize head = 0;
        u64 last_epoch = 0;       // sg::epoch value the head was last reset for (0 = never)
        isize pending_budget = 0; // a set_budget() awaiting the next epoch boundary (0 = none pending)
    };
    cc::mutex<bump_state> _bump;
};
} // namespace sg
