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

mesh_id mesh_manager::acquire(cc::span<tg::pos3f const> positions)
{
    CC_ASSERT(positions.size() % 3 == 0, "mesh_manager::acquire expects a triangle list (vertex count multiple of 3)");
    CC_ASSERT(!positions.empty(), "mesh_manager::acquire needs at least one triangle");

    // accel_structure_build_input for the BLAS build; readonly_buffer so the closest-hit can read the
    // positions back (as a StructuredBuffer) to recompute the flat face normal.
    auto vertices = _ctx.persistent.create_buffer<tg::pos3f>(
        positions.size(),
        sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);

    // Upload the geometry, then build its BLAS. Two lists so the build sees the upload finished; both submit
    // in order on the direct queue, so a later trace that references this BLAS is correctly ordered after.
    auto up = _ctx.create_command_list();
    up->upload.data_to_buffer(vertices, positions);
    _ctx.submit_command_list(cc::move(up));

    auto build = _ctx.create_command_list();
    auto blas = build->raytracing.build_blas({{.vertices = vertices.raw(), .vertex_count = positions.size()}});
    _ctx.submit_command_list(cc::move(build));

    auto const size_in_bytes = vertices.size_in_bytes() + blas->size_in_bytes();
    return insert({.vertices = cc::move(vertices), .triangle_count = positions.size() / 3, .blas = cc::move(blas)},
                  size_in_bytes);
}

material_manager material_manager::create(sg::context& ctx, manager_config const& cfg)
{
    auto manager = material_manager(ctx);
    manager.set_limits(cfg.budget.max_bytes, cfg.budget.max_idle_epochs);
    return manager;
}

material_set_id material_manager::acquire(cc::span<pbr_material const> materials)
{
    CC_ASSERT(!materials.empty(), "material_manager::acquire needs at least one material");

    auto gpu = cc::vector<pbr_material_gpu>();
    gpu.reserve(materials.size());
    for (auto const& m : materials)
        gpu.push_back(pbr_material_gpu::from(m));

    auto buffer = _ctx.persistent.create_buffer<pbr_material_gpu>(
        materials.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);

    auto up = _ctx.create_command_list();
    up->upload.data_to_buffer(buffer, gpu);
    _ctx.submit_command_list(cc::move(up));

    auto const size_in_bytes = buffer.size_in_bytes();
    return insert({.materials = cc::move(buffer), .count = materials.size()}, size_in_bytes);
}

scene_resources scene_resources::create(sg::context& ctx, scene_resources_config const& cfg)
{
    return scene_resources(mesh_manager::create(ctx, cfg.meshes), material_manager::create(ctx, cfg.materials),
                           texture_manager::create(ctx));
}
} // namespace sv
