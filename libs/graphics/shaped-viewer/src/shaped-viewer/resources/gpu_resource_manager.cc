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
#include <shaped-viewer/scene/resident_mesh.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::pow

namespace sv
{
namespace
{
/// One constant read back as up to four floats, for seeding a placeholder.
/// Anything that is not f32 comes back white: a placeholder is a stand-in, and guessing at an integer encoding would
/// be a worse answer than a neutral one.
[[nodiscard]] tg::vec4f constant_as_vec4(cc::span<byte const> bytes, attribute_format format)
{
    auto out = tg::vec4f(1, 1, 1, 1);
    if (format.scalar != scalar_type::f32)
        return out;

    auto const floats = bytes.try_reinterpret_as<f32 const>();
    if (!floats.has_value())
        return out;

    auto const count = cc::min(isize(4), floats.value().size());
    for (auto i = isize(0); i < count; ++i)
        out[int(i)] = floats.value()[i];
    return out;
}

/// The texel a 1x1 placeholder for `a` must hold for the shader to compute the material's own factor from it.
///
/// Both directions are undone here rather than approximated: the swizzle says which CHANNEL each component is read
/// from, and the transform says what is done to it after — so the texel is the factor pushed back through both.
/// A normal map's placeholder therefore comes out as (0.5, 0.5, 1), which is what decodes to the default normal.
[[nodiscard]] tg::vec4f placeholder_texel_for(resolved_attribute const& a)
{
    CC_ASSERT(a.sample != nullptr, "a placeholder texel is only meaningful for a sampled attribute");

    auto const desired = constant_as_vec4(a.fallback_constant, a.format);
    auto const& transform = a.sample->transform;
    auto const& swizzle = a.sample->swizzle;

    // Opaque white to start: a channel the swizzle never reads is never sampled through this slot.
    auto texel = tg::vec4f(1, 1, 1, 1);
    for (auto i = 0; i < a.format.component_count(); ++i)
    {
        auto const scale = transform.scale[i];
        auto const value = scale != 0.0f ? (desired[i] - transform.bias[i]) / scale : 0.0f;

        // zero and one name no channel, so there is nothing to put anywhere for them.
        if (auto const channel = swizzle.components[i]; channel <= texture_channel::a)
            texel[int(channel)] = value;
    }
    return texel;
}

/// The object-space box around a geometry's positions, empty when it has none.
///
/// A summary the GPU mesh keeps, since once the positions are only on the GPU nothing else can answer a framing question.
/// Only the fallback: a `mesh` that already carries a box — every glTF one does, since the format states it per
/// accessor — keeps it rather than paying this scan.
[[nodiscard]] cc::optional<tg::aabb3f> bounds_of(triangle_geometry const& g)
{
    auto const positions = g.positions.span();
    if (positions.empty())
        return {};

    auto box = tg::aabb3f(positions[0], positions[0]);
    for (auto const& p : positions)
        for (auto i = 0; i < 3; ++i)
        {
            box.min[i] = cc::min(box.min[i], p[i]);
            box.max[i] = cc::max(box.max[i], p[i]);
        }
    return box;
}
} // namespace

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

gpu_resource_manager::gpu_resource_manager(sg::context& ctx,
                                           mesh_manager meshes,
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
    _work_budget(work_budget),
    _ctx(&ctx)
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
        ctx, mesh_manager::create(ctx, cfg.meshes), material_manager::create(ctx, cfg.materials),
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
        case material_slot_kind::sample_transform:
            // Both are bytes the resolution already decided; only where they came from differs.
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

            // Substituted at the SLOT rather than by letting the sample lose to a coarser rank.
            // Losing would be no new code, but it flips the permutation when the texture lands — so every affected
            // mesh would recompile and restart its accumulation mid-load, which is the opposite of what a placeholder
            // is for.
            // Pointing the slot elsewhere keeps the permutation stable across the whole load.
            auto const& texture = record.state == residency::pending
                                    ? _placeholder_texture(slot.placeholder_texel, record.texture.format())
                                    : record.texture;

            auto index = cc::vector<byte>();
            append_pod(index, u32(acquire_texture(bindless_table::textures_2d, texture.as_readonly_view())));
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

    // A pending mesh is traced as the placeholder cube, so its record has to name the CUBE's geometry: a hit reads
    // positions back out of the instance to recompute the geometric normal, and the real buffer holds nothing yet.
    auto const pending = m.state != residency::complete;
    auto const& vertices = pending ? meshes.placeholder_vertices() : m.vertices;
    auto const& indices = pending ? meshes.index_stand_in() : m.indices;

    // Every index here is this epoch's, minted right where it is written — which is what puts all four into the access
    // declaration `freeze()` hands the trace.
    return {.param_buffer = u32(acquire_buffer(r.parameters.as_readonly_buffer())),
            .param_offset = 0, // one block per buffer today; the shader reads through the offset regardless
            .vertices = u32(acquire_buffer(vertices.raw()->as_raw_readonly())),
            .indices = u32(acquire_buffer(indices.raw()->as_raw_readonly())),
            .is_indexed = (!pending && m.is_indexed) ? 1u : 0u};
}

bool gpu_resource_manager::_is_resident(sv::resident_mesh const& m)
{
    auto const* const geometry = meshes.get_ptr(m.geometry);
    if (geometry == nullptr || geometry->state != residency::complete)
        return false;

    for (auto const& a : m.attributes)
    {
        // A per_instance attribute uploads nothing, so there is nothing to wait for.
        if (a.attribute == attribute_id::invalid)
            continue;
        auto const* const record = attributes.get_ptr(a.attribute);
        if (record == nullptr || record->state != residency::complete)
            return false;
    }

    for (auto const& t : m.textures)
    {
        auto const* const record = textures.get_ptr(t.source.texture);
        if (record == nullptr || record->state == residency::pending)
            return false;
    }

    return true;
}

sv::resident_mesh const& gpu_resource_manager::create_mesh(sv::mesh const& data)
{
    CC_ASSERT(!data.geometry.is_empty(), "a mesh needs geometry to be placed in a scene");

    // Placed against this manager before: the ids are already minted, so all that is left is to say whether they have
    // arrived since.
    // The transform, material and flags are re-read anyway, because those are the parts a caller changes between
    // frames without changing a single payload.
    if (data.cache.manager == this)
    {
        data.cache.resources.transform = data.transform;
        data.cache.resources.material = data.material;
        data.cache.resources.flags = data.flags;
        data.cache.ready = _is_resident(data.cache.resources);
        return data.cache.resources;
    }

    // The box travels with the payload rather than being recomputed from it: the manager keeps it after the bytes are
    // gone, and a placeholder drawn while the geometry is still arriving has nothing else to be sized by.
    auto const box = data.bounds.has_value() ? data.bounds : bounds_of(data.geometry);

    // Both bridges keep the geometry's own content key, so this resolves to a resident id rather than an upload
    // whenever the mesh has not changed.
    auto const geometry = [&]
    {
        if (data.geometry.is_indexed())
        {
            auto payload = indexed_triangle_data::from(data.geometry);
            payload.bounds = box;
            return meshes.acquire(payload);
        }
        auto payload = triangle_data::from(data.geometry);
        payload.bounds = box;
        return meshes.acquire(payload);
    }();

    auto bindings = cc::vector<mesh_attribute_binding>();
    bindings.reserve(data.attributes.size());
    for (auto const& a : data.attributes)
    {
        // A per_instance attribute is one value for the whole mesh, read out of the parameter block rather than off a buffer,
        // so it never becomes a resource and its bytes travel along instead.
        auto const uploads = a.frequency != attribute_frequency::per_instance;
        bindings.push_back(mesh_attribute_binding::of(a, uploads ? attributes.acquire(a) : attribute_id::invalid));
    }

    auto bound_textures = cc::vector<mesh_texture_binding>();
    bound_textures.reserve(data.textures.size());
    for (auto const& t : data.textures)
        bound_textures.push_back({.name = t.name,
                                  .source = {.texture = textures.acquire(t.source.texture),
                                             .uv_attribute = t.source.uv_attribute,
                                             .sampler = t.source.sampler,
                                             .swizzle = t.source.swizzle,
                                             .transform = t.source.transform}});

    data.cache = {.manager = this,
                  .resources = {.name = data.name,
                                .geometry = geometry,
                                .attributes = cc::move(bindings),
                                .transform = data.transform,
                                .material = data.material,
                                .flags = data.flags,
                                .textures = cc::move(bound_textures),
                                .bounds = box,
                                .triangle_count = data.geometry.triangle_count(),
                                .vertex_count = data.geometry.vertex_count()}};

    data.cache.ready = _is_resident(data.cache.resources);
    return data.cache.resources;
}

scene_item gpu_resource_manager::acquire_scene_item(sv::resident_mesh const& mesh)
{
    CC_ASSERT(mesh.geometry != mesh_id::invalid, "a mesh needs geometry to be placed in a scene");

    auto const lib = acquire_material_library();
    CC_ASSERT(lib.has_value(), "shaped-viewer: no material library to resolve a mesh's material through");

    auto const material = mesh.material == material_id::invalid ? default_material(*lib.value()) : mesh.material;
    auto const resolved = resolve_material(*lib.value(), material, mesh);
    auto const& permutation = shaders.acquire(resolved);

    return {.mesh = mesh.geometry,
            .instance = acquire_instance(resolved, permutation.layout),
            .shader_key = permutation.key,
            .transform = mesh.transform};
}

scene_item gpu_resource_manager::acquire_scene_item(sv::mesh const& mesh)
{
    return acquire_scene_item(create_mesh(mesh));
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
            CC_ASSERT(source->attribute != attribute_id::invalid, "a descriptor slot names an uploaded attribute");
            CC_ASSERT(slot.size_bytes == attribute_desc_size, "an sv::attribute_desc slot is exactly its three uints");
            record.slots.push_back({.kind = slot.kind,
                                    .offset = slot.offset,
                                    .size_bytes = slot.size_bytes,
                                    .attribute = source->attribute,
                                    .element_stride = u32(source->format.size_bytes())});
            break;
        }

        case material_slot_kind::sample_transform:
        {
            CC_ASSERT(a.sample != nullptr, "a sample transform slot names a sampled attribute");
            CC_ASSERT(slot.size_bytes == i32(sizeof(tg::vec4f)) * 2, "a sample transform is a scale and a bias");
            auto bytes = cc::vector<byte>();
            append_pod(bytes, a.sample->transform.scale);
            append_pod(bytes, a.sample->transform.bias);
            record.slots.push_back(
                {.kind = slot.kind, .offset = slot.offset, .size_bytes = slot.size_bytes, .constant = cc::move(bytes)});
            break;
        }

        case material_slot_kind::texture_index:
            CC_ASSERT(a.sample != nullptr, "a texture slot names a sampled attribute");
            CC_ASSERT(slot.size_bytes == i32(sizeof(u32)), "a texture slot is exactly one bindless index");
            // The id is one the caller acquired; whether its PIXELS have arrived is what the block decides per epoch.
            record.slots.push_back({.kind = slot.kind,
                                    .offset = slot.offset,
                                    .size_bytes = slot.size_bytes,
                                    .texture = a.sample->texture,
                                    .placeholder_texel = placeholder_texel_for(a)});
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

    // Nothing is queued here any more: a freshly acquired texture is `pending`, so whether it will need its chain
    // filled is not knowable until its pixels land.
    // `_queue_mip_work` is what decides that, from the ids `collect_settled` reports.
    return id;
}

sg::texture_2d const& gpu_resource_manager::_placeholder_texture(tg::vec4f texel, sg::pixel_format format)
{
    auto const encode = [&](float v)
    {
        auto const clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);

        // An sRGB view decodes what it reads, so the value has to be stored encoded to come back as itself.
        auto const stored
            = sg::is_srgb_format(format)
                ? (clamped <= 0.0031308f ? clamped * 12.92f : 1.055f * tg::pow(clamped, 1.0f / 2.4f) - 0.055f)
                : clamped;
        return u32(stored * 255.0f + 0.5f);
    };

    auto const rgba = encode(texel[0]) | (encode(texel[1]) << 8) | (encode(texel[2]) << 16) | (encode(texel[3]) << 24);
    auto const key = u64(rgba) | (u64(u32(format)) << 32);

    if (auto const* const resident = _placeholder_textures.get_ptr(key); resident != nullptr)
        return *resident;

    auto gpu = _ctx->persistent.create_texture_2d(
        {.format = format,
         .width = 1,
         .height = 1,
         .mip_levels = 1,
         .usage = sg::texture_usage::readonly_texture | sg::texture_usage::copy_dst});

    // Four bytes, and needed by the very next recording: `ctx.upload`'s automatic wait is exactly right here, which is
    // the same reasoning that puts the bulk traffic on `ctx.stream` instead.
    byte const pixels[4]
        = {byte(rgba & 0xFFu), byte((rgba >> 8) & 0xFFu), byte((rgba >> 16) & 0xFFu), byte((rgba >> 24) & 0xFFu)};
    auto cmd = _ctx->create_command_list();
    cmd->upload.bytes_to_texture(gpu.raw(), pixels, {});
    _ctx->submit_command_list(cc::move(cmd));

    _placeholder_textures[key] = cc::move(gpu);
    return _placeholder_textures[key];
}

void gpu_resource_manager::_collect_textures()
{
    auto landed = cc::vector<texture_id>();
    (void)textures.collect_settled(landed);
    _queue_mip_work(landed);
}

void gpu_resource_manager::_queue_mip_work(cc::span<texture_id const> landed)
{
    if (!_texture_policy.generate_mips)
        return;

    for (auto const id : landed)
    {
        auto const* const record = textures.get_ptr(id);
        if (record == nullptr || record->state != residency::base_resident || _is_pending(id))
            continue;

        // Queued rather than done here: generating a full chain inline is exactly the stall the budget spreads out.
        auto const dispatches = sr::box_filter_mipmap_routine::level_count(record->texture, record->uploaded_mips);
        if (dispatches > 0)
            _pending.push_back({.texture = id, .dispatches = dispatches});
    }
}

void gpu_resource_manager::wait_for_pending_uploads()
{
    attributes.wait_for_settled();
    meshes.wait_for_settled();

    auto landed = cc::vector<texture_id>();
    textures.wait_for_settled(landed);
    _queue_mip_work(landed);
}

i32 gpu_resource_manager::record_pending_work(sg::command_list& cmd)
{
    // Whatever landed since the last epoch is finished first, so a mesh whose geometry arrived draws its real
    // triangles this frame rather than next.
    // What ORDER the transfers themselves ran in is the streaming actor's business, decided by the priority each
    // acquire set — this only collects the results.
    (void)attributes.collect_settled();
    (void)meshes.record_settled(cmd);
    _collect_textures();

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
