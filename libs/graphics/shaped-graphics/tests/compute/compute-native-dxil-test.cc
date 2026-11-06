#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_native_dxil.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::native_dxil;

// The route the "sg dx12 native-dxil backend" sweep runs every test body through, which only this file can tell apart.

ASYNC_INVOCABLE_TEST("sg - a package that states NATIVE_DXIL takes SGL's DXIL on any dx12 context, and it runs",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    auto const is_dxil = [&]
    {
        for (auto const format : ctx->accepted_shader_formats())
            return format == sg::shader_format::dxil;
        return false;
    }();
    if (!is_dxil)
        SKIP("this context takes no DXIL");

    auto const shader = co_await shaders::native_dxil::fill->acquire(*ctx);
    CHECK(shader.compiler.name == "sgl");
    CHECK(shader.format == sg::shader_format::dxil);

    auto const pipeline = co_await shaders::native_dxil::fill.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::native_dxil::filled>();
    auto const results = ctx->persistent.create_buffer_from_data(
        cc::vector<i32>::create_defaulted(64), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout, shaders::native_dxil::filled{.values = results.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(64);
    auto const future = cmd->download.data_from_buffer(results);
    ctx->submit_command_list(cc::move(cmd));

    auto const data = co_await future.data();
    REQUIRE(data.size() == 64);
    for (auto i = 0; i < 64; ++i)
        CHECK(data[i] == i * 2 + 1);
}
