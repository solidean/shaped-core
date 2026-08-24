#include "gpu_resource_manager.hh"

#include <clean-core/common/assert.hh>
#include <shaped-graphics/binding/staging_binding_group.hh>
#include <shaped-graphics/context/context.hh>

namespace sv
{
bound_resources::~bound_resources()
{
    if (_manager != nullptr)
        _manager->unlock();
}

bound_resources::bound_resources(bound_resources&& rhs) noexcept : _manager(rhs._manager), _group(cc::move(rhs._group))
{
    rhs._manager = nullptr;
}

cc::span<u32 const> bound_resources::elements(bindless_table table) const
{
    CC_ASSERT(_manager != nullptr, "a moved-from bound_resources names no manager");
    auto const* const e = _manager->_array_of(table);
    if (e == nullptr)
        return {};
    return _manager->_tables[_manager->_slot_of[u32(table)]].acquired;
}

gpu_resource_manager::gpu_resource_manager(mesh_manager meshes,
                                           material_manager materials,
                                           texture_manager textures,
                                           sg::staging_binding_group_handle group)
  : meshes(cc::move(meshes)), materials(cc::move(materials)), textures(cc::move(textures)), _group(cc::move(group))
{
    for (auto& s : _slot_of)
        s = -1;
}

gpu_resource_manager gpu_resource_manager::create(sg::context& ctx, gpu_resource_manager_config const& cfg)
{
    auto const bindings = make_bindless_bindings(cfg.bindless);
    auto group = ctx.persistent.create_staging_binding_group(ctx.cached.acquire_binding_group_layout(bindings));

    auto m = gpu_resource_manager(mesh_manager::create(ctx, cfg.meshes), material_manager::create(ctx, cfg.materials),
                                  texture_manager::create(ctx, cfg.textures), cc::move(group));

    m._tables.reserve(bindings.size());
    for (auto const& b : cfg.bindless.tables)
    {
        if (b.count == 0)
            continue;

        // for_binding clears the array, which is also what tells the group this binding was set — so the
        // "every binding set before the first snapshot" rule is satisfied by wiring alone.
        m._slot_of[u32(b.table)] = i32(m._tables.size());
        m._tables.push_back({.table = b.table, .array = sg::bindless_array::for_binding(ctx, m._group, name_of(b.table))});
    }
    return m;
}

void gpu_resource_manager::advance_to(sg::epoch e)
{
    CC_ASSERT(!_locked, "cannot advance the epoch while frozen — the snapshot's indices would not survive it");
    if (e == _epoch)
        return;

    meshes.begin_frame(e);
    materials.begin_frame(e);
    textures.begin_frame(e);
    for (auto& t : _tables)
        t.acquired.clear();
    _epoch = e;
}

u32 gpu_resource_manager::acquire_texture(bindless_table table, sg::raw_view const& view)
{
    CC_ASSERT(table != bindless_table::buffers, "acquire_texture is for the texture tables — use acquire_buffer");
    return _acquire(table, view);
}

u32 gpu_resource_manager::acquire_buffer(sg::raw_view const& view)
{
    return _acquire(bindless_table::buffers, view);
}

u32 gpu_resource_manager::_acquire(bindless_table table, sg::raw_view const& view)
{
    CC_ASSERT(!_locked, "no acquires while frozen — the bound snapshot could not contain the mint");
    auto const slot = _slot_of[u32(table)];
    CC_ASSERT(slot >= 0, "that bindless table was not declared (its budget is 0)");

    auto& t = _tables[slot];
    auto const index = t.array.acquire(view);

    // Re-acquiring a view returns the index it already has, so the list is deduplicated rather than appended to
    // blindly — an access declaration naming one element twice is not what a dispatch expects.
    for (auto const e : t.acquired)
        if (e == index)
            return index;
    t.acquired.push_back(index);
    return index;
}

void gpu_resource_manager::lock()
{
    CC_ASSERT(!_locked, "already locked");
    _locked = true;
}

void gpu_resource_manager::unlock()
{
    CC_ASSERT(_locked, "unlock without a lock");
    _locked = false;
}

bound_resources gpu_resource_manager::freeze()
{
    lock();
    return bound_resources(*this, _group->snapshot());
}

bool gpu_resource_manager::has_table(bindless_table table) const
{
    return _array_of(table) != nullptr;
}

u32 gpu_resource_manager::table_capacity(bindless_table table) const
{
    auto const* const a = _array_of(table);
    return a == nullptr ? 0 : a->capacity();
}

sg::bindless_array* gpu_resource_manager::_array_of(bindless_table table)
{
    CC_ASSERT(table < bindless_table::count_, "not a bindless table");
    auto const slot = _slot_of[u32(table)];
    return slot < 0 ? nullptr : &_tables[slot].array;
}

sg::bindless_array const* gpu_resource_manager::_array_of(bindless_table table) const
{
    CC_ASSERT(table < bindless_table::count_, "not a bindless table");
    auto const slot = _slot_of[u32(table)];
    return slot < 0 ? nullptr : &_tables[slot].array;
}
} // namespace sv
