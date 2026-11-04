#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// The SGL fixture in tests/shaders/sgl/float_atomics.sgl.
// slib asks for the device's own float add where the context has `float_atomic_add`, so one test runs both forms:
// the compare-exchange loop on dx12 and WebGPU, the native add on vulkan with VK_EXT_shader_atomic_float and on metal.

ASYNC_INVOCABLE_TEST("sg - an SGL atomic[float] sums every invocation's add, in a buffer and in workgroup memory",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const pipeline = co_await shaders::float_atomics::add_quarters.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::float_atomics::sums>();

    constexpr auto count = 4096;
    auto const totals = ctx->persistent.create_buffer_from_data(
        cc::vector<f32>::create_filled(6, 0.0f), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout, shaders::float_atomics::sums{.totals = totals.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(count);
    auto const future = cmd->download.data_from_buffer(totals);
    ctx->submit_command_list(cc::move(cmd));

    // every partial sum is exact in a float, so any order the GPU adds in gives these bits
    auto const data = co_await future.data();
    REQUIRE(data.size() == 6);
    for (auto k = 0; k < 4; ++k)
        CHECK(data[k] == f32(count / 4) * 0.25f);
    CHECK(data[4] == f32(count / 64) * 32.0f);
    CHECK(data[5] == 1.0f);
}
