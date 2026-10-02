#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/half_float.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// The SGL fixture in tests/shaders/sgl/sixteen_bit.sgl: half arithmetic over a constant block and buffers of halves,
// and short arithmetic that wraps at 16 bits.

// The generated structs are the layout: a half3 packs at 2 bytes in a buffer's element.
static_assert(sizeof(shaders::weighted) == 12);
static_assert(offsetof(shaders::weighted, tint) == 4);
static_assert(offsetof(shaders::weighted, scale) == 10);

ASYNC_INVOCABLE_TEST("sg - an SGL compute shader computes in halves over a block and buffers of them",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::shader_f16))
        SKIP("this context has no 16-bit floats");

    auto const pipeline = co_await shaders::sixteen_bit.mix_halves.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::blend>();

    // every value and every step is exact in a half, so any rounding or fusing a GPU picks gives these bits
    constexpr auto count = 256;
    auto inputs = cc::vector<shaders::weighted>();
    for (auto i = 0; i < count; ++i)
        inputs.push_back({.weight = float(i % 8),
                          .tint = tg::vec<3, tg::f16>(tg::f16(i % 16), tg::f16(0.5f), tg::f16(-1.0f)),
                          .scale = tg::f16(2.0f)});
    auto const input_buffer
        = ctx->persistent.create_buffer_from_data(cc::move(inputs), sg::buffer_usage::readonly_buffer);
    auto const halves
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec<2, tg::f16>>::create_defaulted(count),
                                                  sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(*cmd, layout,
                                                           shaders::blend{.gain = tg::f16(4.0f),
                                                                          .offset = 0.5f,
                                                                          .inputs = input_buffer.as_readonly_buffer(),
                                                                          .halves = halves.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(count);
    auto const future = cmd->download.data_from_buffer(halves);
    ctx->submit_command_list(cc::move(cmd));

    // (2a + 2) + 2 - 1.5 + (b + 0.5) for a tint of a and a weight of b
    auto const data = co_await future.data();
    REQUIRE(data.size() == isize(count));
    for (auto i = 0; i < count; ++i)
    {
        auto const sum = float(2 * (i % 16) + 3 + i % 8);
        CHECK(float(data[i][0]) == sum);
        CHECK(float(data[i][1]) == -sum);
    }
}

ASYNC_INVOCABLE_TEST("sg - an SGL compute shader's shorts and ushorts wrap at 16 bits", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::shader_int16))
        SKIP("this context has no 16-bit integers");

    auto const pipeline = co_await shaders::sixteen_bit.sum_shorts.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::short_sums>();

    constexpr auto count = 256;
    auto deltas = cc::vector<tg::vec<2, i16>>();
    for (auto i = 0; i < count; ++i)
        deltas.push_back(tg::vec<2, i16>(i16(i - 128), i16(-2 * i)));
    auto const delta_buffer
        = ctx->persistent.create_buffer_from_data(cc::move(deltas), sg::buffer_usage::readonly_buffer);
    auto const totals
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec<2, u16>>::create_defaulted(count),
                                                  sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout,
        shaders::short_sums{.deltas = delta_buffer.as_readonly_buffer(), .totals = totals.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(count);
    auto const future = cmd->download.data_from_buffer(totals);
    ctx->submit_command_list(cc::move(cmd));

    // 3(i - 128) - 2i is i - 384, which a ushort holds as its value modulo 65536, and so on for every step
    auto const data = co_await future.data();
    REQUIRE(data.size() == isize(count));
    for (auto i = 0; i < count; ++i)
    {
        CHECK(data[i][0] == u16(u16(u16(i - 384) * 2u) + 40000u));
        CHECK(data[i][1] == u16(u16(-2 * i) - 1u));
    }
}
