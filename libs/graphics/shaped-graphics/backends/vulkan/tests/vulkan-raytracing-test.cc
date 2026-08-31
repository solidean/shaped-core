#include "vulkan-test-common.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/vulkan/vulkan_acceleration_structure.hh>

// Embedded SPIR-V for raytrace.hlsl.
// See that file for the dxc command.
#include "raytrace.spirv.h"

using namespace cc::primitive_defines;

// Acceleration-structure builds against a real device.
//
// These sit in tier 2 for a scheduling reason rather than a technical one: the tier-1 raytracing tests gate on
// `cmd.raytracing.is_supported()`, which the vulkan command list deliberately answers false for while the trace seams
// are stubs — a list that cannot dispatch should not claim it can.
// The context's own answer is already true, so the build path is reachable and testable here, and these move to
// covering the vulkan-specific half once tier 1 runs the portable one.
//
// What is vulkan-specific and stays here: a built structure is a VkAccelerationStructureKHR object placed over the
// storage buffer, with a device address of its own, where DXR names one by the address of the storage.

namespace
{
namespace vulkan = sg::backend::vulkan;

sg::raw_buffer_handle make_triangle_vertices(sg::context& ctx)
{
    float const verts[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    auto const buf = ctx.persistent.create_raw_buffer(
        sizeof(verts), sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::copy_dst);
    auto cmd = ctx.create_command_list();
    cmd->upload.data_to_buffer(buf, cc::span<float const>(verts, 9));
    ctx.submit_command_list(cc::move(cmd));
    return buf;
}
} // namespace

TEST("sg vulkan - builds a triangle blas and a tlas over it")
{
    auto handle = vulkan::test::make_context();
    if (handle == nullptr)
        SKIP("no vulkan device");
    auto& ctx = *handle;
    if (!static_cast<vulkan::vulkan_context&>(ctx).is_raytracing_supported())
        SKIP("no ray tracing on this device");

    auto const verts = make_triangle_vertices(ctx);

    sg::blas_triangles tri;
    tri.vertices = verts;
    tri.vertex_count = 3;

    // Both builds in one list, which is what exercises the intra-list accel_write -> accel_read ordering.
    auto cmd = ctx.create_command_list();
    auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_triangles const>(&tri, 1));
    REQUIRE(blas != nullptr);

    sg::tlas_instance inst;
    inst.blas = blas;
    inst.instance_id = 7;
    auto const tlas = cmd->raytracing.build_tlas(cc::span<sg::tlas_instance const>(&inst, 1));
    REQUIRE(tlas != nullptr);
    ctx.submit_command_list(cc::move(cmd));

    CHECK(blas->size_in_bytes() > 0);
    CHECK(blas->geometry_count() == 1);
    CHECK(tlas->size_in_bytes() > 0);
    CHECK(tlas->instance_count() == 1);

    // The vulkan-specific half: each structure is an object with an address, which is what a TLAS instance and a
    // trace actually name.
    auto const& vk_blas = static_cast<vulkan::vulkan_blas const&>(*blas);
    auto const& vk_tlas = static_cast<vulkan::vulkan_tlas const&>(*tlas);
    CHECK(vk_blas._accel != VK_NULL_HANDLE);
    CHECK(vk_blas._address != 0);
    CHECK(vk_tlas._accel != VK_NULL_HANDLE);
    CHECK(vk_tlas._address != 0);

    // Persistent: the handles outlive the epoch that built them, and the validation listener is what says the builds
    // themselves were well-formed.
    ctx.advance_epoch_and_wait_for_idle();
    CHECK(!blas->is_expired());
    CHECK(!tlas->is_expired());
}

TEST("sg vulkan - builds a procedural (aabb) blas")
{
    auto handle = vulkan::test::make_context();
    if (handle == nullptr)
        SKIP("no vulkan device");
    auto& ctx = *handle;
    if (!static_cast<vulkan::vulkan_context&>(ctx).is_raytracing_supported())
        SKIP("no ray tracing on this device");

    float const aabb[6] = {0, 0, 0, 1, 1, 1};
    auto const buf = ctx.persistent.create_raw_buffer(
        sizeof(aabb), sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::copy_dst);
    {
        auto up = ctx.create_command_list();
        up->upload.data_to_buffer(buf, cc::span<float const>(aabb, 6));
        ctx.submit_command_list(cc::move(up));
    }

    sg::blas_aabbs geo;
    geo.aabbs = buf;
    geo.aabb_count = 1;

    auto cmd = ctx.create_command_list();
    auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_aabbs const>(&geo, 1));
    REQUIRE(blas != nullptr);
    ctx.submit_command_list(cc::move(cmd));

    CHECK(blas->size_in_bytes() > 0);
    CHECK(blas->geometry_count() == 1);
    ctx.advance_epoch_and_wait_for_idle();
    CHECK(!blas->is_expired());
}

// The whole trace path end to end: a pipeline of three shader groups, a shader table over them, and a dispatch whose
// results distinguish a hit from a miss per ray.
//
// The alternating rays are what make the readback meaningful: a backend that wrote a constant, traced against an
// empty scene, or mixed up the miss and hit groups would all produce a uniform buffer.
TEST("sg vulkan - traces rays against a tlas")
{
    auto handle = vulkan::test::make_context();
    if (handle == nullptr)
        SKIP("no vulkan device");
    auto& ctx = *handle;
    if (!static_cast<vulkan::vulkan_context&>(ctx).is_raytracing_supported())
        SKIP("no ray tracing on this device");

    constexpr int k_rays = 64;

    auto const verts = make_triangle_vertices(ctx);
    auto const output = ctx.persistent.create_raw_buffer(
        isize(k_rays) * isize(sizeof(u32)), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    REQUIRE(output != nullptr);

    // Build the scene.
    sg::blas_triangles tri;
    tri.vertices = verts;
    tri.vertex_count = 3;

    sg::tlas_handle tlas;
    {
        auto cmd = ctx.create_command_list();
        auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_triangles const>(&tri, 1));
        sg::tlas_instance inst;
        inst.blas = blas;
        tlas = cmd->raytracing.build_tlas(cc::span<sg::tlas_instance const>(&inst, 1));
        ctx.submit_command_list(cc::move(cmd));
    }
    REQUIRE(tlas != nullptr);

    // One SPIR-V module with three entry points is what a shader library is, so all three stages share bytecode.
    auto const module_bytes
        = cc::span<byte const>(reinterpret_cast<byte const*>(raytrace_spirv), isize(sizeof(raytrace_spirv)));
    auto const make_rt_shader = [&](sg::shader_stage stage, cc::string_view entry)
    {
        sg::compiled_shader s;
        s.stage = stage;
        s.format = sg::shader_format::spirv;
        s.entry_point = entry;
        s.bytecode = cc::make_pinned_data(module_bytes);
        return s;
    };

    auto bindings = cc::vector<sg::binding>{
        {.name = "Scene", .group_index = 0, .index = 0, .count = 1, .type = sg::binding_type::acceleration_structure},
        {.name = "Output", .group_index = 0, .index = 1, .count = 1, .type = sg::binding_type::readwrite_structured_buffer}};
    auto group_layout = ctx.cached.acquire_binding_group_layout(bindings);
    auto pipeline_layout = ctx.cached.acquire_pipeline_layout(sg::pipeline_layout_description{.groups = {group_layout}});

    sg::raytracing_pipeline_description desc;
    desc.layout = pipeline_layout;
    auto const raygen = desc.add_raygen_shader(make_rt_shader(sg::shader_stage::raygen, "rgen"));
    auto const miss = desc.add_miss_shader(make_rt_shader(sg::shader_stage::miss, "rmiss"));
    auto const hit = desc.add_hit_shader({.closest_hit = make_rt_shader(sg::shader_stage::closest_hit, "rchit")});

    auto pipeline = ctx.uncached.create_raytracing_pipeline(desc);
    REQUIRE(pipeline != nullptr);

    sg::raytracing_shader_table_description table_desc;
    table_desc.pipeline = pipeline;
    auto const raygen_index = table_desc.add_raygen_shader(raygen);
    (void)table_desc.add_miss_shader(miss);
    (void)table_desc.add_hit_shader(hit);
    auto table = ctx.uncached.create_raytracing_shader_table(table_desc);
    REQUIRE(table != nullptr);

    sg::named_view const views[] = {{.name = "Scene", .view = tlas->as_view()},
                                    {.name = "Output", .view = sg::buffer<u32>::from_raw(output).as_readwrite_buffer()}};
    auto group = ctx.persistent.create_binding_group(group_layout, views);
    REQUIRE(group != nullptr);

    auto cmd = ctx.create_command_list();
    cmd->raytracing.bind_pipeline(*pipeline);
    cmd->raytracing.bind_group(0, *group);
    cmd->raytracing.dispatch_rays(*table, raygen_index, k_rays);
    ctx.submit_command_list(cc::move(cmd));

    auto down = ctx.create_command_list();
    auto future = down->download.data_from_buffer<u32>(output, 0, k_rays);
    ctx.submit_command_list(cc::move(down));

    auto const data = ctx.wait_for(future);
    REQUIRE(data.has_value());
    REQUIRE(data.value().size() == isize(k_rays));

    bool alternating = true;
    for (int i = 0; i < k_rays; ++i)
        if (data.value()[i] != u32(i % 2 == 0 ? 1 : 0))
            alternating = false;
    CHECK(alternating);
}
