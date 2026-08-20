#pragma once

#include <clean-core/string/string.hh>
#include <shaped-graphics/fwd.hh> // sg::binding_slot + the handle typedefs
#include <shaped-graphics/resource/views.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/resources/impl/slot_table.hh>

/// The bindless descriptor group's shape: per-category capacities, and the binding names.
///
/// Each count is a binding array's length — the table's capacity, not a growth hint — and must be >= 2,
/// since a count of 1 is a scalar binding to sg and loses the vacant-element semantics.
/// The names are what the shader declares and what a consumer passes to cmd.*.declare_array_*_access;
/// read them back through `bindless_manager::config()`.
struct sv::bindless_config
{
    u32 buffer_count = 4096;
    u32 texture_1d_count = 256;
    u32 texture_2d_count = 4096;
    u32 texture_3d_count = 256;
    u32 texture_cube_count = 256;

    cc::string buffers_binding = "BindlessBuffers";
    cc::string textures_1d_binding = "BindlessTex1D";
    cc::string textures_2d_binding = "BindlessTex2D";
    cc::string textures_3d_binding = "BindlessTex3D";
    cc::string textures_cube_binding = "BindlessTexCube";
};

/// Owns ONE bindless descriptor group: five readonly binding arrays — buffers, texture1d/2d/3d/cube — kept
/// in an sg::staging_binding_group, with one impl::slot_table per array as the key → element identity map.
/// Writable views are never bindless; they stay ordinary bindings in another group.
///
/// `acquire` returns a category-typed slot — the index a shader uses into that binding array.
/// A slot is only valid for the epoch it was acquired in: re-acquire every view each epoch.
/// Re-acquiring the same view is O(1), returns the same slot, and touches no descriptor, so an unchanged
/// working set never causes a reupload; a mint writes exactly one staging descriptor.
///
/// `lock()` hands out the group and locks the manager — no acquires until `unlock(group)`,
/// which must receive the same group back in the same epoch (both asserted; the handle is shared, so
/// identity means pointer equality, not ownership transfer).
/// `lock_scoped()` is the RAII form: the returned `bindless_lock` carries the handle and
/// unlocks at scope exit.
/// The group is the staging group's snapshot: minted only when a descriptor changed since the last lock,
/// otherwise the same handle is served again.
///
/// Access declaration stays the consumer's job: whoever binds the group declares the elements its dispatch
/// reads via declare_array_*_access, using the config's binding names.
///
/// The layout puts each category in its own register space, space1..space5 in the order above, at index 0.
/// Not thread-safe — owned by whoever runs the frame, like the other managers.
class sv::bindless_manager
{
public:
    /// A manager on `ctx` (which must outlive it), shaped by `cfg` (each count >= 2, asserted).
    /// The layout and staging group are created lazily on first use, so construction is safe — but a first
    /// acquire is not — where the backend cannot build them.
    [[nodiscard]] static bindless_manager create(sg::context& ctx, bindless_config const& cfg = {});

    /// The group's shape — capacities and the binding names a consumer declares access against.
    [[nodiscard]] bindless_config const& config() const { return _cfg; }

    /// The slot for this buffer view, minted or re-used (see the class doc for slot lifetime).
    /// The manager must not be locked.
    [[nodiscard]] bindless_buffer_slot acquire(sg::readonly_buffer_view<byte> const& view);

    /// The slot for this texture view, minted or re-used; one overload per bindless dimension.
    /// The manager must not be locked.
    [[nodiscard]] bindless_texture_1d_slot acquire(sg::readonly_texture_view<sg::tv_1d> const& view);
    [[nodiscard]] bindless_texture_2d_slot acquire(sg::readonly_texture_view<sg::tv_2d> const& view);
    [[nodiscard]] bindless_texture_3d_slot acquire(sg::readonly_texture_view<sg::tv_3d> const& view);
    [[nodiscard]] bindless_texture_cube_slot acquire(sg::readonly_texture_view<sg::tv_cube> const& view);

    /// The group's binding-group layout, created on first use — for building the pipeline layout.
    [[nodiscard]] sg::binding_group_layout_handle const& layout();

    /// The bindless group — the staging group's snapshot, minted only if a descriptor changed since the
    /// last lock; locks the manager.
    /// Slots acquired this epoch index the returned group.
    [[nodiscard]] sg::binding_group_handle lock();

    /// Unlocks; `group` must be the handle `lock` returned, in the same epoch (both asserted).
    void unlock(sg::binding_group_handle const& group);

    /// The RAII form of the pair above: locks now, unlocks when the returned lock leaves scope.
    /// The lock must be destroyed in the epoch it was taken in — the same-epoch rule, made structural.
    [[nodiscard]] bindless_lock lock_scoped();

    [[nodiscard]] bool is_locked() const { return _locked; }

private:
    bindless_manager(sg::context& ctx, bindless_config const& cfg);

    /// Creates the layout + staging group and resolves the five binding slots on first use.
    void _ensure_staging();

    /// The shared acquire: resolve the key in `table`, mirror mints and reclaims onto the staging group.
    [[nodiscard]] u32 _acquire(impl::slot_table& table, sg::binding_slot slot, sg::raw_view const& view);

    sg::context& _ctx;
    bindless_config _cfg;
    sg::binding_group_layout_handle _layout;

    /// The descriptor image behind the group — it owns the descriptors and keeps their resources alive.
    sg::staging_binding_group_handle _staging;
    sg::binding_slot _buffers_slot = sg::binding_slot::invalid;
    sg::binding_slot _tex_1d_slot = sg::binding_slot::invalid;
    sg::binding_slot _tex_2d_slot = sg::binding_slot::invalid;
    sg::binding_slot _tex_3d_slot = sg::binding_slot::invalid;
    sg::binding_slot _tex_cube_slot = sg::binding_slot::invalid;

    impl::slot_table _buffers;
    impl::slot_table _tex_1d;
    impl::slot_table _tex_2d;
    impl::slot_table _tex_3d;
    impl::slot_table _tex_cube;

    /// The snapshot lock served, for unlock's identity check.
    sg::binding_group_handle _group;
    bool _locked = false;
    sg::epoch _lock_epoch = sg::epoch::invalid;
};

/// Holds the bindless manager's lock for its lifetime — `lock_scoped()`'s return value.
/// Carries the group handle the lock covers, and unlocks on destruction, so lock and unlock cannot come
/// apart; the same-epoch rule becomes "do not carry this across an epoch advance".
/// Move-only: the moved-from lock is disarmed and unlocks nothing.
class sv::bindless_lock
{
public:
    /// The locked group — what the dispatch binds.
    /// A copy, deliberately: the handle is shared, and a copy stays valid after the lock is gone — a
    /// reference into the lock would dangle at scope exit.
    [[nodiscard]] sg::binding_group_handle group() const { return _group; }

    ~bindless_lock()
    {
        if (_manager != nullptr)
            _manager->unlock(_group);
    }

    bindless_lock(bindless_lock&& rhs) noexcept : _manager(rhs._manager), _group(cc::move(rhs._group))
    {
        rhs._manager = nullptr;
    }

    bindless_lock(bindless_lock const&) = delete;
    bindless_lock& operator=(bindless_lock const&) = delete;
    bindless_lock& operator=(bindless_lock&&) = delete;

private:
    // Only the manager mints one, through lock_scoped().
    friend class bindless_manager;
    bindless_lock(bindless_manager& manager, sg::binding_group_handle group)
      : _manager(&manager), _group(cc::move(group))
    {
    }

    bindless_manager* _manager = nullptr; // null = disarmed (moved-from)
    sg::binding_group_handle _group;
};

inline sv::bindless_lock sv::bindless_manager::lock_scoped()
{
    return bindless_lock(*this, lock());
}
