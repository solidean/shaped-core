#include "bindless_manager.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/hash.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/context/context.hh>

using namespace cc::primitive_defines;

namespace sv
{
namespace
{
// The slot key is the view's identity hash: resources participate by address, and the mapped entry holds
// the handle alive, so a key's pointer cannot be reused by a new resource while the entry lives.
[[nodiscard]] u64 key_of(sg::raw_view const& v)
{
    return cc::make_hash(v);
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

sv::bindless_buffer_slot sv::bindless_manager::acquire(sg::readonly_buffer_view<byte> const& view)
{
    _ensure_staging(); // before _buffers_slot is read — the first acquire is what resolves it
    return bindless_buffer_slot(_acquire(_buffers, _buffers_slot, view));
}

sv::bindless_texture_1d_slot sv::bindless_manager::acquire(sg::readonly_texture_view<sg::tv_1d> const& view)
{
    _ensure_staging(); // before _tex_1d_slot is read — the first acquire is what resolves it
    return bindless_texture_1d_slot(_acquire(_tex_1d, _tex_1d_slot, view));
}

sv::bindless_texture_2d_slot sv::bindless_manager::acquire(sg::readonly_texture_view<sg::tv_2d> const& view)
{
    _ensure_staging(); // before _tex_2d_slot is read — the first acquire is what resolves it
    return bindless_texture_2d_slot(_acquire(_tex_2d, _tex_2d_slot, view));
}

sv::bindless_texture_3d_slot sv::bindless_manager::acquire(sg::readonly_texture_view<sg::tv_3d> const& view)
{
    _ensure_staging(); // before _tex_3d_slot is read — the first acquire is what resolves it
    return bindless_texture_3d_slot(_acquire(_tex_3d, _tex_3d_slot, view));
}

sv::bindless_texture_cube_slot sv::bindless_manager::acquire(sg::readonly_texture_view<sg::tv_cube> const& view)
{
    _ensure_staging(); // before _tex_cube_slot is read — the first acquire is what resolves it
    return bindless_texture_cube_slot(_acquire(_tex_cube, _tex_cube_slot, view));
}

sg::binding_group_layout_handle const& sv::bindless_manager::layout()
{
    _ensure_staging();
    return _layout;
}

u32 sv::bindless_manager::_acquire(impl::slot_table& table, sg::binding_slot slot, sg::raw_view const& view)
{
    CC_ASSERT(!_locked, "no acquires while the bindless group is locked (unlock first)");

    // The table resolves identity; every mint and reclaim is mirrored onto the staging group, which is what
    // holds a mapped key's resource alive — so the key's raw pointer cannot be reused while it is mapped.
    auto const r = table.acquire(key_of(view), _ctx.current_epoch(),
                                 [&](u32 freed) { _staging->unset_array_element(slot, int(freed)); });
    if (r.inserted)
        _staging->set_array_element(slot, int(r.index), view);
    return r.index;
}

void sv::bindless_manager::_ensure_staging()
{
    if (_staging != nullptr)
        return;

    // One register space per category (index 0 each), so shaders address a category with no register-offset
    // math: `ByteAddressBuffer BindlessBuffers[N] : register(t0, space1);` and so on.
    // A hand-written texture binding carries its dimension — what shapes vacant elements' null descriptors.
    auto const tex_binding = [](cc::string_view name, u32 set, u32 count, sg::texture_view_dimension dim)
    {
        return sg::binding{.name = cc::string(name),
                           .set = set,
                           .index = 0,
                           .count = count,
                           .type = sg::binding_type::readonly_texture,
                           .texture_dimension = dim};
    };
    using vd = sg::texture_view_dimension;
    sg::binding const bindings[] = {
        {.name = _cfg.buffers_binding,
         .set = 1,
         .index = 0,
         .count = _cfg.buffer_count,
         .type = sg::binding_type::readonly_raw_buffer},
        tex_binding(_cfg.textures_1d_binding, 2, _cfg.texture_1d_count, vd::tex_1d),
        tex_binding(_cfg.textures_2d_binding, 3, _cfg.texture_2d_count, vd::tex_2d),
        tex_binding(_cfg.textures_3d_binding, 4, _cfg.texture_3d_count, vd::tex_3d),
        tex_binding(_cfg.textures_cube_binding, 5, _cfg.texture_cube_count, vd::cube),
    };
    _layout = _ctx.cached.acquire_binding_group_layout(bindings);
    _staging = _ctx.persistent.create_staging_binding_group(_layout);

    _buffers_slot = _staging->slot_of(_cfg.buffers_binding);
    _tex_1d_slot = _staging->slot_of(_cfg.textures_1d_binding);
    _tex_2d_slot = _staging->slot_of(_cfg.textures_2d_binding);
    _tex_3d_slot = _staging->slot_of(_cfg.textures_3d_binding);
    _tex_cube_slot = _staging->slot_of(_cfg.textures_cube_binding);

    // Every binding must be set once before the first snapshot; an empty table is a deliberate "nothing here".
    _staging->unset_array(_buffers_slot);
    _staging->unset_array(_tex_1d_slot);
    _staging->unset_array(_tex_2d_slot);
    _staging->unset_array(_tex_3d_slot);
    _staging->unset_array(_tex_cube_slot);
}

sg::binding_group_handle sv::bindless_manager::lock()
{
    CC_ASSERT(!_locked, "the bindless group is already locked");
    _ensure_staging();

    // The staging group mints only when a descriptor changed; an unchanged epoch gets the cached snapshot.
    _group = _staging->snapshot();

    _locked = true;
    _lock_epoch = _ctx.current_epoch();
    return _group;
}

void sv::bindless_manager::unlock(sg::binding_group_handle const& group)
{
    CC_ASSERT(_locked, "unlock without a lock");
    CC_ASSERT(group.get() == _group.get(), "unlock must receive the group lock returned");
    CC_ASSERT(_ctx.current_epoch() == _lock_epoch, "lock and unlock must happen in the same epoch");
    _locked = false;
}
