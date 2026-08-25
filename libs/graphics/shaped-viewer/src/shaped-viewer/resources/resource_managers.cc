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

    auto vertices = _ctx.persistent.create_buffer<tg::pos3f>(positions.size(), geometry_usage);

    // Upload the geometry, then build its BLAS.
    // Two lists so the build sees the upload finished; both submit in order on the direct queue, so a later trace that references this BLAS is correctly ordered after.
    auto up = _ctx.create_command_list();
    up->upload.data_to_buffer(vertices, positions);
    auto stand_in = _acquire_index_stand_in(*up);
    _ctx.submit_command_list(cc::move(up));

    auto build = _ctx.create_command_list();
    auto blas = build->raytracing.build_blas({{.vertices = vertices.raw(), .vertex_count = positions.size()}});
    _ctx.submit_command_list(cc::move(build));

    auto const size_in_bytes = vertices.size_in_bytes() + blas->size_in_bytes();
    return insert(mesh.hash,
                  {.vertices = cc::move(vertices),
                   .indices = cc::move(stand_in),
                   .is_indexed = false,
                   .triangle_count = positions.size() / 3,
                   .blas = cc::move(blas)},
                  size_in_bytes);
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

    auto up = _ctx.create_command_list();
    up->upload.data_to_buffer(vertices, positions);
    up->upload.data_to_buffer(index_buffer, indices);
    _ctx.submit_command_list(cc::move(up));

    auto build = _ctx.create_command_list();
    auto blas = build->raytracing.build_blas({{.vertices = vertices.raw(),
                                               .vertex_count = positions.size(),
                                               .indices = index_buffer.raw(),
                                               .index_count = indices.size()}});
    _ctx.submit_command_list(cc::move(build));

    auto const size_in_bytes = vertices.size_in_bytes() + index_buffer.size_in_bytes() + blas->size_in_bytes();
    return insert(mesh.hash,
                  {.vertices = cc::move(vertices),
                   .indices = cc::move(index_buffer),
                   .is_indexed = true,
                   .triangle_count = indices.size() / 3,
                   .blas = cc::move(blas)},
                  size_in_bytes);
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
    return insert(materials.hash, {.materials = cc::move(buffer), .count = mats.size()}, size_in_bytes);
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
    auto uploaded_bytes = isize(0);
    for (auto mip = i32(0); mip < texture.mip_count; ++mip)
    {
        auto const size = impl::mip_byte_size(texture.format, texture.width, texture.height, mip);
        CC_ASSERT(offset + size <= texture.pixels.span().size(), "texture pixels are shorter than the mips they claim");
        cmd->upload.bytes_to_texture(gpu.raw(), texture.pixels.span().subspan({.offset = offset, .size = size}),
                                     {.mip_level = mip});
        offset += size;
        uploaded_bytes += size;
    }
    _ctx.submit_command_list(cc::move(cmd));

    // Whether it is done is the shape's answer, not the upload's: a texture given every level it has room for
    // needs no follow-up, one given fewer is waiting on mip generation.
    auto const state = texture.mip_count == total_mips ? residency::complete : residency::base_resident;

    return insert(
        texture.hash,
        {.texture = cc::move(gpu), .state = state, .uploaded_mips = texture.mip_count, .total_mips = total_mips},
        uploaded_bytes);
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

    auto data = _ctx.persistent.create_buffer<byte>(bytes.size(), bindless_bytes_usage);

    auto up = _ctx.create_command_list();
    up->upload.data_to_buffer(data, bytes);
    _ctx.submit_command_list(cc::move(up));

    return insert(attribute.hash,
                  {.data = cc::move(data),
                   .format = attribute.format,
                   .frequency = attribute.frequency,
                   .element_count = attribute.element_count()},
                  bytes.size());
}

} // namespace sv
