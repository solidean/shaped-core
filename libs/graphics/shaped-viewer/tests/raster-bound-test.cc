#include "viewer_test_env.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // dx12_config::withhold_binding_arrays
#include <shaped-viewer/all.hh>

// The bound raster mode's ground, on a device as WebGPU's core describes one: no binding arrays and no ray tracing.
//
// A dx12 context that withholds both is how a host whose GPU has them reaches that device, so the suite runs the
// non-bindless path natively, every run, without a browser.
// libs/graphics/shaped-viewer/docs/plans/raster-bound.md is the plan.

namespace
{
using namespace cc::primitive_defines;

/// A dx12 context reporting neither `binding_arrays` nor ray tracing, or an error where there is no dx12 device.
[[nodiscard]] cc::result<sg::context_handle> create_webgpu_like_context()
{
    // The debug layer has to be active before the process's first dx12 device, as dx12-entry.cc's drivers ask for it.
    return sg::create_dx12_context({.activate_global_debug_layer = true,
                                    .adapter = sg::backend::dx12::dx12_adapter::hardware_or_warp,
                                    .withhold_raytracing = true,
                                    .withhold_binding_arrays = true});
}
} // namespace

ASYNC_TEST("sv raster_bound - a resource manager on a device without binding arrays keeps its pools and no tables")
{
    auto created = create_webgpu_like_context();
    if (created.has_error())
        SKIP("no dx12 device here");
    auto const ctx_h = cc::move(created.value());
    auto& ctx = *ctx_h;
    REQUIRE(!ctx.supports(sg::feature::binding_arrays));

    auto m = sv::gpu_resource_manager::create(ctx);
    m.advance_to(ctx.current_epoch());
    CHECK(!m.has_bindless_tables());

    auto lib = sv::material_library::create();
    sv::register_builtin_material_types(lib);
    auto const pbr = lib.acquire_type(sv::builtin_material::pbr).value();
    auto overrides = cc::vector<sv::material_attribute_binding>();
    overrides.push_back(sv::material_attribute_binding::of("roughness", 0.25f));
    auto const material = lib.acquire(sv::material::create("rough", pbr, overrides));

    auto const positions = cc::vector<tg::pos3f>{tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(0, 1, 0)};
    auto data = sv::mesh{.name = "tri", .geometry = sv::triangle_geometry::create_from_positions(positions)};
    data.attributes.push_back(sv::mesh_attribute::create("metallic", sv::attribute_frequency::per_vertex,
                                                         cc::vector<f32>{0.25f, 0.5f, 0.75f}));
    auto const mesh = m.create_mesh(data);
    m.wait_for_pending_uploads();

    auto const resolved = sv::resolve_material(lib, material, mesh);
    auto const generated = sv::generate_material_shader(resolved);
    auto const instance = m.acquire_instance(resolved, generated.layout);

    // The record names its block and its geometry by offsets into the pools, which is all the bound mode reads.
    auto cmd = ctx.create_command_list();
    auto const record = m.describe_instance(*cmd, mesh.geometry, instance);
    CHECK(record.base_vertex == u32(m.meshes.get(mesh.geometry).geometry.base_vertex));
    CHECK(record.first_index == u32(m.meshes.get(mesh.geometry).geometry.first_index));
    auto const block_back = cmd->download.data_from_buffer(m.parameter_pool());
    ctx.submit_command_list(cc::move(cmd));

    // The block landed in the pool, and its descriptor names the attribute's range there.
    auto const pool = co_await block_back.data();
    auto const expected = m.build_instance_parameters(m.get_instance(instance));
    REQUIRE(isize(record.param_offset) + expected.size() <= pool.size());
    CHECK(cc::memcmp(pool.data() + record.param_offset, expected.data(), size_t(expected.size())) == 0);

    // A frame freezes without a bindless group to snapshot.
    {
        auto const bound = m.freeze();
        (void)bound;
    }

    ctx.advance_epoch();
    co_await ctx.idle_completion();
}
