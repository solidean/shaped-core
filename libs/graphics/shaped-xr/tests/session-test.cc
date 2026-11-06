#include <clean-core/platform/environment.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-shader-library/compiler/available_compilers.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-xr/controller_routine.hh>
#include <shaped-xr/runtime_report.hh>
#include <shaped-xr/xr.hh>
#include <typed-geometry/transform/compose.hh>

using namespace cc::primitive_defines;

// These run against Monado's simulated headset and null compositor, and skip without them, since on any other runtime a session would start on someone's head.
// XRT_COMPOSITOR_NULL is what opts in, see libs/graphics/shaped-xr/docs/testing.md.
// The loader holds one instance per process, so they run one at a time.

namespace
{
void require_simulated_headset()
{
    if (!cc::environment_variable("XRT_COMPOSITOR_NULL").has_value())
        SKIP("needs Monado's simulated headset, see shaped-xr's docs/testing.md");
}

[[nodiscard]] cc::unique_ptr<sxr::system> simulated_system(sxr::system_description const& desc)
{
    require_simulated_headset();
    auto created = sxr::system::try_create(desc);
    REQUIRE(created.has_value());
    return cc::move(created.value());
}

// a session starts when the runtime is ready, and its first frames ask for nothing, which must still end
[[nodiscard]] sxr::frame next_rendered_frame(sxr::session& session)
{
    for (auto attempt = 0; attempt < 10000; ++attempt)
    {
        REQUIRE(session.poll_events());
        auto frame = session.wait_frame();
        if (!frame.has_value())
            cc::this_thread_sleep_secs(0.001);
        else if (frame.value().should_render)
            return cc::move(frame.value());
        else
        {
            CHECK(frame.value().views.empty());
            session.submit_frame(nullptr);
        }
    }
    FAIL("the runtime never asked for a frame");
    return {};
}

void check_stereo(sxr::frame const& frame)
{
    REQUIRE(frame.views.size() == 2);
    CHECK(frame.width > 0);
    CHECK(frame.height > 0);
    CHECK(frame.color.array_layers() == 2);
    CHECK(frame.depth.array_layers() == 2);
    auto const between_eyes = frame.views[1].camera_to_world.translation() - frame.views[0].camera_to_world.translation();
    CHECK(between_eyes.length() > 0.0f);
}

void check_cleared(cc::span<byte const> bytes, sxr::frame const& frame)
{
    REQUIRE(bytes.size() == frame.width * frame.height * 4);
    CHECK(u8(bytes[0]) == 255);
    CHECK(u8(bytes[1]) == 0);
    CHECK(u8(bytes[2]) == 255);
    CHECK(u8(bytes[3]) == 255);
}

[[nodiscard]] std::unique_ptr<sg::command_list> cleared(sg::context& ctx, sxr::frame const& frame, tg::vec4f color)
{
    auto cmd = ctx.create_command_list();
    {
        auto pass = cmd->raster.render_to({.color_targets = {frame.color.as_render_target_view().cleared(color)},
                                           .depth_stencil_target = frame.depth.as_depth_stencil_view().cleared(1.0f),
                                           .view_count = 2});
    }
    return cmd;
}
} // namespace

TEST("shaped-xr session - the report names the simulated headset, which has none of Meta's AR",
     nx::config::exclusive("openxr"))
{
    require_simulated_headset();

    auto const report = sxr::inspect_runtime();
    REQUIRE(report.has_value());
    CHECK(!report.value().system_name.empty());
    CHECK(!report.value().extensions.empty());
    for (auto const& feature : report.value().features)
        CHECK(!feature.is_supported);
}

TEST("shaped-xr session - a system wanting what the runtime lacks is created, and offers none of it",
     nx::config::exclusive("openxr"))
{
    auto const system = simulated_system(
        {.wants = {sxr::capability::passthrough, sxr::capability::environment_depth, sxr::capability::hit_tests}});
    CHECK(!system->offers(sxr::capability::passthrough));
    CHECK(!system->offers(sxr::capability::environment_depth));
    CHECK(!system->offers(sxr::capability::hit_tests));
}

ASYNC_TEST("shaped-xr session - vulkan renders into the runtime's images, frame after frame",
           nx::config::exclusive("openxr"))
{
    auto const system = simulated_system({.api = sxr::graphics_api::vulkan});
    auto const ctx = system->create_context().value();
    REQUIRE(ctx->supports(sg::feature::view_instancing));
    auto const session = system->create_session(*ctx, {}).value();
    CHECK(session->availability_of(sxr::capability::hit_tests) == sxr::availability::unsupported);

    for (auto i = 0; i < 3; ++i)
    {
        auto const frame = next_rendered_frame(*session);
        check_stereo(frame);
        session->submit_frame(cleared(*ctx, frame, tg::vec4f(0, 0, 0, 1)));
        ctx->advance_epoch();
        co_await ctx->epochs_in_flight_completion(2);
    }
    co_await ctx->idle_completion();
}

ASYNC_TEST("shaped-xr session - a blend mode the runtime lacks falls back to opaque, and says so",
           nx::config::exclusive("openxr"))
{
    auto const system = simulated_system({.api = sxr::graphics_api::vulkan});
    nx::expect_warning("a blend mode was asked of a session*");
    CHECK(system->offers_blend(sxr::blend_mode::opaque));
    CHECK(!system->offers_blend(sxr::blend_mode::additive));

    auto const ctx = system->create_context().value();
    auto const session = system->create_session(*ctx, {.blend = sxr::blend_mode::additive}).value();
    CHECK(session->blend() == sxr::blend_mode::opaque);

    auto const frame = next_rendered_frame(*session);
    session->submit_frame(cleared(*ctx, frame, tg::vec4f(0, 0, 0, 1)));
    ctx->advance_epoch();
    co_await ctx->idle_completion();
}

ASYNC_TEST("shaped-xr session - a controller's pose is read by name, per hand, and its inputs stay released",
           nx::config::exclusive("openxr"))
{
    auto const system = simulated_system({.api = sxr::graphics_api::vulkan});
    auto const ctx = system->create_context().value();
    auto const session = system->create_session(*ctx, {.actions = sxr::standard_controller_actions()}).value();

    if (!cc::environment_variable("SIMULATED_LEFT").has_value())
        SKIP("needs Monado's simulated controllers, see shaped-xr's docs/testing.md");

    // actions sync from the first focused frame on
    auto left = cc::optional<tg::rigid_transform3f>();
    auto right = cc::optional<tg::rigid_transform3f>();
    for (auto i = 0; i < 100 && !(left.has_value() && right.has_value()); ++i)
    {
        auto const frame = next_rendered_frame(*session);
        left = session->pose("grip", sxr::hand::left);
        right = session->pose("grip", sxr::hand::right);
        session->submit_frame(cleared(*ctx, frame, tg::vec4f(0, 0, 0, 1)));
        ctx->advance_epoch();
        co_await ctx->epochs_in_flight_completion(2);
    }
    CHECK(session->is_focused());
    REQUIRE(left.has_value());
    REQUIRE(right.has_value());
    CHECK(left.value().translation()[0] < right.value().translation()[0]);

    CHECK(!session->pressed("primary", sxr::hand::left));
    CHECK(session->scalar("trigger", sxr::hand::right) == 0.0f);
    session->vibrate("haptic", sxr::hand::left, 0.5f, 0.01f);
    co_await ctx->idle_completion();
}

// a color that reads the same in rgba and bgra, and in linear and sRGB
constexpr auto readback_clear = tg::vec4f(1, 0, 1, 1);

ASYNC_TEST("shaped-xr session - a readable frame's clear reaches both layers of the runtime's vulkan image",
           nx::config::exclusive("openxr"))
{
    auto const system = simulated_system({.api = sxr::graphics_api::vulkan});
    auto const ctx = system->create_context().value();
    auto const session = system->create_session(*ctx, {.readable_color = true}).value();

    auto const frame = next_rendered_frame(*session);
    auto cmd = cleared(*ctx, frame, readback_clear);
    auto const left = cmd->download.bytes_from_texture(frame.color.raw(), {.array_layer = 0});
    auto const right = cmd->download.bytes_from_texture(frame.color.raw(), {.array_layer = 1});
    session->submit_frame(cc::move(cmd));
    ctx->advance_epoch();

    check_cleared(co_await left.bytes(), frame);
    check_cleared(co_await right.bytes(), frame);

    // the image went back to the runtime in its attachment layout, so the next frame still takes it
    auto const next = next_rendered_frame(*session);
    session->submit_frame(cleared(*ctx, next, readback_clear));
    ctx->advance_epoch();
    co_await ctx->idle_completion();
}

#if SXR_TEST_HAS_DXC

namespace
{
/// Draws a hand half a metre ahead of the left eye into a frame cleared to `readback_clear`, and reads both eyes back.
[[nodiscard]] cc::shared_async<cc::unit> check_a_hand_is_drawn(sxr::graphics_api api)
{
    auto lib = slib::shader_library();
    slib::add_available_compilers(lib);
    lib.add_package(sxr::shader_package());

    auto const system = simulated_system({.api = api});
    auto const ctx = system->create_context().value();
    if (auto* const home = ctx->device_home())
        co_await cc::async_resume_on(*home);
    auto const session = system->create_session(*ctx, {.readable_color = true}).value();

    auto const frame = next_rendered_frame(*session);
    sxr::controller_routine::prewarm(*ctx, frame.color.format());
    (void)co_await ctx->routines.idle_completion();

    // +z is forward in tg's frame
    auto const hand = tg::rigid_transform3f(
        tg::compose(frame.views[0].camera_to_world, tg::rigid_transform3f::make_translation(tg::vec3f(0, 0, 0.5f))));
    sxr::controller_pose const hands[] = {{.grip = hand}};

    auto cmd = ctx->create_command_list();
    {
        auto pass = cmd->raster.render_to({.color_targets = {frame.color.as_render_target_view().cleared(readback_clear)},
                                           .depth_stencil_target = frame.depth.as_depth_stencil_view().cleared(1.0f),
                                           .view_count = 2,
                                           .target_set = "an::application's::own::target"});
        CHECK(sxr::controller_routine::execute(pass, frame, hands) == sg::routine_outcome::executed);
    }
    auto const left = cmd->download.bytes_from_texture(frame.color.raw(), {.array_layer = 0});
    auto const right = cmd->download.bytes_from_texture(frame.color.raw(), {.array_layer = 1});
    session->submit_frame(cc::move(cmd));
    ctx->advance_epoch();

    // each eye sees the hand where that eye's own projection puts it, so both layers prove their own matrix
    auto const center = hand.translation();
    auto const check_eye = [&](cc::span<byte const> bytes, sxr::view const& view)
    {
        REQUIRE(bytes.size() == frame.width * frame.height * 4);
        auto const green_at = [&](int x, int y) { return u8(bytes[(isize(y) * frame.width + x) * 4 + 1]); };

        auto const clip = view.world_to_clip() * tg::vec4f(center[0], center[1], center[2], 1.0f);
        auto const x = int((clip[0] / clip[3] * 0.5f + 0.5f) * float(frame.width));
        auto const y = int((0.5f - clip[1] / clip[3] * 0.5f) * float(frame.height));
        REQUIRE(x >= 0);
        REQUIRE(x < frame.width);
        REQUIRE(y >= 0);
        REQUIRE(y < frame.height);

        // the clear leaves green at 0, and the box's grey does not
        CHECK(green_at(x, y) != 0);
        CHECK(green_at(2, 2) == 0);
    };
    check_eye(co_await left.bytes(), frame.views[0]);
    check_eye(co_await right.bytes(), frame.views[1]);
    co_await ctx->idle_completion();
    co_return;
}
} // namespace

ASYNC_TEST("shaped-xr session - a hand is drawn where its pose puts it, on vulkan", nx::config::exclusive("openxr"))
{
    co_await check_a_hand_is_drawn(sxr::graphics_api::vulkan);
}

#ifdef CC_OS_WINDOWS
ASYNC_TEST("shaped-xr session - a hand is drawn where its pose puts it, on dx12", nx::config::exclusive("openxr"))
{
    co_await check_a_hand_is_drawn(sxr::graphics_api::dx12);
}
#endif

#endif

#ifdef CC_OS_WINDOWS
ASYNC_TEST("shaped-xr session - a frame's clear reaches both layers of dx12's own image", nx::config::exclusive("openxr"))
{
    auto const system = simulated_system({.api = sxr::graphics_api::dx12});
    auto const ctx = system->create_context().value();
    REQUIRE(ctx->supports(sg::feature::view_instancing));
    auto const session = system->create_session(*ctx, {}).value();

    auto const frame = next_rendered_frame(*session);
    check_stereo(frame);

    auto cmd = cleared(*ctx, frame, readback_clear);
    auto const left = cmd->download.bytes_from_texture(frame.color.raw(), {.array_layer = 0});
    auto const right = cmd->download.bytes_from_texture(frame.color.raw(), {.array_layer = 1});
    session->submit_frame(cc::move(cmd));
    ctx->advance_epoch();

    check_cleared(co_await left.bytes(), frame);
    check_cleared(co_await right.bytes(), frame);
    co_await ctx->idle_completion();
}
#endif
