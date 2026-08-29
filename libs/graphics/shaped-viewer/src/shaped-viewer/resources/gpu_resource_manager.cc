#include "gpu_resource_manager.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::memcmp
#include <shaped-graphics/binding/staging_binding_group.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-rendering/box_filter_mipmap_routine.hh>
#include <shaped-viewer/material/material_library.hh>
#include <shaped-viewer/material/resolve.hh>
#include <shaped-viewer/material/shader_generator.hh>
#include <shaped-viewer/resources/resource_data.hh>
#include <shaped-viewer/scene/mesh.hh>
#include <shaped-viewer/scene/mesh_attribute.hh>

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

sg::binding_group_layout_handle const& bound_resources::layout() const
{
    CC_ASSERT(_manager != nullptr, "a moved-from bound_resources names no manager");
    return _manager->bindless_layout();
}

void bound_resources::declare_raytracing_access(sg::command_list& cmd) const
{
    CC_ASSERT(_manager != nullptr, "a moved-from bound_resources names no manager");

    auto buffers = cc::vector<sg::array_buffer_access>();
    auto textures = cc::vector<sg::array_texture_access>();

    for (auto const& t : _manager->_tables)
    {
        if (t.table == bindless_table::buffers)
        {
            buffers.clear();
            for (auto const e : t.acquired)
                buffers.push_back({.index = i32(e),
                                   .stages = sg::pipeline_stage_flag::raytracing,
                                   .access = sg::access_flag::shader_read});
            cmd.raytracing.declare_array_buffer_access(name_of(t.table), buffers);
            continue;
        }

        textures.clear();
        for (auto const e : t.acquired)
            textures.push_back({.index = i32(e),
                                .stages = sg::pipeline_stage_flag::raytracing,
                                .access = sg::access_flag::shader_read,
                                .layout = sg::texture_layout::shader_readonly});
        cmd.raytracing.declare_array_texture_access(name_of(t.table), textures);
    }
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
                                           attribute_manager attributes,
                                           material_shader_cache shaders,
                                           sg::staging_binding_group_handle group,
                                           cc::vector<table_entry> tables,
                                           texture_policy texture_policy,
                                           work_budget work_budget)
  : meshes(cc::move(meshes)),
    materials(cc::move(materials)),
    textures(cc::move(textures)),
    attributes(cc::move(attributes)),
    shaders(cc::move(shaders)),
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

    // One array per binding, owned here: an array cannot refuse an acquire on behalf of its siblings, so nothing
    // else may hold one over a binding of this group.
    auto tables = cc::vector<table_entry>();
    tables.reserve(bindings.size());
    for (auto const& b : cfg.bindless.tables)
    {
        if (b.count == 0)
            continue;

        // for_binding clears the array, which is also what tells the group this binding was set — so the
        // "every binding set before the first snapshot" rule is satisfied by wiring alone.
        auto array = sg::bindless_array::for_binding(ctx, group, name_of(b.table));
        auto recorded_in = cc::vector<u64>::create_defaulted(isize(array.capacity()));
        tables.push_back({.table = b.table, .array = cc::move(array), .recorded_in = cc::move(recorded_in)});
    }

    CC_ASSERT(_find_table(tables, bindless_table::textures_2d) != nullptr, "the textures_2d table must be declared — a "
                                                                           "sampled texture is acquired into it");
    CC_ASSERT(_find_table(tables, bindless_table::buffers) != nullptr, "the buffers table must be declared — geometry, "
                                                                       "attributes and parameter blocks are acquired "
                                                                       "into it");

    // The first accepted format is the context's own preference, and a permutation is keyed by its source rather than
    // by its format — so a cache producing anything else would be compiling for a device that cannot take it.
    CC_ASSERT(!ctx.accepted_shader_formats().empty(), "a context accepts at least one shader format");
    auto const format = ctx.accepted_shader_formats().front();

    return gpu_resource_manager(
        mesh_manager::create(ctx, cfg.meshes), material_manager::create(ctx, cfg.materials),
        texture_manager::create(ctx, cfg.textures), attribute_manager::create(ctx, cfg.attributes),
        material_shader_cache::create(
            format, {.epilogue_include = material_shader_cache::hit_epilogue_include, .bindless = &cfg.bindless}),
        cc::move(group), cc::move(tables), cfg.textures_policy, cfg.work);
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
    CC_ASSERT(e >= _epoch, "cannot advance to an epoch already left behind — a stale caller would evict this frame's "
                           "working set and walk the epoch backwards");
    if (e == _epoch)
        return;

    meshes.begin_frame(e);
    materials.begin_frame(e);
    textures.begin_frame(e);
    attributes.begin_frame(e);
    for (auto& t : _tables)
        t.acquired.clear();
    ++_record_stamp;
    _epoch = e;
}


namespace
{
/// sv::attribute_desc: buffer, offset, stride — three uints, which is what a descriptor slot is sized for.
constexpr i32 attribute_desc_size = 12;

/// Appends `value` to `out` as raw bytes, which is how every slot of a parameter block is written.
template <class T>
void append_pod(cc::vector<byte>& out, T const& value)
{
    auto const src = cc::span<T const>(&value, 1).as_bytes();
    for (auto b : src)
        out.push_back(b);
}
} // namespace

cc::vector<byte> gpu_resource_manager::build_instance_parameters(instance_record const& r)
{
    // Zero-filled rather than uninitialized: a layout may leave alignment padding between slots, and a block whose padding
    // varies run to run would be two different uploads of the same material.
    auto out = cc::vector<byte>::create_filled(r.size_bytes, byte(0));

    auto const write = [&](i32 offset, cc::span<byte const> bytes)
    {
        CC_ASSERT(offset + bytes.size() <= out.size(), "a parameter slot does not fit the block its layout sized");
        for (auto i = isize(0); i < bytes.size(); ++i)
            out[offset + i] = bytes[i];
    };

    for (auto const& slot : r.slots)
    {
        switch (slot.kind)
        {
        case material_slot_kind::constant:
            write(slot.offset, slot.constant);
            break;

        case material_slot_kind::attribute_descriptor:
        {
            auto const& record = attributes.get(slot.attribute);

            // Tightly packed from offset 0 within its own buffer, so the stride is just the element size.
            auto desc = cc::vector<byte>();
            append_pod(desc, u32(acquire_buffer(record.data.as_readonly_buffer())));
            append_pod(desc, u32(0));
            append_pod(desc, slot.element_stride);
            write(slot.offset, desc);
            break;
        }

        case material_slot_kind::texture_index:
        {
            auto const& record = textures.get(slot.texture);
            auto index = cc::vector<byte>();
            append_pod(index, u32(acquire_texture(bindless_table::textures_2d, record.texture.as_readonly_view())));
            write(slot.offset, index);
            break;
        }
        }
    }

    return out;
}

instance_gpu gpu_resource_manager::describe_instance(sg::command_list& cmd, mesh_id mesh, instance_id instance)
{
    auto const& m = meshes.get(mesh);

    CC_ASSERT(contains_instance(instance), "no such instance_id");
    auto& r = _instances[isize(u32(instance))];

    auto bytes = build_instance_parameters(r);

    // A material whose every attribute is sourced from somewhere needing no parameter has an empty block, and a zero-sized
    // buffer has no descriptor to acquire.
    // Four bytes keep the shape uniform rather than making the shader branch on whether it has a block at all.
    if (r.parameters.raw() == nullptr)
        r.parameters = cmd.context().persistent.create_buffer<byte>(
            bytes.empty() ? isize(4) : bytes.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);

    // The bytes are the epoch's, the buffer is not.
    // In the steady state they come out identical, and skipping the copy is what keeps the upload path quiet — the buffer's
    // own descriptor never moves either way, which is the part that matters for the group's snapshot.
    auto const unchanged = bytes.size() == r.uploaded.size()
                        && (bytes.empty() || cc::memcmp(bytes.data(), r.uploaded.data(), size_t(bytes.size())) == 0);
    if (!bytes.empty() && !unchanged)
    {
        cmd.upload.data_to_buffer(r.parameters, bytes);
        r.uploaded = cc::move(bytes);
    }

    // Every index here is this epoch's, minted right where it is written — which is what puts all four into the access
    // declaration `freeze()` hands the trace.
    return {.param_buffer = u32(acquire_buffer(r.parameters.as_readonly_buffer())),
            .param_offset = 0, // one block per buffer today; the shader reads through the offset regardless
            .vertices = u32(acquire_buffer(m.vertices.raw()->as_raw_readonly())),
            .indices = u32(acquire_buffer(m.indices.raw()->as_raw_readonly())),
            .is_indexed = m.is_indexed ? 1u : 0u};
}

scene_item gpu_resource_manager::acquire_scene_item(sv::mesh const& mesh)
{
    CC_ASSERT(!mesh.geometry.is_empty(), "a mesh needs geometry to be placed in a scene");

    // Both bridges keep the geometry's own content key, so this resolves to a resident id rather than an upload
    // whenever the mesh has not changed.
    auto const geometry = mesh.geometry.is_indexed() ? meshes.acquire(indexed_triangle_data::from(mesh.geometry))
                                                     : meshes.acquire(triangle_data::from(mesh.geometry));

    auto const lib = acquire_material_library();
    CC_ASSERT(lib.has_value(), "shaped-viewer: no material library to resolve a mesh's material through");

    auto const material = mesh.material == material_id::invalid ? default_material(*lib.value()) : mesh.material;
    auto const resolved = resolve_material(*lib.value(), material, mesh);
    auto const& permutation = shaders.acquire(resolved);

    return {.mesh = geometry,
            .instance = acquire_instance(resolved, permutation.layout),
            .shader_key = permutation.key,
            .transform = mesh.transform};
}

bool gpu_resource_manager::contains_instance(instance_id id) const
{
    return id != instance_id::invalid && isize(u32(id)) < _instances.size();
}

instance_record const& gpu_resource_manager::get_instance(instance_id id) const
{
    CC_ASSERT(contains_instance(id), "no such instance_id");
    return _instances[isize(u32(id))];
}

instance_id gpu_resource_manager::acquire_instance(resolved_material const& r, material_parameter_layout const& layout)
{
    // The layout is what the generated shader reads, so a block built from a different one would be read at the wrong offsets.
    CC_ASSERT(r.type != nullptr, "a resolved material names its type");

    if (auto const* const resident = _instances_by_key.get_ptr(r.parameter_key); resident != nullptr)
        return *resident;

    auto record = instance_record{.size_bytes = layout.size_bytes};
    record.slots.reserve(layout.slots.size());

    for (auto const& slot : layout.slots)
    {
        auto const& a = r.attributes[slot.attribute_index];
        switch (slot.kind)
        {
        case material_slot_kind::constant:
            CC_ASSERT(a.constant.size() == slot.size_bytes, "a constant slot is its declaration's size");
            record.slots.push_back({.kind = slot.kind,
                                    .offset = slot.offset,
                                    .size_bytes = slot.size_bytes,
                                    .constant = cc::vector<byte>::create_copy_of(a.constant)});
            break;

        case material_slot_kind::attribute_descriptor:
        {
            // A descriptor slot serves either the attribute itself or, for a sampled one, the uv set it samples through.
            auto const* const source = a.sample != nullptr ? a.uv : a.attribute;
            CC_ASSERT(source != nullptr, "a descriptor slot names a mesh attribute the resolve found");
            CC_ASSERT(slot.size_bytes == attribute_desc_size, "an sv::attribute_desc slot is exactly its three uints");
            record.slots.push_back({.kind = slot.kind,
                                    .offset = slot.offset,
                                    .size_bytes = slot.size_bytes,
                                    .attribute = attributes.acquire(*source),
                                    .element_stride = u32(source->format.size_bytes())});
            break;
        }

        case material_slot_kind::texture_index:
            CC_ASSERT(a.sample != nullptr, "a texture slot names a sampled attribute");
            CC_ASSERT(slot.size_bytes == i32(sizeof(u32)), "a texture slot is exactly one bindless index");
            // The texture must already be resident: a `texture_id` on a mesh is one the caller acquired.
            record.slots.push_back(
                {.kind = slot.kind, .offset = slot.offset, .size_bytes = slot.size_bytes, .texture = a.sample->texture});
            break;
        }
    }

    auto const id = instance_id(u32(_instances.size()));
    _instances.push_back(cc::move(record));
    _instances_by_key[r.parameter_key] = id;
    return id;
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
    CC_ASSERT(isize(index) < t.recorded_in.size(), "a bindless index outside its array's capacity");

    // Re-acquiring a view returns the index it already has, so the list is deduplicated rather than appended to
    // blindly — an access declaration naming one element twice is not what a dispatch expects.
    // The stamp is what makes that a compare: `acquired` grows across the epoch, so scanning it would cost the
    // n-th acquire n comparisons, on a path both `_acquire` and `_pin` reach.
    if (t.recorded_in[isize(index)] == _record_stamp)
        return;
    t.recorded_in[isize(index)] = _record_stamp;
    t.acquired.push_back(index);
}

texture_id gpu_resource_manager::acquire_texture(texture_data const& texture)
{
    CC_ASSERT(!_locked, "no acquires while frozen — the bound snapshot could not contain the mint");
    auto const id = textures.acquire(texture);

    auto const* const record = textures.get_ptr(id);
    CC_ASSERT(record != nullptr, "a freshly acquired texture must be resident");

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

sg::binding_group_layout_handle const& gpu_resource_manager::bindless_layout() const
{
    return _group->layout();
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
