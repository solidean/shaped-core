#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/binding/binding_group.hh> // sg::declared_binding_group, sg::slotted_view
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/buffer.hh>               // typed buffer<T> wrapper (returned by create_buffer below)
#include <shaped-graphics/resource/texture_descriptions.hh> // shape-specific descriptions + the typed factories below
#include <shaped-graphics/types.hh>

/// Resource factory for a context's *transient* lifetime scope, reached as `ctx.transient`.
/// Transient resources are tied to the current epoch and recycled when it retires (see lifetime_scope) — per-frame scratch that never needs to outlive the work that produced it.
/// Using one past its epoch is a hard error.
///
/// Buffers are sub-allocated by a per-epoch bump allocator over one persistent memory_heap the scope owns.
/// The head resets to 0 whenever the epoch changes, so successive epochs alias the same storage — safe because a direct queue executes each epoch's GPU work before the next's.
/// Requests larger than the budget fall back to a dedicated (committed) allocation.
///
/// One spelling per create, and it throws, as on context_persistent_scope — see docs/error-handling.md.
class sg::context_transient_scope
{
    // buffers
public:
    /// Allocates a transient buffer of `size_in_bytes` from the current epoch's bump window; the storage is reused once the epoch changes.
    /// Size must be >= 0, and 0 is a valid empty buffer.
    /// Throws sg::allocation_exception on allocation failure.
    [[nodiscard]] raw_buffer_handle create_raw_buffer(isize size_in_bytes, buffer_usages usage);

    // Typed buffer factory — allocates `element_count` elements of `T`, so element_count * sizeof(T) bytes.
    // Returns the wrapped `buffer<T>`, whose view factories are typed by `T`.
    // `element_count` must be >= 0, and 0 is a valid empty buffer.
    // Error behaviour mirrors create_raw_buffer.

    template <class T>
    [[nodiscard]] buffer<T> create_buffer(isize element_count, buffer_usages usage)
    {
        return buffer<T>::from_raw(create_raw_buffer(element_count * isize(sizeof(T)), usage));
    }

    // textures
public:
    /// Allocates a transient texture, recycled once this epoch retires.
    /// Throws sg::allocation_exception on allocation failure.
    /// The transient bump-heap is buffers-only today, so a transient texture is a *dedicated* allocation auto-expired at the next epoch, not bump-suballocated.
    /// Placed transient textures wait on a texture-capable transient memory_heap.
    [[nodiscard]] raw_texture_handle create_raw_texture(texture_description const& desc);

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

    [[nodiscard]] texture_1d create_texture_1d(texture_1d_description const& d) { return create_texture(d); }

    [[nodiscard]] texture_2d create_texture_2d(texture_2d_description const& d) { return create_texture(d); }

    [[nodiscard]] texture_3d create_texture_3d(texture_3d_description const& d) { return create_texture(d); }

    [[nodiscard]] texture_cube create_texture_cube(texture_cube_description const& d) { return create_texture(d); }

    [[nodiscard]] texture_1d_array create_texture_1d_array(texture_1d_array_description const& d)
    {
        return create_texture(d);
    }

    [[nodiscard]] texture_2d_array create_texture_2d_array(texture_2d_array_description const& d)
    {
        return create_texture(d);
    }

    [[nodiscard]] texture_cube_array create_texture_cube_array(texture_cube_array_description const& d)
    {
        return create_texture(d);
    }

    [[nodiscard]] texture_2d_ms create_texture_2d_ms(texture_2d_ms_description const& d) { return create_texture(d); }

    [[nodiscard]] texture_2d_array_ms create_texture_2d_array_ms(texture_2d_array_ms_description const& d)
    {
        return create_texture(d);
    }

    [[nodiscard]] texture_cube_ms create_texture_cube_ms(texture_cube_ms_description const& d)
    {
        return create_texture(d);
    }

    [[nodiscard]] texture_cube_array_ms create_texture_cube_array_ms(texture_cube_array_ms_description const& d)
    {
        return create_texture(d);
    }

    // bind path
public:
    /// Instantiates `layout` with the given name->view bindings as a transient binding_group, validated against the layout.
    /// Its descriptors are recycled when this epoch retires.
    /// Throws sg::binding_group_exception on a wiring error or descriptor-heap exhaustion.
    [[nodiscard]] binding_group_handle create_binding_group(binding_group_layout_handle layout,
                                                            cc::span<named_view const> views,
                                                            cc::span<named_sampler const> samplers = {});

    /// The same, keyed by layout slot rather than by binding name — see sg::slotted_view.
    [[nodiscard]] binding_group_handle create_binding_group(binding_group_layout_handle layout,
                                                            cc::span<slotted_view const> views,
                                                            cc::span<named_sampler const> samplers = {});

    /// Builds a group from the generated group struct `G`, against a layout the caller already holds.
    ///
    /// Which scope you call is the lifetime: a group rebuilt every frame belongs on `ctx.transient`, one that
    /// outlives an epoch on `ctx.persistent`.
    ///
    /// **The layout is passed in rather than acquired here**, because a group is created on the frame path —
    /// once per texture switch in an imgui pass — and acquiring hashes the declared table and takes the
    /// pipeline cache's lock to look it up.
    /// Acquire it once, in init, with `ctx.cached.acquire_binding_group_layout<G>()`.
    ///
    /// A sampler `G` gathered that `layout` already declares static is dropped rather than passed on: dx12
    /// refuses a static sampler supplied per group, so sending it would be an error rather than a duplicate.
    ///
    /// Throws sg::binding_group_exception on a layout that does not match `G`, and sg::device_lost_exception
    /// on a lost device.
    template <declared_binding_group G>
    [[nodiscard]] binding_group_handle create_binding_group(binding_group_layout_handle const& layout, G const& group)
    {
        cc::vector<slotted_view> views;
        cc::vector<named_sampler> samplers;
        group.gather(views, samplers);
        impl::drop_static_samplers(*layout, samplers);
        return create_binding_group(layout, views, samplers);
    }

    /// Sets the shared transient memory budget in bytes — the one heap backs all transient resources (buffers today, textures in future).
    /// May be called any time, repeatedly: it records a *pending* budget and returns immediately without touching the GPU.
    /// The change takes effect at the next advance_epoch, which drops the current heap and allocates the next one lazily at the new size; nothing waits.
    ///
    /// **Both heaps can be alive at once.**
    /// The old one lives until the last transient resource placed in it retires, so for one or two epochs the budget is paid twice.
    /// A caller for whom that overlap matters lets the in-flight epochs retire first — `epochs_in_flight_completion(0)` — and sets the budget after.
    /// Starts at default_budget_bytes.
    void set_budget(isize size_in_bytes);

    /// The transient budget a context starts with.
    static constexpr isize default_budget_bytes = isize(128) * 1024 * 1024;

    // Pinned to its owning context: neither copyable nor movable.
    context_transient_scope(context_transient_scope const&) = delete;
    context_transient_scope(context_transient_scope&&) = delete;
    context_transient_scope& operator=(context_transient_scope const&) = delete;
    context_transient_scope& operator=(context_transient_scope&&) = delete;

private:
    // The fallible cores the throwing creates above are built on.
    //
    // Not public: an exhaustion failure is not something a caller can act on, and offering the choice
    // implied that it was — see docs/error-handling.md.

    [[nodiscard]] cc::result<raw_buffer_handle> try_create_raw_buffer(isize size_in_bytes, buffer_usages usage);

    [[nodiscard]] cc::result<raw_texture_handle> try_create_raw_texture(texture_description const& desc);

    [[nodiscard]] cc::result<binding_group_handle> try_create_binding_group(binding_group_layout_handle layout,
                                                                            cc::span<named_view const> views,
                                                                            cc::span<named_sampler const> samplers = {});

    [[nodiscard]] cc::result<binding_group_handle> try_create_binding_group(binding_group_layout_handle layout,
                                                                            cc::span<slotted_view const> views,
                                                                            cc::span<named_sampler const> samplers = {});

    friend class context;
    explicit context_transient_scope(context& ctx) : _ctx(ctx) {}

    // Applies a pending set_budget() at an epoch boundary: drops the heap and adopts the new budget, without waiting.
    // The heap is lazily recreated at the new size on the next transient allocation.
    // No-op if nothing is pending; reached via context::apply_pending_transient_budget from a backend's advance_epoch.
    void apply_pending_budget_at_epoch_boundary();

    // Drops the transient heap, called from context::shutdown before a backend tears its device down.
    //
    // The heap is GPU memory allocated from the device, so it must not outlive it — the same rule routines.clear()
    // above is there for.
    // dx12 happens to survive without this because its heap holds a device reference of its own.
    // A backend whose device is not reference counted, such as vulkan, leaks the allocation instead, and its
    // validation layer reports it at vkDestroyDevice.
    void release_heap_at_shutdown();

    context& _ctx;

    // The bump allocator state: the heap (lazy), its budget, the current head, the epoch the head was last reset for, and a budget change awaiting the next epoch boundary.
    // Guarded — create_raw_buffer may run on any thread.
    struct bump_state
    {
        memory_heap_handle heap = nullptr;
        isize budget = default_budget_bytes;
        isize head = 0;
        u64 last_epoch = 0;       // sg::epoch value the head was last reset for (0 = never)
        isize pending_budget = 0; // a set_budget() awaiting the next epoch boundary (0 = none pending)
    };
    cc::mutex<bump_state> _bump;
};
