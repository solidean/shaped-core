// The SGL cube in a headset through shaped-xr: over Quest Link or any other OpenXR runtime on Windows.
//
// Vulkan by default, where the runtime's images are rendered into directly; `--dx12` takes the other binding.
// Both eyes are one draw through view instancing, and the room shows behind the cube where the runtime offers passthrough.

#include <clean-core/common/time.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/args/ambient.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-shader-library/compiler/available_compilers.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-xr/xr.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/angle.hh>
#include <xr_cube_shaders.hh>

using namespace cc::primitive_defines;

namespace
{
using cube_vertex = shaders::xr_cube::cube_vertex;
constexpr int cube_index_count = 36;

// sgl-cube's mesh: four corners a face, so a shared corner carries three different normals
[[nodiscard]] cc::array<cube_vertex> build_cube_mesh()
{
    tg::vec3f const normals[]
        = {tg::vec3f(0, 0, -1), tg::vec3f(0, 0, 1), tg::vec3f(-1, 0, 0), tg::vec3f(1, 0, 0), tg::vec3f(0, -1, 0), tg::vec3f(0, 1, 0)};
    tg::vec3f const colors[] = {tg::vec3f(0.90f, 0.32f, 0.30f), tg::vec3f(0.30f, 0.62f, 0.90f), tg::vec3f(0.42f, 0.80f, 0.42f),
                                tg::vec3f(0.94f, 0.74f, 0.28f), tg::vec3f(0.66f, 0.44f, 0.88f), tg::vec3f(0.94f, 0.94f, 0.92f)};
    auto out = cc::array<cube_vertex>::create_defaulted(24);
    for (auto face = 0; face < 6; ++face)
    {
        auto const n = normals[face];
        auto const u = tg::vec3f(n[1], n[2], n[0]);
        auto const v = tg::dual(tg::cross(n, u));
        for (auto corner = 0; corner < 4; ++corner)
        {
            auto const su = (corner == 1 || corner == 2) ? 1.0f : -1.0f;
            auto const sv = (corner >= 2) ? 1.0f : -1.0f;
            out[face * 4 + corner]
                = {.position = tg::pos3f::zero + (n + u * su + v * sv) * 0.5f, .normal = n, .color = colors[face]};
        }
    }
    return out;
}

[[nodiscard]] cc::array<u16> build_cube_indices()
{
    auto out = cc::array<u16>::create_defaulted(cube_index_count);
    for (auto face = 0; face < 6; ++face)
    {
        u16 const quad[] = {0, 2, 1, 0, 3, 2};
        for (auto i = 0; i < 6; ++i)
            out[face * 6 + i] = u16(face * 4 + quad[i]);
    }
    return out;
}

/// The cube's world matrix: a 30 cm cube a metre ahead of where the session started, at eye height, spinning.
[[nodiscard]] tg::mat4f cube_to_world(tg::angle_f spin)
{
    auto const rotation = tg::mat3f::make_rotation_y(spin) * 0.3f;
    auto m = tg::mat4f::identity;
    for (auto c = 0; c < 3; ++c)
        for (auto r = 0; r < 3; ++r)
            m[c, r] = rotation[c, r];
    m[3, 2] = 1.0f;
    return m;
}
} // namespace

// `dev.py example xr-cube --test-args "--dx12"` takes the dx12 binding
ASYNC_EXAMPLE("shaped-graphics/xr-cube", nx::config::args("--vulkan"))
{
    auto api = xr::graphics_api::vulkan;
    for (auto const& arg : nx::test_args())
        if (cc::string_view(arg) == "--dx12")
            api = xr::graphics_api::dx12;
    auto created_system = xr::system::try_create({.application_name = "shaped-core xr-cube", .api = api, .wants_passthrough = true});
    if (created_system.has_error())
    {
        cc::eprintln("no headset: {}", created_system.error().to_string());
        FAIL("xr-cube cannot run, as printed above");
        co_return;
    }
    auto const system = cc::move(created_system.value());
    cc::println("xr: {} on {}, through {}", system->system_name(), system->runtime_name(), api == xr::graphics_api::dx12 ? "dx12" : "vulkan");

    auto created_context = system->create_context();
    if (created_context.has_error())
    {
        cc::eprintln("no context on the runtime's GPU: {}", created_context.error().to_string());
        FAIL("xr-cube cannot run, as printed above");
        co_return;
    }
    auto const ctx = created_context.value();
    if (!ctx->supports(sg::feature::view_instancing))
    {
        cc::eprintln("this GPU renders one view a draw, and xr-cube draws both eyes in one");
        FAIL("xr-cube cannot run, as printed above");
        co_return;
    }

    auto created_session = system->create_session(*ctx, {.passthrough = true});
    if (created_session.has_error())
    {
        cc::eprintln("no session: {}", created_session.error().to_string());
        FAIL("xr-cube cannot run, as printed above");
        co_return;
    }
    auto const session = cc::move(created_session.value());
    cc::println("xr: passthrough {}", session->shows_passthrough() ? "on" : "off");

    auto lib = slib::shader_library();
    slib::add_available_compilers(lib);
    lib.add_package(shaders::package());
    auto vertices = ctx->persistent.create_buffer_from_data(build_cube_mesh(), sg::buffer_usage::vertex_buffer);
    auto indices = ctx->persistent.create_buffer_from_data(build_cube_indices(), sg::buffer_usage::index_buffer);
    auto pipeline = sg::raster_pipeline_handle();
    auto built_for = sg::pixel_format::undefined;

    auto frames = u64(0);
    auto const start = cc::current_time_steady_secs();
    auto window_start = start;
    while (session->poll_events())
    {
        auto waited = session->wait_frame();
        if (!waited.has_value())
        {
            cc::this_thread_sleep_secs(0.05); // nothing paces the loop until the runtime is ready
            continue;
        }
        auto& frame = waited.value();
        auto cmd = std::unique_ptr<sg::command_list>();
        if (frame.should_render)
        {
            // the pipeline is built for the format the runtime's images came in, once it is known
            if (built_for != frame.color.format())
            {
                auto const built = ctx->cached.acquire_raster_pipeline(shaders::xr_cube::pipeline, {.color = frame.color.format()});
                co_await cc::async_settled(built);
                if (built->try_value() == nullptr)
                {
                    cc::eprintln("the cube's pipeline did not build");
                    session->submit_frame(nullptr);
                    FAIL("xr-cube cannot run, as printed above");
                    co_return;
                }
                pipeline = *built->try_value();
                built_for = frame.color.format();
            }

            auto const model = cube_to_world(tg::angle_f::make_from_degree(float(cc::current_time_steady_secs() - start) * 30.0f));
            auto const clear = session->shows_passthrough() ? tg::vec4f(0, 0, 0, 0) : tg::vec4f(0.02f, 0.02f, 0.03f, 1.0f);
            cmd = ctx->create_command_list();
            {
                auto pass = cmd->raster.render_to({.color_targets = {frame.color.as_render_target_view().cleared(clear)},
                                                   .depth_stencil_target = frame.depth.as_depth_stencil_view().cleared(1.0f),
                                                   .view_count = 2,
                                                   .target_set = shaders::xr_cube::target::name});
                pass.bind_pipeline(*pipeline);
                pass.bind_vertex_buffers({vertices.as_vertex_buffer()});
                pass.bind_index_buffer(indices.as_index_buffer());
                pass.set_inline_constants(shaders::xr_cube::constants{.left_eye = frame.views[0].world_to_clip() * model,
                                                                      .right_eye = frame.views[1].world_to_clip() * model}
                                              .to_block());
                pass.draw_indexed({.index_range = {.offset = 0, .size = cube_index_count}});
            }
        }
        session->submit_frame(cc::move(cmd));
        ctx->advance_epoch();
        co_await ctx->epochs_in_flight_completion(2);

        if (++frames % 300 == 0)
        {
            auto const now = cc::current_time_steady_secs();
            cc::println("xr: {} frames, {:.1f} fps over the last 300", frames, 300.0 / (now - window_start));
            window_start = now;
        }
    }

    ctx->advance_epoch();
    co_await ctx->idle_completion();
}
