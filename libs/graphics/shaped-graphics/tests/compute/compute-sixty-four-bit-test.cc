#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <typed-geometry/linalg/vec.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

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
