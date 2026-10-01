#include "../shaders/shader_fixtures.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-shader-library/binding/binding_groups.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// The SGL fixture in tests/shaders/sgl/coherence.sgl: an @atomic image updated from many invocations at once, and
// @coherent members published through barriers.

namespace
{
sg::texture_2d make_image(sg::context_handle const& ctx, sg::pixel_format format, int width, int height)
{
    return sg::texture_2d::from_raw(ctx->persistent.create_raw_texture({
        .format = format,
        .dimension = sg::texture_dimension::d2,
        .width = width,
        .height = height,
        .usage = sg::texture_usage::image | sg::texture_usage::copy_src | sg::texture_usage::copy_dst,
    }));
}

/// Four bytes of a downloaded texture as the one 32-bit texel they hold.
template <class T>
T texel_at(cc::span<byte const> bytes, isize index)
{
    auto value = T();
    cc::memcpy(&value, bytes.data() + index * 4, 4);
    return value;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - an SGL @atomic image keeps each texel's maximum and count across every invocation",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::image_atomics))
        SKIP("this context has no image atomics");

    auto const pipeline = co_await shaders::coherence.reduce_max.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::texel_maxima>();

    // scattered values, so that the largest of a texel's 64 stands anywhere among them
    constexpr auto count = 16 * 64;
    auto input = cc::vector<u32>();
    for (auto i = 0; i < count; ++i)
        input.push_back(u32(i) * 2654435761u >> 8);
    auto expected = cc::vector<u32>::create_defaulted(16);
    for (auto i = 0; i < count; ++i)
        expected[i % 16] = input[i] > expected[i % 16] ? input[i] : expected[i % 16];

    auto const values = ctx->persistent.create_buffer_from_data(cc::vector<u32>::create_copy_of(input),
                                                                sg::buffer_usage::readonly_buffer);
    auto const maxima = make_image(ctx, sg::pixel_format::r32_uint, 4, 4);
    auto const counts = make_image(ctx, sg::pixel_format::r32_uint, 4, 4);
    auto const zeros = cc::vector<u32>::create_defaulted(16);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(maxima.raw(), cc::as_bytes(zeros));
    cmd->upload.bytes_to_texture(counts.raw(), cc::as_bytes(zeros));
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout,
        shaders::texel_maxima{.values = values.as_readonly_buffer(),
                              .maxima = maxima.as_image_view<sg::pixel_format::r32_uint>(),
                              .counts = counts.as_image_view<sg::pixel_format::r32_uint>()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(count);
    auto const maxima_back = cmd->download.bytes_from_texture(maxima.raw());
    auto const counts_back = cmd->download.bytes_from_texture(counts.raw());
    ctx->submit_command_list(cc::move(cmd));

    auto const got_maxima = co_await maxima_back.bytes();
    auto const got_counts = co_await counts_back.bytes();
    REQUIRE(got_maxima.size() == 16 * 4);
    REQUIRE(got_counts.size() == 16 * 4);
    for (auto t = 0; t < 16; ++t)
    {
        CHECK(texel_at<u32>(got_maxima, t) == expected[t]);
        CHECK(texel_at<u32>(got_counts, t) == 64u);
    }
}

ASYNC_INVOCABLE_TEST("sg - SGL's @coherent buffer and image take every invocation's write and the workgroups' count",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::device_coherence))
        SKIP("this context has no device coherence");

    auto const pipeline = co_await shaders::coherence.publish.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::handoff>();

    // what coherence promises is visibility to another workgroup within the dispatch, which no readback can observe;
    // this shows that the declarations compile, bind and run, and that each write lands
    constexpr auto groups = 4;
    constexpr auto count = groups * 64;
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const partials = ctx->persistent.create_buffer_from_data(cc::vector<u32>::create_defaulted(count), usage);
    auto const counter = ctx->persistent.create_buffer_from_data(cc::vector<u32>::create_defaulted(1), usage);
    auto const mip = make_image(ctx, sg::pixel_format::r32_float, 16, count / 16);

    auto cmd = ctx->create_command_list();
    auto const group
        = ctx->transient.create_binding_group(*cmd, layout,
                                              shaders::handoff{.partials = partials.as_readwrite_buffer(),
                                                               .counter = counter.as_readwrite_buffer(),
                                                               .mip = mip.as_image_view<sg::pixel_format::r32_float>()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(count);
    auto const partials_back = cmd->download.data_from_buffer(partials);
    auto const counter_back = cmd->download.data_from_buffer(counter);
    auto const mip_back = cmd->download.bytes_from_texture(mip.raw());
    ctx->submit_command_list(cc::move(cmd));

    auto const got_partials = co_await partials_back.data();
    REQUIRE(got_partials.size() == isize(count));
    for (auto i = 0; i < count; ++i)
        CHECK(got_partials[i] == u32(i) * 3u);
    auto const got_counter = co_await counter_back.data();
    REQUIRE(got_counter.size() == 1);
    CHECK(got_counter[0] == u32(groups));
    auto const got_mip = co_await mip_back.bytes();
    REQUIRE(got_mip.size() == isize(count) * 4);
    for (auto i = 0; i < count; ++i)
        CHECK(texel_at<float>(got_mip, i) == float(i));
}
