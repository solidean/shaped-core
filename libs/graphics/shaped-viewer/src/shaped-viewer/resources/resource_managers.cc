#include <clean-core/common/assert.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async.hh>
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

namespace impl
{
// What the streaming actor orders our transfers by; higher runs first.
//
// Attributes outrank the geometry that indexes them, and that ordering is a correctness argument rather than a
// preference: a mesh becomes drawable the moment its BLAS is built, and if its uv set were still in flight then it
// would draw its real triangles against zeroed attribute bytes for a frame or two.
// Geometry outranks textures for the reason the design gives: a grey model beats a floating albedo map.
constexpr i32 attribute_stream_priority = 20;
constexpr i32 geometry_stream_priority = 10;
} // namespace impl

mesh_id mesh_manager::acquire(triangle_data const& mesh)
{
    if (auto const id = find_by_hash(mesh.hash); id.has_value())
        return id.value();

    auto const positions = mesh.positions.span();
    CC_ASSERT(!positions.empty() && positions.size() % 3 == 0, "mesh_manager::acquire expects a triangle list (vertex "
                                                               "count a non-zero multiple of 3)");

    // The BUFFER is created here and the bytes are not: the payload goes to `ctx.stream`, which carries it on the
    // copy queue rather than making the next command list that touches this buffer wait on it.
    // That is the whole difference from `ctx.upload`, and the reason a big asset does not stall the frame that asked
    // for it — see shaped-graphics/transfer/stream.hh.
    auto vertices = _ctx.persistent.create_buffer<tg::pos3f>(positions.size(), geometry_usage);

    auto up = _ctx.create_command_list();
    auto stand_in = _acquire_index_stand_in(*up);
    _ctx.submit_command_list(cc::move(up));

    // The BLAS is not charged yet, since it is not built: `record_settled` adds its size once it is.
    auto const size_in_bytes = vertices.size_in_bytes();
    auto transfer = _ctx.stream.data_to_buffer(vertices, mesh.positions);
    transfer.set_priority(impl::geometry_stream_priority);

    auto const id = insert(mesh.hash,
                           {.state = residency::pending,
                            .vertices = cc::move(vertices),
                            .indices = cc::move(stand_in),
                            .is_indexed = false,
                            .triangle_count = positions.size() / 3,
                            .bounds = mesh.bounds},
                           size_in_bytes);

    _settling[id] = {.vertices = cc::move(transfer), .bytes = size_in_bytes};
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

    auto vertex_transfer = _ctx.stream.data_to_buffer(vertices, mesh.positions);
    auto index_transfer = _ctx.stream.data_to_buffer(index_buffer, mesh.indices);
    vertex_transfer.set_priority(impl::geometry_stream_priority);
    index_transfer.set_priority(impl::geometry_stream_priority);

    auto const id = insert(mesh.hash,
                           {.state = residency::pending,
                            .vertices = cc::move(vertices),
                            .indices = cc::move(index_buffer),
                            .is_indexed = true,
                            .triangle_count = indices.size() / 3,
                            .bounds = mesh.bounds},
                           size_in_bytes);

    _settling[id] = {.vertices = cc::move(vertex_transfer), .indices = cc::move(index_transfer), .bytes = size_in_bytes};
    return id;
}

bool mesh_manager::_try_settle(sg::command_list& cmd, mesh_id id, pending_mesh& p)
{
    auto const vertices_done = p.vertices.is_settled();
    auto const indices_done = !p.indices.is_valid() || p.indices.is_settled();
    if (!vertices_done || !indices_done)
        return false;

    auto* const record = mutable_record(id);
    if (record == nullptr)
        return true; // evicted while streaming; dropping the entry cancels whatever is left

    // Settled without delivering — cancelled, or the transfer failed.
    // `failed` rather than leaving it pending, so nothing waits on it forever and the placeholder is known to be final.
    if (!p.vertices.is_complete() || (p.indices.is_valid() && !p.indices.is_complete()))
    {
        record->state = residency::failed;
        return true;
    }

    // Recorded only now that completion has been observed, which is what the streaming contract asks: a list touching
    // a streamed extent must be SUBMITTED after that observation, and `cmd` is the caller's to submit after this.
    record->blas = record->is_indexed ? cmd.raytracing.build_blas({{.vertices = record->vertices.raw(),
                                                                    .vertex_count = record->vertices.element_count(),
                                                                    .indices = record->indices.raw(),
                                                                    .index_count = record->indices.element_count()}})
                                      : cmd.raytracing.build_blas({{.vertices = record->vertices.raw(),
                                                                    .vertex_count = record->vertices.element_count()}});
    record->state = residency::complete;

    // Only now is the acceleration structure's own cost known, which is the one thing a record cannot size at insert.
    add_bytes(id, record->blas->size_in_bytes());
    return true;
}

isize mesh_manager::record_settled(sg::command_list& cmd)
{
    if (_settling.empty())
        return 0;

    auto finished = isize(0);
    auto keep = cc::map<mesh_id, pending_mesh>();
    for (auto&& [id, p] : _settling)
    {
        if (_try_settle(cmd, id, p))
            ++finished;
        else
            keep[id] = cc::move(p);
    }

    _settling = cc::move(keep);
    return finished;
}

void mesh_manager::wait_for_settled()
{
    if (_settling.empty())
        return;

    // Promoted first: with the automatic waits back on, a list recorded after this needs no ordering of its own.
    for (auto&& [id, p] : _settling)
    {
        (void)id;
        p.vertices.promote_to_async();
        if (p.indices.is_valid())
            p.indices.promote_to_async();
        (void)cc::try_async_blocking_get(p.vertices.completion());
        if (p.indices.is_valid())
            (void)cc::try_async_blocking_get(p.indices.completion());
    }

    auto cmd = _ctx.create_command_list();
    (void)record_settled(*cmd);
    _ctx.submit_command_list(cc::move(cmd));
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

    auto transfer = _ctx.stream.bytes_to_buffer(data.raw(), attribute.data);
    transfer.set_priority(impl::attribute_stream_priority);

    auto const id = insert(attribute.hash,
                           {.state = residency::pending,
                            .data = cc::move(data),
                            .format = attribute.format,
                            .frequency = attribute.frequency,
                            .element_count = attribute.element_count()},
                           bytes.size());

    _settling[id] = cc::move(transfer);
    return id;
}

isize attribute_manager::collect_settled()
{
    if (_settling.empty())
        return 0;

    auto finished = isize(0);
    auto keep = cc::map<attribute_id, sg::stream_upload_handle>();
    for (auto&& [id, transfer] : _settling)
    {
        if (!transfer.is_settled())
        {
            keep[id] = cc::move(transfer);
            continue;
        }

        if (auto* const record = mutable_record(id); record != nullptr)
            record->state = transfer.is_complete() ? residency::complete : residency::failed;
        ++finished;
    }

    _settling = cc::move(keep);
    return finished;
}

void attribute_manager::wait_for_settled()
{
    for (auto&& [id, transfer] : _settling)
    {
        (void)id;
        transfer.promote_to_async();
        (void)cc::try_async_blocking_get(transfer.completion());
    }
    (void)collect_settled();
}

} // namespace sv
