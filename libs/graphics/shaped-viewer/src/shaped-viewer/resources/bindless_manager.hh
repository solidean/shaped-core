#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/views.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/resources/impl/slot_table.hh>

namespace sv
{
// The bindless group's binding names, one per category — also what a consumer passes to
// cmd.*.declare_array_*_access when it dispatches against the group.
inline constexpr char const* bindless_buffers_binding = "BindlessBuffers";
inline constexpr char const* bindless_textures_1d_binding = "BindlessTex1D";
inline constexpr char const* bindless_textures_2d_binding = "BindlessTex2D";
inline constexpr char const* bindless_textures_3d_binding = "BindlessTex3D";
inline constexpr char const* bindless_textures_cube_binding = "BindlessTexCube";
} // namespace sv

/// Per-category capacities of the bindless descriptor group.
///
/// Each count is a binding array's length — the table's capacity, not a growth hint — and must be >= 2,
/// since a count of 1 is a scalar binding to sg and loses the vacant-element semantics.
struct sv::bindless_config
{
    u32 buffer_count = 256;
    u32 texture_1d_count = 16;
    u32 texture_2d_count = 256;
    u32 texture_3d_count = 16;
    u32 texture_cube_count = 16;
};

/// Owns ONE bindless descriptor group: five readonly binding arrays — buffers, texture1d/2d/3d/cube — each
/// backed by a dirty-flagged CPU mirror (see impl/slot_table.hh).
/// Writable views are never bindless; they stay ordinary bindings in another group.
///
/// `acquire` returns a category-typed slot — the index a shader uses into that binding array.
/// A slot is only valid for the epoch it was acquired in: re-acquire every view each epoch.
/// Re-acquiring the same view is O(1), returns the same slot, and leaves the mirror clean, so an unchanged
/// working set never causes a reupload.
///
/// `lock_group()` hands out the group and locks the manager — no acquires until `unlock_group(group)`,
/// which must receive the same group back in the same epoch (both asserted; the handle is shared, so
/// identity means pointer equality, not ownership transfer).
/// The group is recreated only when a mirror changed since the last lock (sg groups are immutable —
/// recreate is the only rebind); otherwise the same handle is served again.
///
/// Access declaration stays the consumer's job: whoever binds the group declares the elements its dispatch
/// reads via declare_array_*_access, using the binding names above.
///
/// The layout puts each category in its own register space, space1..space5 in the order above, at index 0.
/// Not thread-safe — owned by whoever runs the frame, like the other managers.
class sv::bindless_manager
{
public:
    /// A manager on `ctx` (which must outlive it), sized by `cfg` (each count >= 2, asserted).
    /// The layout and group are created lazily, so construction is safe where the backend cannot build them.
    [[nodiscard]] static bindless_manager create(sg::context& ctx, bindless_config const& cfg = {});

    /// The slot for this buffer view, minted or re-used (see the class doc for slot lifetime).
    /// The view must be readonly; the manager must not be locked.
    [[nodiscard]] bindless_buffer_slot acquire(sg::raw_buffer_view const& view);

    /// The slot for this texture view, minted or re-used; one overload per bindless dimension.
    /// The manager must not be locked.
    [[nodiscard]] bindless_texture_1d_slot acquire(sg::readonly_texture_view<sg::tv_1d> const& view);
    [[nodiscard]] bindless_texture_2d_slot acquire(sg::readonly_texture_view<sg::tv_2d> const& view);
    [[nodiscard]] bindless_texture_3d_slot acquire(sg::readonly_texture_view<sg::tv_3d> const& view);
    [[nodiscard]] bindless_texture_cube_slot acquire(sg::readonly_texture_view<sg::tv_cube> const& view);

    /// The group's binding-group layout, created on first use — for building the pipeline layout.
    [[nodiscard]] sg::binding_group_layout_handle const& layout();

    /// The bindless group, recreated first if any mirror changed since the last lock; locks the manager.
    /// Slots acquired this epoch index the returned group.
    [[nodiscard]] sg::binding_group_handle lock_group();

    /// Unlocks; `group` must be the handle `lock_group` returned, in the same epoch (both asserted).
    void unlock_group(sg::binding_group_handle const& group);

    [[nodiscard]] bool is_locked() const { return _locked; }

private:
    bindless_manager(sg::context& ctx, bindless_config const& cfg);

    void _ensure_layout();

    sg::context& _ctx;
    bindless_config _cfg;
    sg::binding_group_layout_handle _layout;
    impl::slot_table _buffers;
    impl::slot_table _tex_1d;
    impl::slot_table _tex_2d;
    impl::slot_table _tex_3d;
    impl::slot_table _tex_cube;

    /// The served group; overwritten on recreate — the old range is freed by sg's epoch finalizer.
    sg::binding_group_handle _group;
    bool _locked = false;
    sg::epoch _lock_epoch = sg::epoch::invalid;
};
