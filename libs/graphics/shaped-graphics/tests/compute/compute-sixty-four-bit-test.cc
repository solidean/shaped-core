#include "../pipeline/pipeline_harness.hh"
#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>
#include <typed-geometry/linalg/vec.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

namespace sg_test
{
cc::shared_async<cc::unit> check_shared_sixty_four_bit_atomics(sg::context& ctx);
}

// The SGL fixture in tests/shaders/sgl/sixty_four_bit.sgl: long arithmetic that needs every one of its 64 bits, and
// double arithmetic that keeps what a float rounds away.

// The constant block is a long and a ulong2 on the next 16-byte row.
static_assert(shaders::sixty_four_bit::long_sums::constants_size == 32);

ASYNC_INVOCABLE_TEST("sg - an SGL compute shader's longs and ulongs keep all 64 bits", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::shader_int64))
        SKIP("this context has no 64-bit integers");

    auto const pipeline = co_await shaders::sixty_four_bit::sum_longs.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::sixty_four_bit::long_sums>();

    constexpr auto count = 256;
    auto deltas = cc::vector<tg::vec<2, i64>>();
    for (auto i = 0; i < count; ++i)
        deltas.push_back(tg::vec<2, i64>(i64(i) * 1000 - 100'000, i64(i) - 64));
    auto const delta_buffer = ctx->persistent.create_buffer_from_data(deltas, sg::buffer_usage::readonly_buffer);
    auto const totals
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec<2, u64>>::create_defaulted(count),
                                                  sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    constexpr auto seed = (i64(1) << 40) + 12'345;
    auto const mask = tg::vec<2, u64>(0xdead'beef'0000'ffffull, u64(1) << 63);
    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(*cmd, layout,
                                                           shaders::sixty_four_bit::long_sums{
                                                               .seed = seed,
                                                               .mask = mask,
                                                               .deltas = delta_buffer.as_readonly_buffer(),
                                                               .totals = totals.as_readwrite_buffer(),
                                                           });
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(count);
    auto const future = cmd->download.data_from_buffer(totals);
    ctx->submit_command_list(cc::move(cmd));

    // the same steps on the CPU, unsigned where a step wraps
    auto const data = co_await future.data();
    REQUIRE(data.size() == isize(count));
    for (auto i = 0; i < count; ++i)
    {
        auto const d = deltas[i];
        auto const product = d[0] * 3'000'000'000 + seed;
        auto const shifted = (u64(product) << 20) >> 4;
        auto const quotient = product / (d[1] | 1);
        CHECK(data[i][0] == (shifted ^ mask[0]) + mask[1]);
        CHECK(data[i][1] == u64(quotient - d[1]));
    }
}

ASYNC_INVOCABLE_TEST("sg - an SGL compute shader's doubles keep what a float rounds away",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::shader_f64))
        SKIP("this context has no 64-bit floats");

    auto const pipeline = co_await shaders::sixty_four_bit::sum_doubles.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::sixty_four_bit::double_sums>();

    constexpr auto count = 256;
    auto values = cc::vector<tg::vec<2, double>>();
    for (auto i = 0; i < count; ++i)
        values.push_back(tg::vec<2, double>(double(i % 16), -double(i % 5) - 0.5));
    auto const value_buffer = ctx->persistent.create_buffer_from_data(values, sg::buffer_usage::readonly_buffer);
    auto const results
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec<2, double>>::create_defaulted(count),
                                                  sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(*cmd, layout,
                                                           shaders::sixty_four_bit::double_sums{
                                                               .scale = 1024.0,
                                                               .values = value_buffer.as_readonly_buffer(),
                                                               .results = results.as_readwrite_buffer(),
                                                           });
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(count);
    auto const future = cmd->download.data_from_buffer(results);
    ctx->submit_command_list(cc::move(cmd));

    // (x + 2^-40) * 2^10 - x * 2^10 is 2^-30 exactly, in any order a GPU computes it, and 0 in floats
    auto const data = co_await future.data();
    REQUIRE(data.size() == isize(count));
    for (auto i = 0; i < count; ++i)
    {
        CHECK(data[i][0] == 1.0 / 1'073'741'824.0);
        auto const v = values[i];
        CHECK(data[i][1] == (v[1] < -v[0] ? v[1] : -v[0]) / 4.0);
    }
}

ASYNC_INVOCABLE_TEST("sg - an SGL compute shader's 64-bit atomics update all 64 bits of a buffer's element",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::shader_int64_atomics))
        SKIP("this context has no 64-bit atomics");

    auto const pipeline = co_await shaders::sixty_four_bit::tally_longs.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::sixty_four_bit::long_atomics>();

    constexpr auto groups = 4;
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const totals = ctx->persistent.create_buffer_from_data(cc::vector<u64>::create_defaulted(groups), usage);
    auto const lows = ctx->persistent.create_buffer_from_data(cc::vector<i64>::create_defaulted(groups), usage);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout,
        shaders::sixty_four_bit::long_atomics{.totals = totals.as_readwrite_buffer(), .lows = lows.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(groups * 64);
    auto const totals_back = cmd->download.data_from_buffer(totals);
    auto const lows_back = cmd->download.data_from_buffer(lows);
    ctx->submit_command_list(cc::move(cmd));

    // 1 + 2 + ... + 64 is 2080, so each group's element is 2080 * 2^33, and its lowest its last id times -2^40
    auto const got_totals = co_await totals_back.data();
    auto const got_lows = co_await lows_back.data();
    REQUIRE(got_totals.size() == isize(groups));
    REQUIRE(got_lows.size() == isize(groups));
    for (auto g = 0; g < groups; ++g)
    {
        CHECK(got_totals[g] == u64(2080) << 33);
        CHECK(got_lows[g] == -(i64(64 * g + 63) << 40));
    }
}

ASYNC_INVOCABLE_TEST("sg - an SGL compute shader's ulong atomic min and max compare all 64 bits of a buffer's element",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::shader_int64) || !ctx->supports(sg::feature::shader_int64_min_max_atomics))
        SKIP("this context has no 64-bit atomic min and max");

    auto const pipeline = co_await shaders::sixty_four_bit_min_max::extremes.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::sixty_four_bit_min_max::ulong_extremes>();

    constexpr auto groups = 4;
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const lowest = ctx->persistent.create_buffer_from_data(cc::vector<u64>::create_filled(groups, ~u64(0)), usage);
    auto const highest = ctx->persistent.create_buffer_from_data(cc::vector<u64>::create_defaulted(groups), usage);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout,
        shaders::sixty_four_bit_min_max::ulong_extremes{.lowest = lowest.as_readwrite_buffer(),
                                                        .highest = highest.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(groups * 64);
    auto const lowest_back = cmd->download.data_from_buffer(lowest);
    auto const highest_back = cmd->download.data_from_buffer(highest);
    ctx->submit_command_list(cc::move(cmd));

    // id i offers i in the high word and its complement in the low one, so the group's first id is its lowest
    auto const offered = [](u64 i) { return (i << 32) | (u64(0xffffffff) - i); };
    auto const got_lowest = co_await lowest_back.data();
    auto const got_highest = co_await highest_back.data();
    REQUIRE(got_lowest.size() == isize(groups));
    REQUIRE(got_highest.size() == isize(groups));
    for (auto g = 0; g < groups; ++g)
    {
        CHECK(got_lowest[g] == offered(u64(64 * g)));
        CHECK(got_highest[g] == offered(u64(64 * g + 63)));
    }
}

ASYNC_INVOCABLE_TEST("sg - 64-bit values cross from the vertex to the pixel stage flat, all 64 bits (CHK-454, "
                     "EMIT-196)",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::shader_int64) || !ctx->supports(sg::feature::shader_f64)
        || !ctx->supports(sg::feature::shader_int64_atomics))
        SKIP("this context lacks a 64-bit feature the package requires");

    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::sixty_four_bit::long_crossing);
    auto const words = ctx->persistent.create_buffer_from_data(
        cc::vector<u32>::create_defaulted(9), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const group = ctx->persistent.create_binding_group(
        ctx->cached.acquire_binding_group_layout<shaders::sixty_four_bit::crossed>(),
        shaders::sixty_four_bit::crossed{.words = words.as_readwrite_buffer()});
    auto const pixels = co_await sg_test::draw_offscreen(*ctx,
                                                         {.width = 4,
                                                          .height = 4,
                                                          .colors = {sg::pixel_format::rgba8_unorm},
                                                          .target_set = shaders::sixty_four_bit::long_target::name},
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             scope.bind_group(0, *group);
                                                             scope.draw({.vertex_range = {.offset = 0, .size = 3}});
                                                         });
    CHECK(pixels.colors.size() == 1);
    auto cmd = ctx->create_command_list();
    auto const back = cmd->download.data_from_buffer(words);
    ctx->submit_command_list(cc::move(cmd));
    auto const got = co_await back.data();
    REQUIRE(got.size() == 9);
    CHECK(got[0] == 0x09abcdefu);
    CHECK(got[1] == 0x12345678u);
    CHECK(got[2] == 1u);
    CHECK(got[3] == 8u);
    // a double3 and a ulong4 over two locations each, and the member after them
    CHECK(got[4] == 3u + 5u);
    CHECK(got[5] == 2u + 1u + 3u);
    CHECK(got[6] == 7u);
    CHECK(got[7] == 11u);
    CHECK(got[8] == 0x5eedu);
}

/// tests/shaders/sgl/sixty_four_bit_shared.sgl on `ctx`, which must have shader_shared_int64_atomics: four
/// workgroups each sum and take the minimum of 64 values in workgroup memory, and every result needs all 64 bits.
cc::shared_async<cc::unit> sg_test::check_shared_sixty_four_bit_atomics(sg::context& ctx)
{
    auto const pipeline = co_await shaders::sixty_four_bit_shared::shared_sums.acquire_pipeline(ctx);
    auto const layout = ctx.cached.acquire_binding_group_layout<shaders::sixty_four_bit_shared::shared_results>();
    constexpr auto groups = 4;
    auto const totals = ctx.persistent.create_buffer_from_data(
        cc::vector<u64>::create_defaulted(groups), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const lows = ctx.persistent.create_buffer_from_data(
        cc::vector<i64>::create_defaulted(groups), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto cmd = ctx.create_command_list();
    auto const group = ctx.transient.create_binding_group(
        *cmd, layout,
        shaders::sixty_four_bit_shared::shared_results{.totals = totals.as_readwrite_buffer(),
                                                       .lows = lows.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(64 * groups);
    auto const totals_back = cmd->download.data_from_buffer(totals);
    auto const lows_back = cmd->download.data_from_buffer(lows);
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const got_totals = co_await totals_back.data();
    auto const got_lows = co_await lows_back.data();
    REQUIRE(got_totals.size() == groups);
    REQUIRE(got_lows.size() == groups);
    // 64 * 2^33 plus 0 + 1 + ... + 63, and -(2^40) - 63
    for (auto g = 0; g < groups; ++g)
    {
        CHECK(got_totals[g] == (u64(64) << 33) + 2016u).context(cc::format("workgroup {}", g));
        CHECK(got_lows[g] == -(i64(1) << 40) - 63).context(cc::format("workgroup {}", g));
    }
}

ASYNC_INVOCABLE_TEST("sg - an SGL workgroup's 64-bit atomics keep all 64 bits (CHK-510)", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::shader_shared_int64_atomics))
        SKIP("this device has no 64-bit atomics in workgroup memory");
    co_await sg_test::check_shared_sixty_four_bit_atomics(*ctx);
}
