#include <clean-core/common/assert.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/impl/content_hash.hh>
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

namespace
{
/// The attribute named `name`, or null when the mesh does not carry it.
[[nodiscard]] mesh_attribute const* find_attribute(cc::span<mesh_attribute const> attributes, cc::string_view name)
{
    for (auto const& a : attributes)
        if (a.name == name)
            return &a;
    return nullptr;
}

/// `a`'s elements as T, checked against what the attribute is supposed to carry; empty when the mesh has no such attribute.
template <class T>
[[nodiscard]] cc::span<T const> pbr_elements(mesh_attribute const* a, isize triangle_count)
{
    if (a == nullptr)
        return {};

    CC_ASSERT(a->frequency == attribute_frequency::per_triangle, "a pbr material attribute must be per_triangle");
    CC_ASSERT(a->element_count() == triangle_count, "a pbr material attribute must hold one element per triangle");
    return a->elements_as<T>();
}

/// `elements[i]`, or `fallback` when the mesh carries no such attribute.
template <class T>
[[nodiscard]] T element_or(cc::span<T const> elements, isize i, T const& fallback)
{
    return elements.empty() ? fallback : elements[i];
}
} // namespace

material_set_id material_manager::acquire(cc::span<mesh_attribute const> attributes, isize triangle_count)
{
    CC_ASSERT(triangle_count > 0, "material_manager::acquire needs at least one triangle");

    auto const* const base_color = find_attribute(attributes, pbr_attribute::base_color);
    auto const* const metallic = find_attribute(attributes, pbr_attribute::metallic);
    auto const* const roughness = find_attribute(attributes, pbr_attribute::roughness);
    auto const* const emissive = find_attribute(attributes, pbr_attribute::emissive);

    // Folded in a fixed name order, so the key does not depend on how the mesh happens to list its attributes.
    // A missing attribute folds in as a zero digest, which is what makes "carries no emissive" its own set rather than
    // an alias of one that does.
    cc::hash128 const digests[] = {
        base_color != nullptr ? base_color->hash : cc::hash128{}, metallic != nullptr ? metallic->hash : cc::hash128{},
        roughness != nullptr ? roughness->hash : cc::hash128{}, emissive != nullptr ? emissive->hash : cc::hash128{}};
    auto const key = cc::hash128::create(cc::span<cc::hash128 const>(digests).as_bytes(), impl::material_hash_seed);

    if (auto const id = find_by_hash(key); id.has_value())
        return id.value();

    auto const base_colors = pbr_elements<tg::vec3f>(base_color, triangle_count);
    auto const metallics = pbr_elements<f32>(metallic, triangle_count);
    auto const roughnesses = pbr_elements<f32>(roughness, triangle_count);
    auto const emissives = pbr_elements<tg::vec3f>(emissive, triangle_count);

    auto const defaults = pbr_material{};

    auto gpu = cc::vector<pbr_material_gpu>();
    gpu.reserve(triangle_count);
    for (auto i = isize(0); i < triangle_count; ++i)
        gpu.push_back(pbr_material_gpu::from({.base_color = element_or(base_colors, i, defaults.base_color),
                                              .metallic = element_or(metallics, i, defaults.metallic),
                                              .roughness = element_or(roughnesses, i, defaults.roughness),
                                              .emissive = element_or(emissives, i, defaults.emissive)}));

    auto buffer = _ctx.persistent.create_buffer<pbr_material_gpu>(
        triangle_count, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);

    auto up = _ctx.create_command_list();
    up->upload.data_to_buffer(buffer, gpu);
    _ctx.submit_command_list(cc::move(up));

    auto const size_in_bytes = buffer.size_in_bytes();
    return insert(key, {.materials = cc::move(buffer), .count = triangle_count}, size_in_bytes);
}

scene_resources scene_resources::create(sg::context& ctx, scene_resources_config const& cfg)
{
    return scene_resources(mesh_manager::create(ctx, cfg.meshes), material_manager::create(ctx, cfg.materials),
                           texture_manager::create(ctx), bindless_manager::create(ctx, cfg.bindless));
}
} // namespace sv
