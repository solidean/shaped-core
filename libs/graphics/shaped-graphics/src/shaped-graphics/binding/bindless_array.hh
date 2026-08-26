#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/binding/impl/bindless_array_state.hh>
#include <shaped-graphics/fwd.hh> // sg::binding_slot, sg::epoch
#include <shaped-graphics/resource/views.hh>

/// The transient half of a bindless array, reached as `array.transient`.
class sg::bindless_array_transient_scope
{
public:
    /// The element index for `view` this epoch, minted or re-used (see bindless_array for index lifetime).
    /// The view must satisfy the binding — the staging group validates it.
    /// Never store the result where it outlives the epoch; that is what `persistent` is for.
    [[nodiscard]] bindless_index acquire(raw_view const& view) { return bindless_index(_state.acquire(view)); }

    // Pinned to its array, which rebinds it on move.
    bindless_array_transient_scope(bindless_array_transient_scope const&) = delete;
    bindless_array_transient_scope(bindless_array_transient_scope&&) = delete;
    bindless_array_transient_scope& operator=(bindless_array_transient_scope const&) = delete;
    bindless_array_transient_scope& operator=(bindless_array_transient_scope&&) = delete;

private:
    friend class bindless_array;
    explicit bindless_array_transient_scope(impl::bindless_array_state& state) : _state(state) {}

    impl::bindless_array_state& _state;
};

/// The persistent half of a bindless array, reached as `array.persistent`.
class sg::bindless_array_persistent_scope
{
public:
    /// A shared hold on `view`'s element, minted or re-used, whose index stays true until the last handle dies.
    /// Acquiring a view that is already held returns the *same* handle, so one element is never pinned twice.
    [[nodiscard]] bindless_element_handle acquire(raw_view const& view);

    // Pinned to its array, which rebinds it on move.
    bindless_array_persistent_scope(bindless_array_persistent_scope const&) = delete;
    bindless_array_persistent_scope(bindless_array_persistent_scope&&) = delete;
    bindless_array_persistent_scope& operator=(bindless_array_persistent_scope const&) = delete;
    bindless_array_persistent_scope& operator=(bindless_array_persistent_scope&&) = delete;

private:
    friend class bindless_array;
    explicit bindless_array_persistent_scope(impl::bindless_array_state& state) : _state(state) {}

    impl::bindless_array_state& _state;
};

/// A bindless view over ONE array binding of a staging_binding_group: the key → element-index map that turns
/// a view into the index a shader uses into that array.
/// Exactly one array may exist over a given binding, and it is move-constructible only — see below.
/// It shares the group's handle, so the group cannot go out from under it — but it owns no descriptor of its
/// own and nothing here binds or snapshots; the context lives outside and must outlive it.
/// Several arrays over different bindings of one group are independent of each other.
///
/// There are two ways in, and which one you need is decided by **how long the index has to stay true**.
/// They are reached through scopes, as elsewhere in sg: `array.transient.acquire(view)` and
/// `array.persistent.acquire(view)`.
///
/// **`transient.acquire`** returns a `bindless_index` valid only for the epoch it was acquired in, so
/// re-acquire every view each epoch.
/// Re-acquiring the same view is O(1), returns the same index, and touches no descriptor, so an unchanged
/// working set never causes a reupload; a mint writes exactly one staging descriptor.
/// When the array is full, every index not acquired this epoch is reclaimed at once; if every index was
/// acquired this epoch, the working set exceeds the binding's count and acquire asserts.
///
/// **`persistent.acquire`** returns a shared `bindless_element_handle` that keeps its element alive for as long
/// as any copy of the handle does.
/// That index — and only that index — may be written into GPU memory outliving the epoch, a material buffer
/// above all.
/// Releasing the last handle frees the slot and clears its descriptor, so an index read afterwards resolves to
/// a vacant descriptor rather than to whatever moved in.
/// Pins raise the floor the transient working set has to fit above: capacity must cover both.
///
/// The two results are deliberately different types rather than both being `u32`.
/// Storing a transient index somewhere persistent is the one mistake this class cannot catch at runtime, so it
/// is made not to compile.
///
/// Guarding the window between a mint and the snapshot that must contain it is NOT this class's job.
/// One array cannot enforce it, because the invariant spans every array over one staging group: the owner is
/// whoever holds the group and all its arrays, and in shaped-viewer that is `sv::gpu_resource_manager`.
/// Taking the snapshot is that owner's too — `group->snapshot()`, bind it, and refuse acquires while it is the
/// bound one.
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

    /// Indices valid for this epoch only: `array.transient.acquire(view)`.
    bindless_array_transient_scope transient;

    /// Shared holds whose index outlives the epoch: `array.persistent.acquire(view)`.
    bindless_array_persistent_scope persistent;

    /// The binding this array writes, resolved once at construction.
    [[nodiscard]] binding_slot slot() const { return _state->slot; }

    /// How many elements the binding holds — the layout's count, and the ceiling on the working set.
    [[nodiscard]] u32 capacity() const { return u32(_state->table.capacity()); }

    [[nodiscard]] u32 occupied_count() const { return u32(_state->table.occupied_count()); }

    /// How many elements are pinned by a live handle — the floor a transient working set fits above.
    [[nodiscard]] u32 pinned_count() const { return u32(_state->table.pinned_count()); }

    /// **Move-constructible only.**
    /// `transient` and `persistent` hold a reference to the state they are reached through, so an assignment would have to
    /// rebind them and cannot — an assigned-to array would keep writing into the state it used to name.
    /// Construction is all `for_binding` returning by value and a `cc::vector` growing need, so nothing is lost, and an
    /// assignment now fails to compile at the call site rather than dangling at run time.
    /// There must be at most one array per binding regardless: the invariant spans every array over one staging group, so
    /// whoever owns the group owns them all.
    bindless_array(bindless_array&& rhs) noexcept
      : transient(*rhs._state), persistent(*rhs._state), _state(cc::move(rhs._state))
    {
    }
    bindless_array(bindless_array const&) = delete;
    bindless_array& operator=(bindless_array const&) = delete;
    bindless_array& operator=(bindless_array&&) = delete;
    ~bindless_array() = default;

private:
    explicit bindless_array(std::shared_ptr<impl::bindless_array_state> state)
      : transient(*state), persistent(*state), _state(cc::move(state))
    {
    }

    // Declared after the scopes, which are therefore constructed from the incoming state rather than from this one.
    std::shared_ptr<impl::bindless_array_state> _state;
};

/// One pinned element of a bindless array — what `array.persistent.acquire` hands out, always behind a
/// `bindless_element_handle`.
///
/// Its `index()` stays true through any number of epochs and reclaims, which is what makes it the one index
/// safe to write into GPU memory outliving an epoch.
/// It keeps the array's state alive, so releasing it is safe even after every `bindless_array` naming that
/// binding is gone.
class sg::bindless_element
{
public:
    ~bindless_element();

    /// The element index this handle holds.
    [[nodiscard]] u32 index() const { return _index; }

    /// The binding it indexes into, for a caller holding elements of several arrays.
    [[nodiscard]] binding_slot slot() const { return _state->slot; }

    bindless_element(bindless_element const&) = delete;
    bindless_element& operator=(bindless_element const&) = delete;

private:
    friend class bindless_array_persistent_scope;
    bindless_element(std::shared_ptr<impl::bindless_array_state> state, u32 index)
      : _state(cc::move(state)), _index(index)
    {
    }

    std::shared_ptr<impl::bindless_array_state> _state;
    u32 _index = 0;
};
