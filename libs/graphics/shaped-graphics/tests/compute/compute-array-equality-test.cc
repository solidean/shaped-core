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

ASYNC_INVOCABLE_TEST("sg - an SGL compute shader compares two arrays element by element", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const pipeline = co_await shaders::array_equality::compare_arrays.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::array_equality::array_equality>();

    auto values = cc::vector<i32>();
    for (auto const v : {1, 2, 1, 3})
        values.push_back(v);
    auto const value_buffer = ctx->persistent.create_buffer_from_data(values, sg::buffer_usage::readonly_buffer);
    auto const results = ctx->persistent.create_buffer_from_data(
        cc::vector<i32>::create_defaulted(1), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(*cmd, layout,
                                                           shaders::array_equality::array_equality{
                                                               .values = value_buffer.as_readonly_buffer(),
                                                               .results = results.as_readwrite_buffer(),
                                                           });
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(1);
    auto const future = cmd->download.data_from_buffer(results);
    ctx->submit_command_list(cc::move(cmd));

    auto const data = co_await future.data();
    REQUIRE(data.size() == 1);
    CHECK(data[0] == 15);
}
