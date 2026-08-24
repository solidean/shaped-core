#include "viewer_test_env.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-viewer/all.hh>

// Headless, one view, through the single-view routine on its own: view_renderer path-traces it into the texture kept under the view's id and hands that back — no target, no pass.
// The frame-level counterpart is viewer-renderer-test; this one pins the half a caller reaches for when it wants the image rather than a composited frame.
//
// No pixel readback: reaching the end without an assert / exception / debug-layer error means the trace recorded and ran.
TEST("sv - view renderer end to end (headless)")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the shaders");

    // Build the scene through the managers (this is where the BLAS is built).
    auto const cloud = sv_test::make_triangle_cloud(64);
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const mesh = resources.meshes.acquire(sv::triangle_data::create(cloud.positions));
    auto const materials = resources.materials.acquire(sv::material_data::create(cloud.materials));
    REQUIRE(resources.meshes.contains(mesh));
    REQUIRE(resources.materials.contains(materials));

    auto const size = tg::vec2i(128, 128);

    auto v = sv::view_data{};
    v.id = sv::view_id::from_string("headless");
    v.resolution = size;
    v.camera = sv::camera{.position = tg::pos3d(2.4, 1.8, -3.2)}; // default orientation frames the origin
    sv::ensure_scene_3d(v).items.push_back({.mesh = mesh, .materials = materials});
    // Lights are a typed list on the view — an overhead rect facing down (cross(+x, +z) is -y).
    // Exercises the area_light -> area_light_gpu derivation the view_renderer does.
    sv::ensure_scene_3d(v).area_lights.push_back({.center = tg::pos3f(0, 3, 0),
                                                  .half_extent_u = tg::vec3f(0.75f, 0, 0),
                                                  .half_extent_v = tg::vec3f(0, 0, 0.75f),
                                                  .emission = tg::vec3f(18.0f, 18.0f, 18.0f)});

    auto cmd = ctx.create_command_list();
    resources.advance_to(ctx.current_epoch()); // the frame's job, not the renderer's
    auto store = sv::view_store{};             // and so is what the view keeps across frames

    // The renderer only ever hands back a texture — it never sees an output target.
    auto const traced = sv::view_renderer::execute(*cmd, v, resources, store);
    CHECK(traced.width() == size[0]);
    CHECK(traced.height() == size[1]); // sized from the view, not from any target

    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch_and_wait_for_idle();
}

// The same frame, driven from indexed geometry: an indexed BLAS build plus the closest-hit's Vertices[Indices[..]] lookup.
// A Cornell box is the payload because its quads genuinely share vertices, so welding actually shrinks the vertex buffer.
// The index buffer is then not the identity sequence the non-indexed path would synthesize.
TEST("sv - view renderer renders indexed geometry (headless)")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the shaders");

    auto const box = sv_test::make_cornell_box();
    auto const welded = sv_test::weld_triangle_list(box.positions);
    REQUIRE(welded.indices.size() == box.positions.size());
    REQUIRE(welded.positions.size() < box.positions.size()); // the quads really do share vertices

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const mesh = resources.meshes.acquire(sv::indexed_triangle_data::create(welded.positions, welded.indices));
    auto const materials = resources.materials.acquire(sv::material_data::create(box.materials));

    auto const* const mesh_rec = resources.meshes.get_ptr(mesh);
    REQUIRE(mesh_rec != nullptr);
    CHECK(mesh_rec->is_indexed); // an indexed BLAS, and the closest-hit reads through the real index buffer
    CHECK(mesh_rec->indices.element_count() == welded.indices.size());
    CHECK(mesh_rec->vertices.element_count() == welded.positions.size());
    // Triangle order follows the index buffer, so the per-triangle material set still lines up.
    CHECK(mesh_rec->triangle_count == box.materials.size());

    auto const size = tg::vec2i(96, 96);

    auto v = sv::view_data{};
    v.id = sv::view_id::from_string("indexed");
    v.resolution = size;
    v.camera = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
    sv::ensure_scene_3d(v).items.push_back({.mesh = mesh, .materials = materials});
    sv::ensure_scene_3d(v).area_lights.push_back({.center = tg::pos3f(0, 3, 0),
                                                  .half_extent_u = tg::vec3f(0.75f, 0, 0),
                                                  .half_extent_v = tg::vec3f(0, 0, 0.75f),
                                                  .emission = tg::vec3f(15.0f, 15.0f, 15.0f)});

    auto cmd = ctx.create_command_list();
    resources.advance_to(ctx.current_epoch());
    auto store = sv::view_store{};
    (void)sv::view_renderer::execute(*cmd, v, resources, store);
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch_and_wait_for_idle();

    // A second acquire of the same content must hit the cache rather than build a second BLAS.
    auto const again = resources.meshes.acquire(sv::indexed_triangle_data::create(welded.positions, welded.indices));
    CHECK(again == mesh);
    CHECK(resources.meshes.count() == 1);
}
