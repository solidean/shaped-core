#include <clean-core/common/assert.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/resources/resource_managers.hh>

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

scene_resources scene_resources::create(sg::context& ctx, scene_resources_config const& cfg)
{
    return scene_resources(mesh_manager::create(ctx, cfg.meshes), material_manager::create(ctx, cfg.materials),
                           texture_manager::create(ctx));
}
} // namespace sv
