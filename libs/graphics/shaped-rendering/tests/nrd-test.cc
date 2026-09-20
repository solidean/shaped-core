#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/impl/nrd_instance.hh>
#include <shaped-rendering/nrd_denoise_routine.hh>
#include <shaped-rendering/shaders.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_library.hh>
#include <typed-geometry/scalar/scalar.hh>

#if SR_HAS_NRD
#include <shaped-rendering/impl/nrd_session.hh>
#endif

using namespace cc::primitive_defines;

// NRD's dispatch list, executed through sg.
//
// NRD renders nothing itself: it reports the pipelines it needs, the scratch it needs, and then a list of dispatches
// over those plus ours.
// So what there is to get wrong is entirely on our side — the register layout the pipelines are built against, the
// pool textures, and whether each dispatch's resources reach the bindings it declared.
//
// Every one of those failures is a debug-layer error rather than a wrong number, which is why this test asserts so
// little and still means something: the log rule fails it on any validation message, and reaching the end means the
// whole frame recorded and ran.
//
// It needs no particular adapter.
// NRD's dispatches are ordinary compute, so unlike DLSS this runs on WARP too — which is the whole reason this member
// is the one CI could test.
#if SR_HAS_NRD
ASYNC_INVOCABLE_TEST("sr - an NRD frame's dispatches build and run", (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    auto const extent = tg::vec2i(64, 48);

    auto session = sr::impl::nrd_session();
    REQUIRE(session.create(ctx, sr::impl::nrd_denoiser::reblur_diffuse_specular, extent));
    CHECK(session.is_valid());
    CHECK(session.extent() == extent);

    // The pipelines build in the background, so a session is ready some time after it is created.
    // Waited on the context's own backlog, which is what a pipeline build is queued on — a condition rather than a
    // duration.
    cc::async_backlog const* const backlogs[] = {&ctx.backlog};
    co_await cc::async_settled(cc::async_backlog::settled(backlogs));
    REQUIRE(session.is_ready()).context("NRD's pipelines never finished building");

    auto const make = [&](sg::pixel_format format)
    {
        return ctx.persistent.create_texture_2d({.format = format,
                                                 .width = extent[0],
                                                 .height = extent[1],
                                                 .usage = sg::texture_usage::readonly_texture
                                                        | sg::texture_usage::readwrite_texture
                                                        | sg::texture_usage::copy_dst});
    };

    // Zeroed inputs in NRD's own formats.
    // What is under test is the plumbing rather than the picture: a denoiser handed an empty scene has nothing to say,
    // and asserting on what it said would be asserting on NRD.
    auto const resources = sr::impl::nrd_resources{
        .motion = make(sg::pixel_format::rgba16_float),
        .normal_roughness = make(sg::pixel_format::rgb10a2_unorm),
        .view_z = make(sg::pixel_format::r32_float),
        .diffuse_radiance_hit_distance = make(sg::pixel_format::rgba16_float),
        .specular_radiance_hit_distance = make(sg::pixel_format::rgba16_float),
        .out_diffuse_radiance_hit_distance = make(sg::pixel_format::rgba16_float),
        .out_specular_radiance_hit_distance = make(sg::pixel_format::rgba16_float),
    };

    // Two frames: the first resets the history and the second continues it, which are different dispatch lists.
    // One frame alone would never exercise the reprojecting half, which is most of what REBLUR is.
    for (auto frame_index = u32(0); frame_index < 2; ++frame_index)
    {
        auto cmd = ctx.create_command_list();
        auto const frame = sr::impl::nrd_frame{.frame_index = frame_index, .reset = frame_index == 0};
        CHECK(session.execute(*cmd, frame, resources)).context(cc::format("frame {}", frame_index));
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
    }

    (void)co_await ctx.idle_completion();
}

// A session that was never created runs nothing, rather than dispatching against a null instance.
TEST("sr - an uncreated NRD session is inert")
{
    auto session = sr::impl::nrd_session();
    CHECK(!session.is_valid());
    CHECK(!session.is_ready());
}

// The member, end to end: guides in, one denoised image out.
//
// A constant radiance field is the one input whose answer is known without reimplementing REBLUR.
// Blurring a constant is that constant, accumulating it is that constant, and reprojecting it across zero motion is
// that constant — so whatever the denoiser does inside, the output has to come back as what went in.
//
// That makes this a real check on every encoding step between us and NRD rather than a smoke test.
// The radiance is packed into YCoCg with a normalized hit distance in alpha and decoded back out; a pack that dropped
// the chroma, a resolve that skipped the decode, or a repack that fed the diffuse texture to both lobes all land on a
// different colour, and none of them would report an error.
ASYNC_INVOCABLE_TEST("sr - NRD returns a constant radiance field unchanged",
                     (sg::context_handle const& ctx_h),
                     exclusive("slib-shader-library"))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    auto lib = slib::shader_library();
    auto compiler = slib::create_dxc_compiler();
    if (!compiler.has_value())
        SKIP("no DXC compiler to build NRD's repack and resolve passes");
    lib.add_compiler(cc::move(compiler.value()));
    lib.add_package(sr::shader_package());

    sr::nrd_denoise_routine::prewarm(ctx);
    (void)co_await ctx.routines.idle_completion();

    constexpr auto k_size = 16;
    auto const extent = tg::vec2i(k_size, k_size);

    auto const make = [&](sg::pixel_format format)
    {
        return ctx.persistent.create_texture_2d({.format = format,
                                                 .width = k_size,
                                                 .height = k_size,
                                                 .usage = sg::texture_usage::readonly_texture
                                                        | sg::texture_usage::readwrite_texture
                                                        | sg::texture_usage::copy_dst | sg::texture_usage::copy_src});
    };

    auto const diffuse = make(sg::pixel_format::rgba32_float);
    auto const specular = make(sg::pixel_format::rgba32_float);
    auto const normal = make(sg::pixel_format::rgba32_float);
    auto const roughness = make(sg::pixel_format::rgba32_float);
    auto const depth = make(sg::pixel_format::rgba32_float);
    auto const motion = make(sg::pixel_format::rgba32_float);
    auto const hit_distance = make(sg::pixel_format::rg32_float);
    auto const output = make(sg::pixel_format::rgba32_float);

    // Channels that differ from one another, so a repack that collapsed colour onto luminance cannot pass.
    auto const diffuse_radiance = tg::vec3f(0.2f, 0.5f, 0.9f);
    auto const specular_radiance = tg::vec3f(0.3f, 0.1f, 0.05f);

    auto const fill = [&](sg::command_list& cmd, sg::texture_2d const& t, tg::vec4f v)
    {
        auto const pixels = cc::vector<tg::vec4f>::create_filled(k_size * k_size, v);
        cmd.upload.bytes_to_texture(t.raw(), cc::span<tg::vec4f const>(pixels).as_bytes());
    };

    auto history = sr::denoise_history();
    auto outcome = sr::denoise_outcome{};

    // The session's own pipelines build after the first call reaches it, so the first call reports pending by design.
    // Run until it stops doing so rather than assuming a fixed number of frames, and keep going for one more after
    // that: a reset frame and a continued frame are different dispatch lists.
    auto denoised_frames = 0;
    for (auto attempt = 0; attempt < 8 && denoised_frames < 2; ++attempt)
    {
        auto cmd = ctx.create_command_list();

        fill(*cmd, diffuse, tg::vec4f(diffuse_radiance[0], diffuse_radiance[1], diffuse_radiance[2], 0));
        fill(*cmd, specular, tg::vec4f(specular_radiance[0], specular_radiance[1], specular_radiance[2], 0));
        fill(*cmd, normal, tg::vec4f(0, 0, 1, 0));
        fill(*cmd, roughness, tg::vec4f(0.5f, 0, 0, 0));
        fill(*cmd, depth, tg::vec4f(5.0f, 0, 0, 0));
        fill(*cmd, motion, tg::vec4f(0, 0, 0, 0));
        {
            auto const pixels = cc::vector<tg::vec2f>::create_filled(k_size * k_size, tg::vec2f(2.0f, 2.0f));
            cmd->upload.bytes_to_texture(hit_distance.raw(), cc::span<tg::vec2f const>(pixels).as_bytes());
        }

        auto const in = sr::denoise_inputs{
            .color = diffuse,
            .specular = specular,
            .guides
            = {.normal = normal, .roughness = roughness, .depth = depth, .motion = motion, .hit_distance = hit_distance},
            .output = output,
        };

        outcome = sr::nrd_denoise_routine::execute(*cmd, in, history);
        REQUIRE(outcome.status != sr::denoise_status::unsupported);
        REQUIRE(outcome.status != sr::denoise_status::failed);

        if (outcome.is_denoised())
            ++denoised_frames;

        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();

        cc::async_backlog const* const backlogs[] = {&ctx.backlog};
        co_await cc::async_settled(cc::async_backlog::settled(backlogs));
    }

    REQUIRE(denoised_frames == 2).context("NRD never produced a denoised frame");

    auto cmd = ctx.create_command_list();
    auto const readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(output.raw()));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const pixels = co_await readback.data();
    REQUIRE(pixels.size() == k_size * k_size);

    // The interior, away from the border REBLUR's spatial passes clamp against.
    auto const expected = diffuse_radiance + specular_radiance;
    auto const& centre = pixels[(k_size / 2) * k_size + k_size / 2];
    for (auto c = 0; c < 3; ++c)
        CHECK(tg::abs(centre[c] - expected[c]) < 0.02f).context(cc::format("channel {}", c));
}
#endif
