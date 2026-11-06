#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

ASYNC_INVOCABLE_TEST("sg - an SGL compute shader adds, subtracts, negates and scales a matrix of every size",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const pipeline = co_await shaders::matrix_arithmetic::matrix_operations.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::matrix_arithmetic::matrix_arithmetic>();

    auto scale = cc::vector<f32>();
    scale.push_back(2.0f);
    auto const scale_buffer = ctx->persistent.create_buffer_from_data(scale, sg::buffer_usage::readonly_buffer);
    auto const results
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec<4, f32>>::create_defaulted(3),
                                                  sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(*cmd, layout,
                                                           shaders::matrix_arithmetic::matrix_arithmetic{
                                                               .scale = scale_buffer.as_readonly_buffer(),
                                                               .results = results.as_readwrite_buffer(),
                                                           });
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(1);
    auto const future = cmd->download.data_from_buffer(results);
    ctx->submit_command_list(cc::move(cmd));

    auto const data = co_await future.data();
    REQUIRE(data.size() == 3);
    CHECK((data[0] == tg::vec<4, f32>(9.0f, 6.0f, 0.0f, 0.0f)));
    CHECK((data[1] == tg::vec<4, f32>(3.0f, 6.0f, 6.0f, 0.0f)));
    CHECK((data[2] == tg::vec<4, f32>(3.0f, 6.0f, 9.0f, 6.0f)));
}
