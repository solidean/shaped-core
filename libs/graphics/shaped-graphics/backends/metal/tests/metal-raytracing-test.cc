#include "metal-test-common.hh"
#include "raytrace.metallib.h"
#include "raytrace_second.metallib.h"

#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/metal/metal_acceleration_structure.hh>
#include <shaped-graphics/backends/metal/metal_raytracing_pipeline.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/raytracing/raytracing_shader_table.hh>

// Acceleration-structure builds and the bind path that traces against one.
//
// The tier-1 suite covers the same surface portably and is the specification; these are the metal-specific facts on
// top — that a structure is a resource rather than a buffer here, and that a tlas reaches a shader as a resource id.
//
// The two execution tests at the bottom are the only thing anywhere that traces a ray on metal, since the tier-1 suite
// cannot: bytecode is per-backend by construction.
// They run against a HAND-WRITTEN metallib, which raytrace.metal explains and marks as temporary.

namespace mtl = sg::backend::metal;
using namespace cc::primitive_defines;

namespace
{
/// One unit triangle, as the float3 positions a BLAS build takes.
constexpr float k_triangle[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};

/// One entry point out of the shared fixture library.
/// sg::compiled_shader is single-entry, so each shader takes the same bytes with a different entry point — which is
/// what makes the backend load one library per registered shader.
[[nodiscard]] sg::compiled_shader shader_from(cc::span<u8 const> library, sg::shader_stage stage, cc::string entry_point)
{
    auto shader = sg::compiled_shader{};
    shader.stage = stage;
    shader.format = sg::shader_format::metal_lib;
    shader.entry_point = cc::move(entry_point);
    auto blob = cc::pinned_data<byte>::create_uninitialized(library.size());
    cc::memcpy(blob.data(), library.data(), size_t(library.size()));
    shader.bytecode = cc::pinned_data<byte const>(cc::move(blob));
    shader.workgroup_size = sg::compute_dimensions{.x = 1, .y = 1, .z = 1};
    return shader;
}

[[nodiscard]] sg::compiled_shader fixture_shader(sg::shader_stage stage, cc::string entry_point)
{
    return shader_from(mtl::test::raytrace_metallib, stage, cc::move(entry_point));
}

/// The binding shape raytrace.metal declares, written by hand so the test states what it means.
[[nodiscard]] cc::vector<sg::binding> scene_bindings()
{
    auto bindings = cc::vector<sg::binding>();
    bindings.push_back(
        {.name = "scene", .space = 0, .index = 0, .count = 1, .type = sg::binding_type::acceleration_structure});
    bindings.push_back(
        {.name = "out", .space = 0, .index = 1, .count = 1, .type = sg::binding_type::readwrite_structured_buffer});
    return bindings;
}

[[nodiscard]] sg::raw_buffer_handle make_vertex_buffer(sg::context_handle const& ctx)
{
    auto const buffer = ctx->persistent.create_raw_buffer(
        isize(sizeof(k_triangle)), sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::copy_dst);
    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_buffer(buffer, cc::as_bytes(cc::span<float const>(k_triangle)));
    ctx->submit_command_list(cc::move(cmd));
    return buffer;
}
} // namespace

TEST("sg metal - a triangle BLAS builds and reports its sizes")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const vertices = make_vertex_buffer(ctx);

    auto cmd = ctx->create_command_list();
    REQUIRE(cmd->raytracing.is_supported());

    auto const geometry = sg::blas_triangles{.vertices = vertices, .vertex_count = 3};
    auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_triangles const>(&geometry, 1));
    REQUIRE(blas != nullptr);
    ctx->submit_command_list(cc::move(cmd));

    // Metal answers all three sizes from one query, refit scratch included — long before anything refits.
    CHECK(blas->size_in_bytes() > 0);
    CHECK(blas->build_scratch_size_in_bytes() > 0);
    CHECK(blas->geometry_count() == 1);
    CHECK(!blas->is_expired());

    // The metal-specific half: the structure is an MTLAccelerationStructure, not a buffer, and it is named by the
    // resource id an instance descriptor and an argument buffer both carry.
    auto const& mtl_blas = static_cast<mtl::metal_blas const&>(*blas);
    CHECK(mtl_blas.storage().accel() != nullptr);
    CHECK(mtl_blas.storage().resource_id()._impl != 0);
}

TEST("sg metal - a procedural BLAS builds from AABBs")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    constexpr float k_aabb[6] = {0, 0, 0, 1, 1, 1};
    auto const buffer = ctx->persistent.create_raw_buffer(
        isize(sizeof(k_aabb)), sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::copy_dst);
    auto upload = ctx->create_command_list();
    upload->upload.bytes_to_buffer(buffer, cc::as_bytes(cc::span<float const>(k_aabb)));
    ctx->submit_command_list(cc::move(upload));

    auto cmd = ctx->create_command_list();
    auto const geometry = sg::blas_aabbs{.aabbs = buffer, .aabb_count = 1};
    auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_aabbs const>(&geometry, 1));
    REQUIRE(blas != nullptr);
    ctx->submit_command_list(cc::move(cmd));

    CHECK(blas->size_in_bytes() > 0);
    CHECK(!blas->is_expired());
}

ASYNC_TEST("sg metal - a TLAS builds over an instance and keeps its BLAS alive")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const vertices = make_vertex_buffer(ctx);

    auto cmd = ctx->create_command_list();
    auto const geometry = sg::blas_triangles{.vertices = vertices, .vertex_count = 3};
    auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_triangles const>(&geometry, 1));
    REQUIRE(blas != nullptr);

    auto const instance = sg::tlas_instance{.blas = blas, .instance_id = 7};
    auto const tlas = cmd->raytracing.build_tlas(cc::span<sg::tlas_instance const>(&instance, 1));
    REQUIRE(tlas != nullptr);
    ctx->submit_command_list(cc::move(cmd));
    co_await ctx->idle_completion();

    CHECK(tlas->size_in_bytes() > 0);
    CHECK(tlas->instance_count() == 1);
    CHECK(!tlas->is_expired());

    auto const& mtl_tlas = static_cast<mtl::metal_tlas const&>(*tlas);
    CHECK(mtl_tlas.storage().accel() != nullptr);
}

ASYNC_TEST("sg metal - a binding group encodes a tlas as a resource id")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const vertices = make_vertex_buffer(ctx);

    auto cmd = ctx->create_command_list();
    auto const geometry = sg::blas_triangles{.vertices = vertices, .vertex_count = 3};
    auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_triangles const>(&geometry, 1));
    auto const instance = sg::tlas_instance{.blas = blas};
    auto const tlas = cmd->raytracing.build_tlas(cc::span<sg::tlas_instance const>(&instance, 1));
    REQUIRE(tlas != nullptr);
    ctx->submit_command_list(cc::move(cmd));
    co_await ctx->idle_completion();

    auto const b = sg::binding{
        .name = "Scene",
        .space = 0,
        .index = 0,
        .count = 1,
        .type = sg::binding_type::acceleration_structure,
    };
    auto layout
        = ctx->create_metal_binding_group_layout(cc::span<sg::binding const>(&b, 1), {}, sg::lifetime_scope::persistent);
    REQUIRE(layout.has_value()).context(layout.has_error() ? layout.error().to_string() : cc::string());

    auto const view = sg::named_view{.name = "Scene", .view = tlas->as_view()};
    auto group = ctx->create_metal_binding_group(layout.value(), cc::span<sg::named_view const>(&view, 1), {},
                                                 sg::lifetime_scope::persistent);
    REQUIRE(group.has_value()).context(group.has_error() ? group.error().to_string() : cc::string());

    // The group holds the structure: an argument buffer is raw ids, so nothing else keeps the target alive.
    REQUIRE(group.value()->bound_tlases().size() == 1);
    CHECK(group.value()->bound_tlases()[0] == tlas);
}

// ---------------------------------------------------------------------------------------------------------------
// Execution.
//
// The fixture dispatches two threads: thread 0 aims at the geometry and thread 1 aims past it, so every test below
// covers the hit path and the miss path at once and the two cannot be confused.
// A hit reports its distance, a miss reports -1, and a payload nothing wrote stays 0 — three distinguishable numbers,
// so "the wrong function ran" and "no function ran" are different failures.
namespace
{
constexpr float k_triangle_distance = 1.0f;
constexpr float k_procedural_distance = 3.0f; // what procedural_hit reports, which is not the AABB's own entry distance
constexpr float k_miss = -1.0f;
constexpr auto k_thread_count = isize(2);

/// The scene a trace runs against, plus the buffer the answers land in.
struct traceable_scene
{
    sg::tlas_handle tlas;
    sg::raw_buffer_handle out;
    sg::binding_group_layout_handle layout;
    sg::pipeline_layout_handle pipeline_layout;
};

[[nodiscard]] traceable_scene finish_scene(mtl::metal_context_handle const& ctx, sg::tlas_handle tlas)
{
    auto const out = ctx->persistent.create_raw_buffer(k_thread_count * isize(sizeof(float)),
                                                       sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto const bindings = scene_bindings();
    auto layout = ctx->create_metal_binding_group_layout(bindings, {}, sg::lifetime_scope::persistent);
    REQUIRE(layout.has_value()).context(layout.has_error() ? layout.error().to_string() : cc::string());

    auto pipeline_layout_desc = sg::pipeline_layout_description{};
    pipeline_layout_desc.groups.push_back(layout.value());
    auto pipeline_layout = ctx->create_metal_pipeline_layout(pipeline_layout_desc, sg::lifetime_scope::persistent);
    REQUIRE(pipeline_layout.has_value());

    return {.tlas = cc::move(tlas), .out = out, .layout = layout.value(), .pipeline_layout = pipeline_layout.value()};
}

/// A one-triangle scene.
/// `is_opaque` is what decides whether an any-hit function is consulted at all — traversal skips it on opaque geometry.
[[nodiscard]] traceable_scene make_triangle_scene(mtl::metal_context_handle const& ctx, bool is_opaque)
{
    auto const vertices = make_vertex_buffer(ctx);

    auto cmd = ctx->create_command_list();
    auto const geometry = sg::blas_triangles{.vertices = vertices, .vertex_count = 3, .is_opaque = is_opaque};
    auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_triangles const>(&geometry, 1));
    auto const instance = sg::tlas_instance{.blas = blas};
    auto const tlas = cmd->raytracing.build_tlas(cc::span<sg::tlas_instance const>(&instance, 1));
    ctx->submit_command_list(cc::move(cmd));

    return finish_scene(ctx, tlas);
}

/// A one-AABB scene, whose contents only an intersection function can describe.
[[nodiscard]] traceable_scene make_procedural_scene(mtl::metal_context_handle const& ctx)
{
    constexpr float k_box[6] = {0, 0, 0, 1, 1, 1};
    auto const buffer = ctx->persistent.create_raw_buffer(
        isize(sizeof(k_box)), sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::copy_dst);
    auto upload = ctx->create_command_list();
    upload->upload.bytes_to_buffer(buffer, cc::as_bytes(cc::span<float const>(k_box)));
    ctx->submit_command_list(cc::move(upload));

    auto cmd = ctx->create_command_list();
    auto const geometry = sg::blas_aabbs{.aabbs = buffer, .aabb_count = 1, .is_opaque = false};
    auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_aabbs const>(&geometry, 1));
    auto const instance = sg::tlas_instance{.blas = blas};
    auto const tlas = cmd->raytracing.build_tlas(cc::span<sg::tlas_instance const>(&instance, 1));
    ctx->submit_command_list(cc::move(cmd));

    return finish_scene(ctx, tlas);
}

[[nodiscard]] sg::binding_group_handle bind_scene(mtl::metal_context_handle const& ctx, traceable_scene const& scene)
{
    sg::named_view views[2] = {
        {.name = "scene", .view = scene.tlas->as_view()},
        {.name = "out",
         .view = scene.out->as_raw_readwrite({.offset = 0, .size = scene.out->size_in_bytes()}, isize(sizeof(float)))},
    };
    auto group = ctx->create_metal_binding_group(scene.layout, views, {}, sg::lifetime_scope::persistent);
    REQUIRE(group.has_value()).context(group.has_error() ? group.error().to_string() : cc::string());
    return group.value();
}

/// Record one raygen entry point through the pipeline path and submit it.
/// `hit` is the group under test, which is what makes each caller below differ by one shader.
/// The wait is the caller's, because awaiting belongs in the ASYNC_TEST body rather than in a helper.
[[nodiscard]] sg::bytes_future trace_with_pipeline(mtl::metal_context_handle const& ctx,
                                                   traceable_scene const& scene,
                                                   cc::string raygen_entry,
                                                   sg::hit_shader hit)
{
    auto const group = bind_scene(ctx, scene);

    auto desc = sg::raytracing_pipeline_description{.layout = scene.pipeline_layout};
    auto const raygen = desc.add_raygen_shader(fixture_shader(sg::shader_stage::raygen, cc::move(raygen_entry)));
    auto const miss = desc.add_miss_shader(fixture_shader(sg::shader_stage::miss, "miss_marker"));
    auto const hit_handle = desc.add_hit_shader(cc::move(hit));

    auto pipeline = ctx->create_metal_raytracing_pipeline(desc, sg::lifetime_scope::persistent);
    REQUIRE(pipeline.has_value()).context(pipeline.has_error() ? pipeline.error().to_string() : cc::string());

    auto table_desc = sg::raytracing_shader_table_description{.pipeline = pipeline.value()};
    auto const raygen_slot = table_desc.add_raygen_shader(raygen);
    (void)table_desc.add_miss_shader(miss);
    (void)table_desc.add_hit_shader(hit_handle);

    auto table = ctx->create_metal_raytracing_shader_table(table_desc, sg::lifetime_scope::persistent);
    REQUIRE(table.has_value()).context(table.has_error() ? table.error().to_string() : cc::string());

    auto cmd = ctx->create_command_list();
    cmd->raytracing.bind_pipeline(*pipeline.value());
    cmd->raytracing.bind_group(0, *group);
    cmd->raytracing.dispatch_rays(*table.value(), raygen_slot, int(k_thread_count), 1, 1);
    auto future = cmd->download.bytes_from_buffer(scene.out, 0, scene.out->size_in_bytes());
    ctx->submit_command_list(cc::move(cmd));
    return future;
}

/// The two floats a traced dispatch wrote, once the caller has drained.
[[nodiscard]] cc::vector<float> traced_values(sg::bytes_future& future)
{
    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    auto const* const values = reinterpret_cast<float const*>(bytes.value().data());
    return cc::vector<float>::create_copy_of(cc::span<float const>(values, k_thread_count));
}
} // namespace

ASYNC_TEST("sg metal - an inline ray query hits and misses")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const scene = make_triangle_scene(ctx, true);
    auto const group = bind_scene(ctx, scene);

    auto const shader = fixture_shader(sg::shader_stage::compute, "trace_inline");
    auto pipeline = ctx->create_metal_compute_pipeline({.shader = shader, .layout = scene.pipeline_layout},
                                                       sg::lifetime_scope::persistent);
    REQUIRE(pipeline.has_value()).context(pipeline.has_error() ? pipeline.error().to_string() : cc::string());

    auto cmd = ctx->create_command_list();
    cmd->compute.bind_pipeline(*pipeline.value());
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_groups(int(k_thread_count), 1, 1);
    auto future = cmd->download.bytes_from_buffer(scene.out, 0, scene.out->size_in_bytes());
    ctx->submit_command_list(cc::move(cmd));
    co_await ctx->idle_completion();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    auto const* const values = reinterpret_cast<float const*>(bytes.value().data());

    // A hit distance distinguishes "traversal found the triangle" from "traversal ran against an empty scene", which
    // is what a structure missing from the residency set would look like.
    CHECK(values[0] == k_triangle_distance).context(cc::format("hit thread read {}", values[0]));
    CHECK(values[1] == k_miss).context(cc::format("miss thread read {}", values[1]));
}

ASYNC_TEST("sg metal - dispatch_rays reaches the closest-hit and miss functions")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const scene = make_triangle_scene(ctx, true);
    auto future = trace_with_pipeline(
        ctx, scene, "raygen",
        sg::hit_shader{.closest_hit = fixture_shader(sg::shader_stage::closest_hit, "closest_hit_marker")});
    co_await ctx->idle_completion();
    auto const values = traced_values(future);

    // Which of the two the kernel reached through its visible function tables, not merely that the dispatch ran:
    // a payload nothing wrote would still be 0.
    CHECK(values[0] == k_triangle_distance).context(cc::format("closest-hit thread read {}", values[0]));
    CHECK(values[1] == k_miss).context(cc::format("miss thread read {}", values[1]));
}

ASYNC_TEST("sg metal - an any-hit function rejects a hit during traversal")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // Non-opaque, which is the whole precondition: traversal consults no any-hit function on opaque geometry.
    auto const scene = make_triangle_scene(ctx, false);
    auto future = trace_with_pipeline(
        ctx, scene, "raygen",
        sg::hit_shader{.closest_hit = fixture_shader(sg::shader_stage::closest_hit, "closest_hit_marker"),
                       .any_hit = fixture_shader(sg::shader_stage::any_hit, "any_hit_reject")});
    co_await ctx->idle_completion();
    auto const values = traced_values(future);

    // The same geometry and the same ray that report a hit in the test above.
    // Reading a miss here is what proves the any-hit function ran, since nothing else could have changed the answer.
    CHECK(values[0] == k_miss).context(cc::format("rejected thread read {}, expected the any-hit to reject it", values[0]));
    CHECK(values[1] == k_miss).context(cc::format("miss thread read {}", values[1]));
}

ASYNC_TEST("sg metal - an intersection function describes a procedural primitive")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const scene = make_procedural_scene(ctx);
    auto future = trace_with_pipeline(
        ctx, scene, "raygen_procedural",
        sg::hit_shader{.closest_hit = fixture_shader(sg::shader_stage::closest_hit, "closest_hit_marker"),
                       .intersection = fixture_shader(sg::shader_stage::intersection, "procedural_hit")});
    co_await ctx->idle_completion();
    auto const values = traced_values(future);

    // procedural_hit reports 3, which is not the AABB's own entry distance — so the number proves the intersection
    // function produced it rather than traversal reporting the box itself.
    CHECK(values[0] == k_procedural_distance).context(cc::format("procedural thread read {}", values[0]));
    CHECK(values[1] == k_miss).context(cc::format("miss thread read {}", values[1]));
}

ASYNC_TEST("sg metal - a hit function recurses through its own table to the declared depth")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const scene = make_triangle_scene(ctx, true);
    auto const group = bind_scene(ctx, scene);

    // Four levels, which is what the shader recurses to and what the pipeline declares.
    // Apple's contract is that maxCallStackDepth defaults to 1 and is raised when a compute pass uses recursive
    // functions; measured here, under-declaring it still ran correctly, which matches Apple's note that the framework
    // reserves a large stack anyway.
    // So this asserts the depth actually reached rather than that an under-declared stack fails — that would be a test
    // of the driver's generosity.
    constexpr auto k_depth = u32(4);

    auto desc = sg::raytracing_pipeline_description{.layout = scene.pipeline_layout};
    desc.max_recursion_depth = k_depth;
    auto const raygen = desc.add_raygen_shader(fixture_shader(sg::shader_stage::raygen, "raygen_recursive"));
    auto const miss = desc.add_miss_shader(fixture_shader(sg::shader_stage::miss, "miss_marker"));
    auto const hit = desc.add_hit_shader(
        sg::hit_shader{.closest_hit = fixture_shader(sg::shader_stage::closest_hit, "closest_hit_recursive")});

    auto pipeline = ctx->create_metal_raytracing_pipeline(desc, sg::lifetime_scope::persistent);
    REQUIRE(pipeline.has_value()).context(pipeline.has_error() ? pipeline.error().to_string() : cc::string());

    auto table_desc = sg::raytracing_shader_table_description{.pipeline = pipeline.value()};
    auto const raygen_slot = table_desc.add_raygen_shader(raygen);
    (void)table_desc.add_miss_shader(miss);
    (void)table_desc.add_hit_shader(hit);

    auto table = ctx->create_metal_raytracing_shader_table(table_desc, sg::lifetime_scope::persistent);
    REQUIRE(table.has_value()).context(table.has_error() ? table.error().to_string() : cc::string());

    auto cmd = ctx->create_command_list();
    cmd->raytracing.bind_pipeline(*pipeline.value());
    cmd->raytracing.bind_group(0, *group);
    cmd->raytracing.dispatch_rays(*table.value(), raygen_slot, int(k_thread_count), 1, 1);
    auto future = cmd->download.bytes_from_buffer(scene.out, 0, scene.out->size_in_bytes());
    ctx->submit_command_list(cc::move(cmd));
    co_await ctx->idle_completion();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    auto const* const values = reinterpret_cast<float const*>(bytes.value().data());

    // The payload counts one per level, so it IS the depth reached.
    CHECK(values[0] == float(k_depth)).context(cc::format("recursed to depth {}, expected {}", values[0], k_depth));
    CHECK(values[1] == k_miss).context(cc::format("miss thread read {}", values[1]));
}

ASYNC_TEST("sg metal - a shader table is built from two separate libraries")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // The realistic shape: sg::compiled_shader is single-entry, so a real shader pipeline hands the backend one blob
    // per shader rather than one blob with several entry points.
    // Every other test here draws from one library, which cannot tell a backend that loads one per shader apart from
    // one that loads the first and finds the rest inside it.
    auto const scene = make_triangle_scene(ctx, true);
    auto const group = bind_scene(ctx, scene);

    auto desc = sg::raytracing_pipeline_description{.layout = scene.pipeline_layout};
    auto const raygen = desc.add_raygen_shader(fixture_shader(sg::shader_stage::raygen, "raygen"));
    auto const hit = desc.add_hit_shader(
        sg::hit_shader{.closest_hit = fixture_shader(sg::shader_stage::closest_hit, "closest_hit_marker")});

    // The one shader from the other file.
    auto const miss = desc.add_miss_shader(
        shader_from(mtl::test::raytrace_second_metallib, sg::shader_stage::miss, "miss_from_second_library"));

    auto pipeline = ctx->create_metal_raytracing_pipeline(desc, sg::lifetime_scope::persistent);
    REQUIRE(pipeline.has_value()).context(pipeline.has_error() ? pipeline.error().to_string() : cc::string());

    auto table_desc = sg::raytracing_shader_table_description{.pipeline = pipeline.value()};
    auto const raygen_slot = table_desc.add_raygen_shader(raygen);
    (void)table_desc.add_miss_shader(miss);
    (void)table_desc.add_hit_shader(hit);

    auto table = ctx->create_metal_raytracing_shader_table(table_desc, sg::lifetime_scope::persistent);
    REQUIRE(table.has_value()).context(table.has_error() ? table.error().to_string() : cc::string());

    auto cmd = ctx->create_command_list();
    cmd->raytracing.bind_pipeline(*pipeline.value());
    cmd->raytracing.bind_group(0, *group);
    cmd->raytracing.dispatch_rays(*table.value(), raygen_slot, int(k_thread_count), 1, 1);
    auto future = cmd->download.bytes_from_buffer(scene.out, 0, scene.out->size_in_bytes());
    ctx->submit_command_list(cc::move(cmd));
    co_await ctx->idle_completion();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    auto const* const values = reinterpret_cast<float const*>(bytes.value().data());

    // The hit thread reaches the first library's closest-hit, and the miss thread reaches the SECOND library's miss,
    // whose -2 is what distinguishes it from the -1 the first library's miss writes.
    CHECK(values[0] == k_triangle_distance).context(cc::format("closest-hit thread read {}", values[0]));
    CHECK(values[1] == -2.0f)
        .context(cc::format("miss thread read {}, expected -2 from the second library (-1 would mean the first "
                            "library's miss ran)",
                            values[1]));
}
