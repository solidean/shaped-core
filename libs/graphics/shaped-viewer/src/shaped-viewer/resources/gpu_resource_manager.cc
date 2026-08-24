#include "gpu_resource_manager.hh"

#include <clean-core/common/assert.hh>
#include <shaped-graphics/binding/staging_binding_group.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-rendering/box_filter_mipmap_routine.hh>

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
                                           sg::staging_binding_group_handle group,
                                           cc::vector<table_entry> tables,
                                           texture_policy texture_policy,
                                           work_budget work_budget)
  : meshes(cc::move(meshes)),
    materials(cc::move(materials)),
    textures(cc::move(textures)),
    _group(cc::move(group)),
    _tables(cc::move(tables)),
    _texture_policy(texture_policy),
    _work_budget(work_budget)
{
    for (auto& s : _slot_of)
        s = -1;
    for (auto i = isize(0); i < _tables.size(); ++i)
        _slot_of[u32(_tables[i].table)] = i32(i);
}

gpu_resource_manager gpu_resource_manager::create(sg::context& ctx, gpu_resource_manager_config const& cfg)
{
    auto const bindings = make_bindless_bindings(cfg.bindless);
    auto group = ctx.persistent.create_staging_binding_group(ctx.cached.acquire_binding_group_layout(bindings));

    // The arrays are built before the managers, because a manager that pins its resources — the texture one —
    // is handed the array it pins into rather than reaching back for it later.
    auto tables = cc::vector<table_entry>();
    tables.reserve(bindings.size());
    for (auto const& b : cfg.bindless.tables)
    {
        if (b.count == 0)
            continue;

        // for_binding clears the array, which is also what tells the group this binding was set — so the
        // "every binding set before the first snapshot" rule is satisfied by wiring alone.
        tables.push_back({.table = b.table, .array = sg::bindless_array::for_binding(ctx, group, name_of(b.table))});
    }

    auto const* const textures_2d = _find_table(tables, bindless_table::textures_2d);
    CC_ASSERT(textures_2d != nullptr, "the textures_2d table must be declared — the texture manager pins into it");

    return gpu_resource_manager(mesh_manager::create(ctx, cfg.meshes), material_manager::create(ctx, cfg.materials),
                                texture_manager::create(ctx, cfg.textures, textures_2d->array), cc::move(group),
                                cc::move(tables), cfg.textures_policy, cfg.work);
}

gpu_resource_manager::table_entry const* gpu_resource_manager::_find_table(cc::span<table_entry const> tables,
                                                                           bindless_table table)
{
    for (auto const& t : tables)
        if (t.table == table)
            return &t;
    return nullptr;
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

sg::bindless_index gpu_resource_manager::acquire_texture(bindless_table table, sg::raw_view const& view)
{
    CC_ASSERT(table != bindless_table::buffers, "acquire_texture is for the texture tables — use acquire_buffer");
    return _acquire(table, view);
}

sg::bindless_index gpu_resource_manager::acquire_buffer(sg::raw_view const& view)
{
    return _acquire(bindless_table::buffers, view);
}

sg::bindless_element_handle gpu_resource_manager::pin_texture(bindless_table table, sg::raw_view const& view)
{
    CC_ASSERT(table != bindless_table::buffers, "pin_texture is for the texture tables — use pin_buffer");
    return _pin(table, view);
}

sg::bindless_element_handle gpu_resource_manager::pin_buffer(sg::raw_view const& view)
{
    return _pin(bindless_table::buffers, view);
}

sg::bindless_index gpu_resource_manager::_acquire(bindless_table table, sg::raw_view const& view)
{
    CC_ASSERT(!_locked, "no acquires while frozen — the bound snapshot could not contain the mint");
    auto& t = _tables[_declared_slot_of(table)];
    auto const index = t.array.transient.acquire(view);
    _record(t, u32(index));
    return index;
}

sg::bindless_element_handle gpu_resource_manager::_pin(bindless_table table, sg::raw_view const& view)
{
    auto& t = _tables[_declared_slot_of(table)];
    auto handle = t.array.persistent.acquire(view);

    // A pinned element is resident for as long as the handle lives, so it belongs in every access declaration
    // this epoch — a dispatch reading it through a material buffer never went through acquire.
    _record(t, handle->index());
    return handle;
}

i32 gpu_resource_manager::_declared_slot_of(bindless_table table) const
{
    CC_ASSERT(table < bindless_table::count_, "not a bindless table");
    auto const slot = _slot_of[u32(table)];
    CC_ASSERT(slot >= 0, "that bindless table was not declared (its budget is 0)");
    return slot;
}

void gpu_resource_manager::_record(table_entry& t, u32 index)
{
    // Re-acquiring a view returns the index it already has, so the list is deduplicated rather than appended to
    // blindly — an access declaration naming one element twice is not what a dispatch expects.
    for (auto const e : t.acquired)
        if (e == index)
            return;
    t.acquired.push_back(index);
}

texture_id gpu_resource_manager::acquire_texture(texture_data const& texture)
{
    CC_ASSERT(!_locked, "no acquires while frozen — the bound snapshot could not contain the mint");
    auto const id = textures.acquire(texture);

    // Its element is pinned, so nothing here has to keep it alive; what this does is make sure the dispatch
    // that reads it through a material buffer finds it in the access declaration.
    auto const* const record = textures.get_ptr(id);
    CC_ASSERT(record != nullptr, "a freshly acquired texture must be resident");
    _record(_tables[_declared_slot_of(bindless_table::textures_2d)], record->index());

    // Queued rather than done here: an acquire is on the caller's critical path, and generating a full chain
    // inline is exactly the stall the budget exists to spread out.
    if (_texture_policy.generate_mips && record->state == residency::base_resident && !_is_pending(id))
    {
        auto const dispatches = sr::box_filter_mipmap_routine::level_count(record->texture, record->uploaded_mips);
        if (dispatches > 0)
            _pending.push_back({.texture = id, .dispatches = dispatches});
    }
    return id;
}

i32 gpu_resource_manager::record_pending_work(sg::command_list& cmd)
{
    if (_work_budget.max_dispatches_per_epoch <= 0 || _pending.empty())
        return 0;

    auto spent = i32(0);
    auto keep = cc::vector<pending_work>();
    for (auto const& w : _pending)
    {
        auto const* const record = textures.get_ptr(w.texture);
        if (record == nullptr)
            continue; // evicted since it was queued, so nobody is waiting on it any more

        // Oldest first, and a request that does not fit is not split: a partially generated chain would read
        // as complete while its tail is still uninitialized.
        if (spent + w.dispatches > _work_budget.max_dispatches_per_epoch && spent > 0)
        {
            keep.push_back(w);
            continue;
        }

        sr::box_filter_mipmap_routine::execute(cmd, record->texture, record->uploaded_mips);
        spent += w.dispatches;
        textures.mark_mips_complete(w.texture);
    }
    _pending = cc::move(keep);
    return spent;
}

bool gpu_resource_manager::_is_pending(texture_id id) const
{
    for (auto const& w : _pending)
        if (w.texture == id)
            return true;
    return false;
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
