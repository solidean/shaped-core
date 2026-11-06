#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/vec.hh>

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

ASYNC_INVOCABLE_TEST("sg - an SGL array of mat3 two dimensions deep keeps every column, read and written at any level",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const pipeline = co_await shaders::array_equality::nest_matrices.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::array_equality::nest>();

    // no matrix here is symmetric, and each third column differs, which is the one DXC's SPIR-V lost
    auto const turn
        = [](f32 k) { return tg::mat3f::make_from_cols(tg::vec3f(1, k, 0), tg::vec3f(0, 1, 2), tg::vec3f(3, 4, k)); };
    auto items = cc::vector<shaders::array_equality::nested>::create_defaulted(3);
    for (auto i = 0; i < 2; ++i)
        for (auto k = 0; k < 2; ++k)
            items[0].turns[i][k] = turn(f32(10 * i + k + 1));
    items[0].tag = 7.0f;

    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const item_buffer = ctx->persistent.create_buffer_from_data(cc::move(items), usage);
    auto const picked = ctx->persistent.create_buffer<tg::vec4f>(2, usage);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(*cmd, layout,
                                                           shaders::array_equality::nest{
                                                               .items = item_buffer.as_readwrite_buffer(),
                                                               .picked = picked.as_readwrite_buffer(),
                                                           });
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(1);
    auto const items_back = cmd->download.data_from_buffer(item_buffer);
    auto const picked_back = cmd->download.data_from_buffer(picked);
    ctx->submit_command_list(cc::move(cmd));

    auto const got_items = co_await items_back.data();
    auto const got_picked = co_await picked_back.data();
    REQUIRE(got_items.size() == 3);
    REQUIRE(got_picked.size() == 2);
    CHECK((got_picked[0] == tg::vec4f(3, 4, 12, 7)));
    CHECK((got_picked[1] == tg::vec4f(3, 4, 11, 0)));
    CHECK(tg::mat3f(got_items[1].turns[1][0]) == turn(11));
    CHECK(tg::mat3f(got_items[1].turns[0][1]) == turn(12));
    CHECK(got_items[1].tag == 7.0f);
    CHECK(tg::mat3f(got_items[2].turns[0][0]) == turn(11));
    CHECK(tg::mat3f(got_items[2].turns[0][1]) == turn(12));
}
