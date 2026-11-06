#include "viewer_test_env.hh"

#include <babel-serializer/image/image.hh>
#include <clean-core/common/time.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
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

/// The Cornell box seen straight through its open side.
[[nodiscard]] sv::camera cornell_camera()
{
    auto cam = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
    cam.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
    return cam;
}
} // namespace

TEST("sv raster_bound - a device without ray tracing or binding arrays resolves automatic to raster_bound")
{
    auto created = create_webgpu_like_context();
    if (created.has_error())
        SKIP("no dx12 device here");
    CHECK(sv::resolve_render_mode(*created.value(), sv::render_mode::automatic) == sv::render_mode::raster_bound);
    CHECK(sv::resolve_render_mode(*created.value(), sv::render_mode::raster) == sv::render_mode::raster);
}

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

// A bound layer's capture, end to end: the viewer comes up on the device, the image is the Cornell box's, and it is final.
//
// The capture asks for `accumulation_sample_cap` samples, which only a converged layer holds, so a file at all means the layer converged.
// The capture protocol is process environment, which is why this carries capture-environment as dx12-entry.cc's drivers do.
ASYNC_TEST("sv raster_bound - a bound layer draws the Cornell box on a device without binding arrays",
           nx::config::exclusive("capture-environment"))
{
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler, so the viewer's shaders do not build");
    auto created = create_webgpu_like_context();
    if (created.has_error())
        SKIP("no dx12 device here");
    auto& ctx = *created.value();
    REQUIRE(!ctx.supports(sg::feature::binding_arrays));

    constexpr auto size = tg::vec2i(96, 64);
    auto const path = cc::temp_file_path("sv-raster-bound", ".png");
    auto const on = cc::scoped_environment_variable(sr::capture_request_env_var, "1");
    auto const out = cc::scoped_environment_variable(sr::capture_output_env_var, path);
    auto const which = cc::scoped_environment_variable(sr::capture_name_env_var, "front");
    auto const dim = cc::scoped_environment_variable(sr::capture_size_env_var, cc::format("{}x{}", size[0], size[1]));
    auto const acc
        = cc::scoped_environment_variable(sr::capture_accumulate_env_var, cc::format("{}", sv::accumulation_sample_cap));
    auto const lim = cc::scoped_environment_variable(sr::capture_timeout_env_var, "40");

    auto const box = sv_test::make_cornell_box();
    auto const mesh = sv_test::as_mesh("cornell box", box.positions, box.materials);

    auto const start = cc::current_time_steady_secs();
    for (auto f : sv::interactive(ctx, "sv-test/raster-bound"))
    {
        auto view = f.window().view();
        view.camera(cornell_camera());
        f.register_capture("front", [](sv::capture_context const&) {});
        auto scene = view.add_scene();
        scene.add_mesh(mesh);
        scene.add_rect_light("key", tg::pos3f(0, 0.99f, 0), tg::vec3f(0.35f, 0, 0), tg::vec3f(0, 0, 0.35f)).nits(40);
        scene.settings({.mode = sv::render_mode::raster_bound});
        REQUIRE(cc::current_time_steady_secs() - start < 35.0);
    }
    co_await cc::async_settled(sv::background_work(ctx));

    {
        auto reread = cc::file_read_stream_adapter::open(path);
        REQUIRE(reread.has_value());
        cc::read_stream in = reread.value();
        auto const decoded = babel::image::read(in);
        REQUIRE(decoded.has_value());
        auto const& img = decoded.value();
        REQUIRE(img.width == size[0]);
        REQUIRE(img.height == size[1]);

        // The left wall red and the right one green, as the bindless mode and the tracer draw them.
        auto const at = [&](int x, int y, int c) { return int(img.pixels[(y * img.width + x) * img.channels + c]); };
        CHECK(at(22, 32, 0) > at(22, 32, 1) + 20);
        CHECK(at(74, 32, 1) > at(74, 32, 0) + 20);
    }
    cc::remove_file(path);
    CHECK(!ctx.is_device_lost());
}
