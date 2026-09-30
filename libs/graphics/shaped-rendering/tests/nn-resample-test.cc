#include "shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <sr_sgl_shaders.hh>

using namespace cc::primitive_defines;

// The network's max pool and nearest upsample, which are SGL and so run on every backend.

namespace
{
/// Builds one SGL entry point's compute pipeline into `out`, failing with the compiler's own text rather than a null handle.
///
/// The pipeline travels through a reference rather than the return, because what this has to report is a compiler
/// message and the coroutine's value would be the pipeline.
template <class Entry>
[[nodiscard]] cc::shared_async<cc::unit> build_sgl(sg::context& ctx,
                                                   Entry const& entry,
                                                   cc::string_view name,
                                                   sg::compute_pipeline_handle& out)
{
    auto const shader = entry->acquire(ctx);
    co_await cc::async_settled(shader);
    if (shader->has_error())
        FAIL(cc::format("{} did not compile:\n{}", name, shader->try_error()->underlying().to_string()));

    auto const* const compiled = shader->try_value();
    REQUIRE(compiled != nullptr);

    auto const built
        = co_await ctx.cached.acquire_compute_pipeline({.shader = *compiled, .layout = entry.acquire_layout(ctx)});
    REQUIRE(built != nullptr).context(cc::format("{} did not build a pipeline", name));
    out = built;
    co_return;
}
} // namespace

// The max pool and the nearest upsample, which are the only other things the network does to a feature map.
//
// Both are trivial and both are easy to get subtly wrong: a pool that takes the wrong four texels, or an upsample
// that writes three of its four, produces a feature map that is the right size and the wrong content.
ASYNC_INVOCABLE_TEST("sr - the network's pool and upsample move the texels they say they do",
                     (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    auto const pool_layout = ctx.cached.acquire_binding_group_layout<sr::sgl_shaders::nn_pool_features>();
    auto const up_layout = ctx.cached.acquire_binding_group_layout<sr::sgl_shaders::nn_upsample_features>();
    auto pool_pipeline = sg::compute_pipeline_handle();
    auto up_pipeline = sg::compute_pipeline_handle();
    co_await build_sgl(ctx, sr::sgl_shaders::nn_pool.main_cs, "nn_pool", pool_pipeline);
    co_await build_sgl(ctx, sr::sgl_shaders::nn_upsample.main_cs, "nn_upsample", up_pipeline);

    // A 4x4 map of two channels, every value distinct so a wrong texel is a wrong number.
    constexpr auto src_w = 4;
    constexpr auto src_h = 4;
    constexpr auto channels = 2;

    // Which corner of a 2x2 block holds its maximum, 0 to 3 as top-left, top-right, bottom-left, bottom-right.
    // It differs per block and per channel, so a pool that returns any fixed corner gets some block wrong.
    auto const max_corner = [](int bx, int by, int c) { return (bx + 2 * by + c) % 4; };
    auto const value_at = [&](int x, int y, int c)
    {
        auto const corner = (x % 2) + 2 * (y % 2);
        auto const peak = corner == max_corner(x / 2, y / 2, c) ? 1000.0f : 0.0f;
        return peak + f32((y * src_w + x) * channels + c);
    };

    auto source = cc::vector<f32>();
    for (auto y = 0; y < src_h; ++y)
        for (auto x = 0; x < src_w; ++x)
            for (auto c = 0; c < channels; ++c)
                source.push_back(value_at(x, y, c));

    auto cmd = ctx.create_command_list();

    auto const src = ctx.transient.create_buffer<f32>(source.size(),
                                                      sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    cmd->upload.data_to_buffer(src, source);

    auto const pooled = ctx.transient.create_buffer<f32>(
        (src_w / 2) * (src_h / 2) * channels,
        sg::buffer_usage::readonly_buffer | sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    cmd->compute.bind_pipeline(*pool_pipeline);
    cmd->compute.bind_group(0, *ctx.transient.create_binding_group(
                                   *cmd, pool_layout,
                                   sr::sgl_shaders::nn_pool_features{.source = src.as_readonly_buffer(),
                                                                     .target = pooled.as_readwrite_buffer()}));
    cmd->compute.set_inline_constants(
        sr::sgl_shaders::nn_pool_constants{.width = src_w / 2, .height = src_h / 2, .channels = channels}.to_block());
    cmd->compute.dispatch_threads(channels, src_w / 2, src_h / 2);

    auto const up = ctx.transient.create_buffer<f32>(src_w * src_h * channels,
                                                     sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    cmd->compute.bind_pipeline(*up_pipeline);
    cmd->compute.bind_group(0, *ctx.transient.create_binding_group(
                                   *cmd, up_layout,
                                   sr::sgl_shaders::nn_upsample_features{.source = pooled.as_readonly_buffer(),
                                                                         .target = up.as_readwrite_buffer()}));
    cmd->compute.set_inline_constants(
        sr::sgl_shaders::nn_upsample_constants{.width = src_w / 2, .height = src_h / 2, .channels = channels}.to_block());
    cmd->compute.dispatch_threads(channels, src_w / 2, src_h / 2);

    auto const pooled_back = sg::data_future<f32>(cmd->download.data_from_buffer(pooled));
    auto const up_back = sg::data_future<f32>(cmd->download.data_from_buffer(up));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const got_pool = co_await pooled_back.data();
    auto const got_up = co_await up_back.data();

    // The pool takes the maximum of the 2x2 block, which is the corner `max_corner` put the peak in.
    for (auto y = 0; y < src_h / 2; ++y)
        for (auto x = 0; x < src_w / 2; ++x)
            for (auto c = 0; c < channels; ++c)
            {
                auto const corner = max_corner(x, y, c);
                auto const expected = value_at(x * 2 + corner % 2, y * 2 + corner / 2, c);
                auto const got = got_pool[(y * (src_w / 2) + x) * channels + c];
                CHECK(got == expected)
                    .context(cc::format("pool at {},{} channel {}: got {}, expected {}", x, y, c, got, expected));
            }

    // The upsample writes each pooled texel to all four under it, so every 2x2 block is uniform and equals it.
    for (auto y = 0; y < src_h; ++y)
        for (auto x = 0; x < src_w; ++x)
            for (auto c = 0; c < channels; ++c)
            {
                auto const expected = got_pool[((y / 2) * (src_w / 2) + (x / 2)) * channels + c];
                auto const got = got_up[(y * src_w + x) * channels + c];
                CHECK(got == expected).context(cc::format("upsample at {},{} channel {}", x, y, c));
            }
}
