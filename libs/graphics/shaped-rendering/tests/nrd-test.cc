#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/impl/nrd_instance.hh>

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
        .normal_roughness = make(sg::pixel_format::rgba8_unorm),
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
#endif
