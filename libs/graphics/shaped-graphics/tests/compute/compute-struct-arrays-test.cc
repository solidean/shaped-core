#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// SGL's DXIL splits an array of structs a column per field and lane (EMIT-293), which the native-dxil sweep runs.
// The values each entry point of struct_arrays.sgl writes, per its comments, with `params` [3, 2, 0].

ASYNC_INVOCABLE_TEST("sg - an SGL array of structs keeps each field, in a variable, in workgroup memory and in a table",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const locals = co_await shaders::struct_arrays::split_locals.acquire_pipeline(*ctx);
    auto const shared = co_await shaders::struct_arrays::split_shared.acquire_pipeline(*ctx);
    auto const table = co_await shaders::struct_arrays::split_table.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::struct_arrays::columns>();

    auto params = cc::vector<i32>();
    for (auto const v : {3, 2, 0})
        params.push_back(v);
    auto const param_buffer = ctx->persistent.create_buffer_from_data(params, sg::buffer_usage::readonly_buffer);
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const local_results = ctx->persistent.create_buffer_from_data(cc::vector<f32>::create_defaulted(34), usage);
    auto const shared_results = ctx->persistent.create_buffer_from_data(cc::vector<f32>::create_defaulted(128), usage);
    auto const table_results = ctx->persistent.create_buffer_from_data(cc::vector<f32>::create_defaulted(3), usage);

    auto cmd = ctx->create_command_list();
    auto const run = [&](auto const& pipeline, auto const& results, i32 threads)
    {
        auto const group = ctx->transient.create_binding_group(*cmd, layout,
                                                               shaders::struct_arrays::columns{
                                                                   .params = param_buffer.as_readonly_buffer(),
                                                                   .results = results.as_readwrite_buffer(),
                                                               });
        cmd->compute.bind_pipeline(*pipeline);
        cmd->compute.bind_group(0, *group);
        cmd->compute.dispatch_threads(threads);
    };
    run(locals, local_results, 1);
    run(shared, shared_results, 64);
    run(table, table_results, 1);
    auto const local_future = cmd->download.data_from_buffer(local_results);
    auto const shared_future = cmd->download.data_from_buffer(shared_results);
    auto const table_future = cmd->download.data_from_buffer(table_results);
    ctx->submit_command_list(cc::move(cmd));

    auto const from_locals = co_await local_future.data();
    REQUIRE(from_locals.size() == 34);
    for (auto k = 0; k < 4; ++k)
    {
        // the first three elements take the field write and the helper's, and element 2 the lane write
        auto const f = f32(k);
        auto const is_counted = k < 3;
        f32 const expected[] = {f,      is_counted ? 100 + f : 10 + f,  20 + f, 30 + f,
                                40 + f, is_counted ? 1050 + f : 50 + f, 60 + f, k == 2 ? -1.0f : 20 + f};
        for (auto l = 0; l < 8; ++l)
            CHECK(from_locals[k * 8 + l] == expected[l]).context(cc::format("element {} leaf {}", k, l));
    }
    CHECK(from_locals[32] == 23 + 53);
    CHECK(from_locals[33] == 63);

    auto const from_shared = co_await shared_future.data();
    REQUIRE(from_shared.size() == 128);
    for (auto i = 0; i < 64; ++i)
    {
        auto const other = f32((i + 3) % 64);
        CHECK(from_shared[i * 2] == 60 + 2 * other).context(cc::format("invocation {}", i));
        CHECK(from_shared[i * 2 + 1] == 60 + other).context(cc::format("invocation {}", i));
    }

    auto const from_table = co_await table_future.data();
    REQUIRE(from_table.size() == 3);
    CHECK(from_table[0] == 57);
    CHECK(from_table[1] == 5);
    CHECK(from_table[2] == 67);
}
