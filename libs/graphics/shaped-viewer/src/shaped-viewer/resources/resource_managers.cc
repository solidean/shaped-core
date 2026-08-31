#include <clean-core/common/assert.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/impl/content_hash.hh>
#include <shaped-viewer/resources/impl/mip_layout.hh>
#include <shaped-viewer/resources/resource_managers.hh>
#include <shaped-viewer/scene/mesh_attribute.hh>

namespace sv
{
mesh_manager mesh_manager::create(sg::context& ctx, manager_config const& cfg)
{
    auto manager = mesh_manager(ctx);
    manager.set_limits(cfg.budget.max_bytes, cfg.budget.max_idle_epochs);
    return manager;
}

namespace
{
// accel_structure_build_input for the BLAS build; readonly_buffer so the closest-hit can read the geometry
// back (as StructuredBuffers) to recompute the flat face normal.
constexpr auto geometry_usage
    = sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst;
} // namespace

mesh_id mesh_manager::acquire(triangle_data const& mesh)
{
    if (auto const id = find_by_hash(mesh.hash); id.has_value())
        return id.value();

    auto const positions = mesh.positions.span();
    CC_ASSERT(!positions.empty() && positions.size() % 3 == 0, "mesh_manager::acquire expects a triangle list (vertex "
                                                               "count a non-zero multiple of 3)");

    // The BUFFER is created here and the bytes are not: an acquire mints an id and queues the work, and
    // `record_uploads` is what spends the bandwidth — at whatever rate the epoch's budget allows.
    auto vertices = _ctx.persistent.create_buffer<tg::pos3f>(positions.size(), geometry_usage);

    auto up = _ctx.create_command_list();
    auto stand_in = _acquire_index_stand_in(*up);
    _ctx.submit_command_list(cc::move(up));

    // The BLAS is not charged yet, since it is not built: `record_uploads` adds its size once it is.
    auto const size_in_bytes = vertices.size_in_bytes();
    auto const id = insert(mesh.hash,
                           {.state = residency::pending,
                            .vertices = cc::move(vertices),
                            .indices = cc::move(stand_in),
                            .is_indexed = false,
                            .triangle_count = positions.size() / 3,
                            .bounds = mesh.bounds},
                           size_in_bytes);

    _pending.push_back({.id = id, .positions = mesh.positions, .bytes = size_in_bytes});
    return id;
}

sg::blas_handle const& mesh_manager::placeholder_blas()
{
    if (_placeholder_blas != nullptr)
        return _placeholder_blas;

    // The unit cube as a raw triangle list: 12 triangles over the corners of [0,1]^3, wound outward.
    // Spelled out rather than generated, because it is read once and a loop over face tables would be longer.
    constexpr tg::pos3f c[8] = {tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(1, 1, 0), tg::pos3f(0, 1, 0),
                                tg::pos3f(0, 0, 1), tg::pos3f(1, 0, 1), tg::pos3f(1, 1, 1), tg::pos3f(0, 1, 1)};
    constexpr int quads[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {2, 3, 7, 6}, {1, 2, 6, 5}, {0, 4, 7, 3}};

    auto positions = cc::vector<tg::pos3f>();
    positions.reserve(36);
    for (auto const& q : quads)
        for (auto const i : {0, 1, 2, 0, 2, 3})
            positions.push_back(c[q[i]]);

    _placeholder_vertices = _ctx.persistent.create_buffer<tg::pos3f>(positions.size(), geometry_usage);

    // Two lists so the build sees the upload finished, for the same reason an acquire uses two.
    auto up = _ctx.create_command_list();
    up->upload.data_to_buffer(_placeholder_vertices, positions);
    (void)_acquire_index_stand_in(*up); // the placeholder is non-indexed, and still has to bind something
    _ctx.submit_command_list(cc::move(up));

    auto build = _ctx.create_command_list();
    _placeholder_blas
        = build->raytracing.build_blas({{.vertices = _placeholder_vertices.raw(), .vertex_count = positions.size()}});
    _ctx.submit_command_list(cc::move(build));

    return _placeholder_blas;
}

sg::buffer<tg::pos3f> const& mesh_manager::placeholder_vertices()
{
    (void)placeholder_blas();
    return _placeholder_vertices;
}

sg::buffer<u32> const& mesh_manager::index_stand_in()
{
    (void)placeholder_blas();
    return _index_stand_in;
}

mesh_id mesh_manager::acquire(indexed_triangle_data const& mesh)
{
    if (auto const id = find_by_hash(mesh.hash); id.has_value())
        return id.value();

    auto const positions = mesh.positions.span();
    auto const indices = mesh.indices.span();
    CC_ASSERT(!indices.empty() && indices.size() % 3 == 0, "mesh_manager::acquire expects an index count that is a "
                                                           "non-zero multiple of 3");
    CC_ASSERT(!positions.empty(), "mesh_manager::acquire needs at least one vertex");

    auto vertices = _ctx.persistent.create_buffer<tg::pos3f>(positions.size(), geometry_usage);
    auto index_buffer = _ctx.persistent.create_buffer<u32>(indices.size(), geometry_usage);

    auto const size_in_bytes = vertices.size_in_bytes() + index_buffer.size_in_bytes();
    auto const id = insert(mesh.hash,
                           {.state = residency::pending,
                            .vertices = cc::move(vertices),
                            .indices = cc::move(index_buffer),
                            .is_indexed = true,
                            .triangle_count = indices.size() / 3,
                            .bounds = mesh.bounds},
                           size_in_bytes);

    _pending.push_back({.id = id, .positions = mesh.positions, .indices = mesh.indices, .bytes = size_in_bytes});
    return id;
}

isize mesh_manager::record_uploads(isize max_bytes)
{
    if (_pending.empty())
        return 0;

    auto spent = isize(0);
    auto keep = cc::vector<pending_mesh>();

    for (auto& w : _pending)
    {
        auto* const record = mutable_record(w.id);
        if (record == nullptr)
            continue; // evicted since it was queued, so nobody is waiting on it any more

        // The first request always runs whatever its size: half a vertex buffer would build a BLAS over a hole, so a
        // request is never split, and a budget smaller than one mesh still drains one mesh per epoch.
        if (max_bytes > 0 && spent > 0 && spent + w.bytes > max_bytes)
        {
            keep.push_back(cc::move(w));
            continue;
        }

        // Two lists so the build sees the upload finished; both submit in order on the direct queue, so a later trace
        // that references this BLAS is correctly ordered after.
        auto up = _ctx.create_command_list();
        up->upload.data_to_buffer(record->vertices, w.positions.span());
        if (record->is_indexed)
            up->upload.data_to_buffer(record->indices, w.indices.span());
        _ctx.submit_command_list(cc::move(up));

        auto build = _ctx.create_command_list();
        record->blas = record->is_indexed
                         ? build->raytracing.build_blas({{.vertices = record->vertices.raw(),
                                                          .vertex_count = record->vertices.element_count(),
                                                          .indices = record->indices.raw(),
                                                          .index_count = record->indices.element_count()}})
                         : build->raytracing.build_blas(
                               {{.vertices = record->vertices.raw(), .vertex_count = record->vertices.element_count()}});
        _ctx.submit_command_list(cc::move(build));

        record->state = residency::complete;
        spent += w.bytes;

        // Only now is the acceleration structure's own cost known, which is the one thing a record cannot size at insert.
        add_bytes(w.id, record->blas->size_in_bytes());
    }

    _pending = cc::move(keep);
    return spent;
}

sg::buffer<u32> mesh_manager::_acquire_index_stand_in(sg::command_list& cmd)
{
    if (_index_stand_in.raw() == nullptr)
    {
        // One triangle's worth, zero-filled so the resource is defined rather than merely allocated.
        u32 const zeros[] = {0, 0, 0};
        _index_stand_in
            = _ctx.persistent.create_buffer<u32>(3, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
        cmd.upload.data_to_buffer(_index_stand_in, zeros);
    }
    return _index_stand_in;
}

material_manager material_manager::create(sg::context& ctx, manager_config const& cfg)
{
    auto manager = material_manager(ctx);
    manager.set_limits(cfg.budget.max_bytes, cfg.budget.max_idle_epochs);
    return manager;
}

material_set_id material_manager::acquire(material_data const& materials)
{
    if (auto const id = find_by_hash(materials.hash); id.has_value())
        return id.value();

    auto const mats = materials.materials.span();
    CC_ASSERT(!mats.empty(), "material_manager::acquire needs at least one material");

    auto gpu = cc::vector<pbr_material_gpu>();
    gpu.reserve(mats.size());
    for (auto const& m : mats)
        gpu.push_back(pbr_material_gpu::from(m));

    auto buffer = _ctx.persistent.create_buffer<pbr_material_gpu>(
        mats.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);

    auto up = _ctx.create_command_list();
    up->upload.data_to_buffer(buffer, gpu);
    _ctx.submit_command_list(cc::move(up));

    auto const size_in_bytes = buffer.size_in_bytes();
    return insert(materials.hash, {.state = residency::complete, .materials = cc::move(buffer), .count = mats.size()},
                  size_in_bytes);
}

texture_manager texture_manager::create(sg::context& ctx, manager_config const& cfg)
{
    auto m = texture_manager(ctx);
    m.set_limits(cfg.budget.max_bytes, cfg.budget.max_idle_epochs);
    return m;
}

void texture_manager::mark_mips_complete(texture_id id)
{
    auto* const record = mutable_record(id);
    if (record == nullptr)
        return;
    record->uploaded_mips = record->total_mips;
    record->state = residency::complete;
}

texture_id texture_manager::acquire(texture_data const& texture)
{
    if (auto const resident = find_by_hash(texture.hash); resident.has_value())
        return resident.value();

    CC_ASSERT(texture.width > 0 && texture.height > 0, "a texture needs a positive extent");
    CC_ASSERT(texture.mip_count >= 1, "a texture carries at least its base level");

    // The full chain is allocated up front even when only the base level is supplied, so generating the rest
    // later fills this texture in place rather than replacing it.
    auto const total_mips = impl::mip_count_of(texture.width, texture.height);
    CC_ASSERT(texture.mip_count <= total_mips, "more mips supplied than the extent has");

    auto gpu = _ctx.persistent.create_texture_2d({.format = texture.format,
                                                  .width = texture.width,
                                                  .height = texture.height,
                                                  .mip_levels = total_mips,
                                                  .usage = sg::texture_usage::readonly_texture
                                                         | sg::texture_usage::readwrite_texture
                                                         | sg::texture_usage::copy_dst});

    // Every supplied level in one list, submitted before returning, so the id is usable the moment it is minted.
    auto cmd = _ctx.create_command_list();
    auto offset = isize(0);
    for (auto mip = i32(0); mip < texture.mip_count; ++mip)
    {
        auto const size = impl::mip_byte_size(texture.format, texture.width, texture.height, mip);
        CC_ASSERT(offset + size <= texture.pixels.span().size(), "texture pixels are shorter than the mips they claim");
        cmd->upload.bytes_to_texture(gpu.raw(), texture.pixels.span().subspan({.offset = offset, .size = size}),
                                     {.mip_level = mip});
        offset += size;
    }
    _ctx.submit_command_list(cc::move(cmd));

    // The budget is charged for the whole allocated chain rather than the levels supplied so far.
    // A record's byte size is fixed at insert, so `mark_mips_complete` could not revise it afterwards, and a
    // completed texture would sit in the budget at three quarters of what it costs.
    auto chain_bytes = isize(0);
    for (auto mip = i32(0); mip < total_mips; ++mip)
        chain_bytes += impl::mip_byte_size(texture.format, texture.width, texture.height, mip);

    // Whether it is done is the shape's answer, not the upload's: a texture given every level it has room for
    // needs no follow-up, one given fewer is waiting on mip generation.
    auto const state = texture.mip_count == total_mips ? residency::complete : residency::base_resident;

    return insert(
        texture.hash,
        {.texture = cc::move(gpu), .state = state, .uploaded_mips = texture.mip_count, .total_mips = total_mips},
        chain_bytes);
}

namespace
{
// readonly_buffer so a shader reads it as a ByteAddressBuffer through the bindless table; copy_dst for the upload.
constexpr auto bindless_bytes_usage = sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst;
} // namespace

attribute_manager attribute_manager::create(sg::context& ctx, manager_config const& cfg)
{
    auto manager = attribute_manager(ctx);
    manager.set_limits(cfg.budget.max_bytes, cfg.budget.max_idle_epochs);
    return manager;
}

attribute_id attribute_manager::acquire(mesh_attribute const& attribute)
{
    if (auto const resident = find_by_hash(attribute.hash); resident.has_value())
        return resident.value();

    auto const bytes = attribute.data.span();
    CC_ASSERT(!bytes.empty(), "an attribute a material reads must carry elements");

    // The buffer exists from here on, so a descriptor naming it is valid immediately; only its contents wait.
    auto data = _ctx.persistent.create_buffer<byte>(bytes.size(), bindless_bytes_usage);

    auto const id = insert(attribute.hash,
                           {.state = residency::pending,
                            .data = cc::move(data),
                            .format = attribute.format,
                            .frequency = attribute.frequency,
                            .element_count = attribute.element_count()},
                           bytes.size());

    _pending.push_back({.id = id, .data = attribute.data});
    return id;
}

isize attribute_manager::record_uploads(isize max_bytes)
{
    if (_pending.empty())
        return 0;

    auto spent = isize(0);
    auto keep = cc::vector<pending_attribute>();

    for (auto& w : _pending)
    {
        auto* const record = mutable_record(w.id);
        if (record == nullptr)
            continue;

        auto const bytes = w.data.span();
        if (max_bytes > 0 && spent > 0 && spent + bytes.size() > max_bytes)
        {
            keep.push_back(cc::move(w));
            continue;
        }

        auto up = _ctx.create_command_list();
        up->upload.data_to_buffer(record->data, bytes);
        _ctx.submit_command_list(cc::move(up));

        record->state = residency::complete;
        spent += bytes.size();
    }

    _pending = cc::move(keep);
    return spent;
}

} // namespace sv
