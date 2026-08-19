#include "bindless_manager.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/hash.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/context/context.hh>

namespace sv
{
namespace
{
// View-identity keys: every field that reaches the descriptor participates, so two views differing in any of them mint different slots.
// The resource pointer is safe as identity because the mapped entry holds the handle alive — the address cannot be reused while the key is in the table.
[[nodiscard]] u64 key_of(sg::raw_buffer_view const& v)
{
    return cc::make_hash(v.buffer.get(), v.access, v.shape, v.offset_in_bytes, v.size_in_bytes, v.element_count,
                         v.stride_in_bytes);
}

template <class Traits>
[[nodiscard]] u64 key_of(sg::readonly_texture_view<Traits> const& v)
{
    return cc::make_hash(v.texture.get(), Traits::dimension, v.format, v.range.mip_range.start, v.range.mip_range.end,
                         v.range.array_range.start, v.range.array_range.end, v.range.aspect_range.start,
                         v.range.aspect_range.end);
}

// A vacant texture element of the category's dimension: null handle, but dimension + format still shape the
// null descriptor the backend builds for it.
[[nodiscard]] sg::raw_texture_view vacant_texture(sg::texture_view_dimension dim)
{
    return {.access = sg::view_class::readonly,
            .texture = nullptr,
            .view_dimension = dim,
            .format = sg::pixel_format::rgba8_unorm};
}

[[nodiscard]] sg::raw_buffer_view vacant_buffer()
{
    return {.access = sg::view_class::readonly, .shape = sg::view_shape::raw, .buffer = nullptr};
}

// The full element list of one category: each occupied slot's view, a vacant view everywhere else.
[[nodiscard]] sg::named_view mirror_to_named_view(char const* name, sv::impl::slot_table const& table, sg::raw_view vacant)
{
    auto nv = sg::named_view{.name = name, .views = {}};
    for (auto const& e : table.entries())
        nv.views.push_back(e.occupied ? e.view : vacant);
    return nv;
}
} // namespace
} // namespace sv

sv::bindless_manager sv::bindless_manager::create(sg::context& ctx, bindless_config const& cfg)
{
    CC_ASSERT(cfg.buffer_count >= 2 && cfg.texture_1d_count >= 2 && cfg.texture_2d_count >= 2
                  && cfg.texture_3d_count >= 2 && cfg.texture_cube_count >= 2,
              "bindless counts must be >= 2 (a count of 1 is a scalar binding to sg and loses vacant elements)");
    return bindless_manager(ctx, cfg);
}

sv::bindless_manager::bindless_manager(sg::context& ctx, bindless_config const& cfg)
  : _ctx(ctx),
    _cfg(cfg),
    _buffers(cfg.buffer_count),
    _tex_1d(cfg.texture_1d_count),
    _tex_2d(cfg.texture_2d_count),
    _tex_3d(cfg.texture_3d_count),
    _tex_cube(cfg.texture_cube_count)
{
}

sv::bindless_buffer_slot sv::bindless_manager::acquire(sg::raw_buffer_view const& view)
{
    CC_ASSERT(!_locked, "no acquires while the bindless group is locked (unlock_group first)");
    CC_ASSERT(view.access == sg::view_class::readonly, "bindless views are readonly; writable views belong in "
                                                       "another group");
    return bindless_buffer_slot(_buffers.acquire(key_of(view), view, _ctx.current_epoch()));
}

sv::bindless_texture_1d_slot sv::bindless_manager::acquire(sg::readonly_texture_view<sg::tv_1d> const& view)
{
    CC_ASSERT(!_locked, "no acquires while the bindless group is locked (unlock_group first)");
    return bindless_texture_1d_slot(_tex_1d.acquire(key_of(view), view, _ctx.current_epoch()));
}

sv::bindless_texture_2d_slot sv::bindless_manager::acquire(sg::readonly_texture_view<sg::tv_2d> const& view)
{
    CC_ASSERT(!_locked, "no acquires while the bindless group is locked (unlock_group first)");
    return bindless_texture_2d_slot(_tex_2d.acquire(key_of(view), view, _ctx.current_epoch()));
}

sv::bindless_texture_3d_slot sv::bindless_manager::acquire(sg::readonly_texture_view<sg::tv_3d> const& view)
{
    CC_ASSERT(!_locked, "no acquires while the bindless group is locked (unlock_group first)");
    return bindless_texture_3d_slot(_tex_3d.acquire(key_of(view), view, _ctx.current_epoch()));
}

sv::bindless_texture_cube_slot sv::bindless_manager::acquire(sg::readonly_texture_view<sg::tv_cube> const& view)
{
    CC_ASSERT(!_locked, "no acquires while the bindless group is locked (unlock_group first)");
    return bindless_texture_cube_slot(_tex_cube.acquire(key_of(view), view, _ctx.current_epoch()));
}

sg::binding_group_layout_handle const& sv::bindless_manager::layout()
{
    _ensure_layout();
    return _layout;
}

void sv::bindless_manager::_ensure_layout()
{
    if (_layout != nullptr)
        return;

    // One register space per category (index 0 each), so shaders address a category with no register-offset
    // math: `ByteAddressBuffer BindlessBuffers[N] : register(t0, space1);` and so on.
    auto const tex_binding = [](char const* name, u32 set, u32 count)
    {
        return sg::binding{.name = name, .set = set, .index = 0, .count = count, .type = sg::binding_type::readonly_texture};
    };
    sg::binding const bindings[] = {
        {.name = bindless_buffers_binding,
         .set = 1,
         .index = 0,
         .count = _cfg.buffer_count,
         .type = sg::binding_type::readonly_raw_buffer},
        tex_binding(bindless_textures_1d_binding, 2, _cfg.texture_1d_count),
        tex_binding(bindless_textures_2d_binding, 3, _cfg.texture_2d_count),
        tex_binding(bindless_textures_3d_binding, 4, _cfg.texture_3d_count),
        tex_binding(bindless_textures_cube_binding, 5, _cfg.texture_cube_count),
    };
    _layout = _ctx.cached.acquire_binding_group_layout(bindings);
}

sg::binding_group_handle sv::bindless_manager::lock_group()
{
    CC_ASSERT(!_locked, "the bindless group is already locked");
    _ensure_layout();

    auto const dirty = _buffers.dirty() || _tex_1d.dirty() || _tex_2d.dirty() || _tex_3d.dirty() || _tex_cube.dirty();
    if (dirty || _group == nullptr)
    {
        using vd = sg::texture_view_dimension;
        cc::vector<sg::named_view> views;
        views.push_back(mirror_to_named_view(bindless_buffers_binding, _buffers, vacant_buffer()));
        views.push_back(mirror_to_named_view(bindless_textures_1d_binding, _tex_1d, vacant_texture(vd::tex_1d)));
        views.push_back(mirror_to_named_view(bindless_textures_2d_binding, _tex_2d, vacant_texture(vd::tex_2d)));
        views.push_back(mirror_to_named_view(bindless_textures_3d_binding, _tex_3d, vacant_texture(vd::tex_3d)));
        views.push_back(mirror_to_named_view(bindless_textures_cube_binding, _tex_cube, vacant_texture(vd::cube)));

        // Overwrite, not patch: sg groups are immutable, and the old group's descriptor range is freed by
        // an epoch finalizer once its last-using epoch retires.
        _group = _ctx.persistent.create_binding_group(_layout, views);
        _buffers.clear_dirty();
        _tex_1d.clear_dirty();
        _tex_2d.clear_dirty();
        _tex_3d.clear_dirty();
        _tex_cube.clear_dirty();
    }

    _locked = true;
    _lock_epoch = _ctx.current_epoch();
    return _group;
}

void sv::bindless_manager::unlock_group(sg::binding_group_handle const& group)
{
    CC_ASSERT(_locked, "unlock_group without a lock_group");
    CC_ASSERT(group.get() == _group.get(), "unlock_group must receive the group lock_group returned");
    CC_ASSERT(_ctx.current_epoch() == _lock_epoch, "lock_group and unlock_group must happen in the same epoch");
    _locked = false;
}
