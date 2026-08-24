#pragma once

#include <clean-core/string/string_view.hh>
#include <shaped-graphics/binding/impl/slot_table.hh>
#include <shaped-graphics/fwd.hh> // sg::binding_slot, sg::epoch
#include <shaped-graphics/resource/views.hh>

/// A bindless view over ONE array binding of a staging_binding_group: the key → element-index map that turns
/// a view into the index a shader uses into that array.
/// It shares the group's handle, so the group cannot go out from under it — but it owns no descriptor of its
/// own and nothing here mints, binds or snapshots; the context lives outside and must outlive it.
/// Several arrays over different bindings of one group are independent of each other.
///
/// `acquire` returns the element index for a view, minting it on a miss.
/// An index is only valid for the epoch it was acquired in: re-acquire every view each epoch.
/// Re-acquiring the same view is O(1), returns the same index, and touches no descriptor, so an unchanged
/// working set never causes a reupload; a mint writes exactly one staging descriptor.
/// When the array is full, every index not acquired this epoch is reclaimed at once; if every index was
/// acquired this epoch, the working set exceeds the binding's count and acquire asserts.
///
/// Guarding the window between a mint and the snapshot that must contain it is NOT this class's job.
/// One array cannot enforce it, because the invariant spans every array over one staging group: the owner is
/// whoever holds the group and all its arrays, and in shaped-viewer that is `sv::gpu_resource_manager`.
/// Taking the snapshot is that owner's too — `group->snapshot()`, bind it, and refuse acquires while it is the
/// bound one.
///
/// TODO: `acquire` should split into a transient handle (a typed enum, this epoch only) and a persistent one
/// (refcounted, frees its slot), with eager eviction the default so a stale index fails immediately.
/// Until it does, an index written into GPU memory that outlives its epoch has nothing protecting it — the
/// reclaim rule above only covers indices re-acquired every epoch.
///
/// Access declaration stays the consumer's job: whoever binds the group declares the elements its dispatch
/// reads via declare_array_*_access.
/// Writable views are never bindless; they stay ordinary bindings.
/// Not thread-safe, like the staging group it writes to.
class sg::bindless_array
{
public:
    /// A bindless view over `group`'s array binding named `name`; the handle is kept, and `ctx` must outlive it.
    /// `group` must be non-null, and the binding must exist and be an array (count > 1 — a count of 1 is a
    /// scalar binding to sg and loses the vacant-element semantics); all three are asserted.
    /// Clears the array, so the empty table and the descriptors agree — which also counts as having said what
    /// the binding holds, satisfying the group's "every binding set before the first snapshot" rule.
    [[nodiscard]] static bindless_array for_binding(context& ctx, staging_binding_group_handle group, cc::string_view name);

    /// The binding this array writes, resolved once at construction.
    [[nodiscard]] binding_slot slot() const { return _slot; }

    /// How many elements the binding holds — the layout's count, and the ceiling on the working set.
    [[nodiscard]] u32 capacity() const { return u32(_table.capacity()); }

    [[nodiscard]] u32 occupied_count() const { return u32(_table.occupied_count()); }

    /// The element index for `view`, minted or re-used (see the class doc for index lifetime).
    /// The view must satisfy the binding — the staging group validates it.
    [[nodiscard]] u32 acquire(raw_view const& view);

    /// Movable, so an owner can keep one array per table in a container.
    /// Never copied: two arrays over one binding would mint conflicting descriptors from two tables.
    bindless_array(bindless_array&&) noexcept = default;
    bindless_array& operator=(bindless_array&&) noexcept = default;
    bindless_array(bindless_array const&) = delete;
    bindless_array& operator=(bindless_array const&) = delete;

private:
    bindless_array(context& ctx, staging_binding_group_handle group, binding_slot slot, u32 capacity);

    context* _ctx = nullptr; // a reference would cost the move assignment; never null after construction
    staging_binding_group_handle _group;
    binding_slot _slot = binding_slot::invalid;
    impl::slot_table<raw_view> _table;
};
