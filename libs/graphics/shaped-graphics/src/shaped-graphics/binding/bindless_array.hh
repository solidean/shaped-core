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
/// `lock()` refuses acquires until `unlock()`, which must come in the same epoch (both asserted) — it guards
/// the window in which a snapshot is bound, and mints nothing.
/// Taking the snapshot stays the group owner's job: lock every array over the group, call `group->snapshot()`,
/// bind it, unlock.
/// `lock_scoped()` is the RAII form.
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
    /// The view must satisfy the binding — the staging group validates it — and the array must not be locked.
    [[nodiscard]] u32 acquire(raw_view const& view);

    /// Refuses acquires until `unlock`, which must come in the same epoch.
    void lock();
    void unlock();

    /// The RAII form of the pair above: locks now, unlocks when the returned lock leaves scope.
    /// The lock must be destroyed in the epoch it was taken in — the same-epoch rule, made structural.
    [[nodiscard]] bindless_lock lock_scoped();

    [[nodiscard]] bool is_locked() const { return _locked; }

private:
    bindless_array(context& ctx, staging_binding_group_handle group, binding_slot slot, u32 capacity);

    context& _ctx;
    staging_binding_group_handle _group;
    binding_slot _slot = binding_slot::invalid;
    impl::slot_table _table;

    bool _locked = false;
    epoch _lock_epoch = epoch::invalid;
};

/// Holds a bindless_array's lock for its lifetime — `lock_scoped()`'s return value.
/// Unlocks on destruction, so lock and unlock cannot come apart; the same-epoch rule becomes "do not carry
/// this across an epoch advance".
/// Move-only: the moved-from lock is disarmed and unlocks nothing.
class sg::bindless_lock
{
public:
    ~bindless_lock()
    {
        if (_array != nullptr)
            _array->unlock();
    }

    bindless_lock(bindless_lock&& rhs) noexcept : _array(rhs._array) { rhs._array = nullptr; }

    bindless_lock(bindless_lock const&) = delete;
    bindless_lock& operator=(bindless_lock const&) = delete;
    bindless_lock& operator=(bindless_lock&&) = delete;

private:
    // Only the array mints one, through lock_scoped().
    friend class bindless_array;
    explicit bindless_lock(bindless_array& array) : _array(&array) {}

    bindless_array* _array = nullptr; // null = disarmed (moved-from)
};

inline sg::bindless_lock sg::bindless_array::lock_scoped()
{
    lock();
    return bindless_lock(*this);
}
