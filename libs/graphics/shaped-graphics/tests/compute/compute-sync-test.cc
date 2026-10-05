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

ASYNC_INVOCABLE_TEST("sg - SGL's workgroup memory, barriers and atomics agree across a workgroup on the GPU",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const pipeline = co_await shaders::sync.reduce.acquire_pipeline(*ctx);
    auto const group_layout = ctx->cached.acquire_binding_group_layout<shaders::tally>();

    constexpr auto groups = 4;
    constexpr auto count = groups * 64;
    auto input = cc::vector<i32>();
    for (auto i = 0; i < count; ++i)
        input.push_back(i);
    auto const values = ctx->persistent.create_buffer_from_data(cc::move(input), sg::buffer_usage::readonly_buffer);
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const sums = ctx->persistent.create_buffer_from_data(cc::vector<i32>::create_defaulted(groups), usage);
    auto const bins = ctx->persistent.create_buffer_from_data(cc::vector<i32>::create_defaulted(4), usage);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(*cmd, group_layout,
                                                           shaders::tally{.values = values.as_readonly_buffer(),
                                                                          .sums = sums.as_readwrite_buffer(),
                                                                          .bins = bins.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(count);
    auto const sums_back = cmd->download.data_from_buffer(sums);
    auto const bins_back = cmd->download.data_from_buffer(bins);
    ctx->submit_command_list(cc::move(cmd));

    // Each sum is only right once the first barrier holds, and each count of 64 only once the second does.
    auto const got_sums = co_await sums_back.data();
    REQUIRE(got_sums.size() == isize(groups));
    for (auto g = 0; g < groups; ++g)
    {
        auto const first = g * 64;
        CHECK(got_sums[g] == (64 * first + 63 * 32) * 1000 + 64);
    }
    auto const got_bins = co_await bins_back.data();
    REQUIRE(got_bins.size() == 4);
    for (auto b = 0; b < 4; ++b)
        CHECK(got_bins[b] == count / 4);
}

ASYNC_INVOCABLE_TEST("sg - an SGL binding array is filled with a view per element and indexed per thread",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::binding_arrays))
        SKIP("this context has no binding arrays");

    auto const pipeline = co_await shaders::binding_arrays.gather.acquire_pipeline(*ctx);
    auto const group_layout = ctx->cached.acquire_binding_group_layout<shaders::lanes>();

    constexpr auto count = 64;
    auto const source = [&](i32 lane)
    {
        auto data = cc::vector<i32>();
        for (auto i = 0; i < count; ++i)
            data.push_back(lane * 1000 + i);
        return ctx->persistent.create_buffer_from_data(cc::move(data), sg::buffer_usage::readonly_buffer);
    };
    auto const s0 = source(0);
    auto const s1 = source(1);
    auto const s2 = source(2);
    auto const merged = ctx->persistent.create_buffer_from_data(
        cc::vector<i32>::create_defaulted(count), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, group_layout,
        shaders::lanes{.sources = {s0.as_readonly_buffer(), s1.as_readonly_buffer(), s2.as_readonly_buffer()},
                       .merged = merged.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    // sg asks the dispatch which elements of a bound array it touches, since the index is the shader's to pick.
    auto const reads = cc::vector<sg::array_buffer_access>{{.index = 0, .access = sg::access_flag::shader_read},
                                                           {.index = 1, .access = sg::access_flag::shader_read},
                                                           {.index = 2, .access = sg::access_flag::shader_read}};
    cmd->compute.declare_array_buffer_access("lanes.sources", reads);
    cmd->compute.dispatch_threads(count);
    auto const back = cmd->download.data_from_buffer(merged);
    ctx->submit_command_list(cc::move(cmd));

    auto const got = co_await back.data();
    REQUIRE(got.size() == isize(count));
    for (auto i = 0; i < count; ++i)
        CHECK(got[i] == (i % 3) * 1000 + i);
}

ASYNC_INVOCABLE_TEST("sg - SGL bytes are loaded and stored a word at a time, and picked per thread from an array",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    constexpr auto count = 64;
    auto const as_bytes = [](sg::buffer<u32> const& b, bool is_written)
    {
        auto const raw = is_written ? b.raw()->as_raw_readwrite() : b.raw()->as_raw_readonly();
        return raw;
    };

    // A record per thread: its scale as float bits, then three ids.
    {
        auto records = cc::vector<u32>();
        for (auto i = 0; i < count; ++i)
        {
            records.push_back(cc::bit_cast<u32>(f32(i) * 0.5f));
            records.push_back(u32(i));
            records.push_back(u32(100 + i));
            records.push_back(u32(200 + i));
        }
        auto const pipeline = co_await shaders::bytes.repack.acquire_pipeline(*ctx);
        auto const layout = ctx->cached.acquire_binding_group_layout<shaders::raw>();
        auto const in = ctx->persistent.create_buffer_from_data(cc::move(records), sg::buffer_usage::readonly_buffer);
        auto const out
            = ctx->persistent.create_buffer_from_data(cc::vector<u32>::create_defaulted(count * 4),
                                                      sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
        auto cmd = ctx->create_command_list();
        auto const group
            = ctx->transient.create_binding_group(*cmd, layout,
                                                  shaders::raw{.records = as_bytes(in, false).as_readonly<byte>(),
                                                               .repacked = as_bytes(out, true).as_readwrite<byte>()});
        cmd->compute.bind_pipeline(*pipeline);
        cmd->compute.bind_group(0, *group);
        cmd->compute.dispatch_threads(count);
        auto const back = cmd->download.data_from_buffer(out);
        ctx->submit_command_list(cc::move(cmd));

        auto const got = co_await back.data();
        REQUIRE(got.size() == isize(count * 4));
        for (auto i = 0; i < count; ++i)
        {
            CHECK(got[i * 4 + 0] == u32(200 + i));
            CHECK(got[i * 4 + 1] == u32(100 + i));
            CHECK(got[i * 4 + 2] == u32(i));
            CHECK(cc::bit_cast<f32>(got[i * 4 + 3]) == f32(i));
        }
    }

    // Three blocks, each thread reading its own word of the block its index picks.
    if (!ctx->supports(sg::feature::binding_arrays))
        co_return;
    auto const pipeline = co_await shaders::bytes.pick.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::blocks>();
    auto const block = [&](u32 k)
    {
        auto data = cc::vector<u32>();
        for (auto i = 0; i < count; ++i)
            data.push_back(k * 1000 + u32(i));
        return ctx->persistent.create_buffer_from_data(cc::move(data), sg::buffer_usage::readonly_buffer);
    };
    auto const b0 = block(0);
    auto const b1 = block(1);
    auto const b2 = block(2);
    auto const merged = ctx->persistent.create_buffer_from_data(
        cc::vector<u32>::create_defaulted(count), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout,
        shaders::blocks{.tables = {as_bytes(b0, false).as_readonly<byte>(), as_bytes(b1, false).as_readonly<byte>(),
                                   as_bytes(b2, false).as_readonly<byte>()},
                        .merged = merged.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    auto const reads = cc::vector<sg::array_buffer_access>{{.index = 0, .access = sg::access_flag::shader_read},
                                                           {.index = 1, .access = sg::access_flag::shader_read},
                                                           {.index = 2, .access = sg::access_flag::shader_read}};
    cmd->compute.declare_array_buffer_access("blocks.tables", reads);
    cmd->compute.dispatch_threads(count);
    auto const back = cmd->download.data_from_buffer(merged);
    ctx->submit_command_list(cc::move(cmd));

    auto const got = co_await back.data();
    REQUIRE(got.size() == isize(count));
    for (auto i = 0; i < count; ++i)
        CHECK(got[i] == u32(i % 3) * 1000 + u32(i));
}

ASYNC_INVOCABLE_TEST("sg - SGL's quad swap, subgroup sum and uniform load agree with the CPU at any subgroup size",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::subgroups))
        SKIP("this context has no subgroup operations");

    auto const pipeline = co_await shaders::subgroups.reduce.acquire_pipeline(*ctx);
    auto const group_layout = ctx->cached.acquire_binding_group_layout<shaders::wave_io>();

    constexpr auto groups = 4;
    constexpr auto count = groups * 64;
    auto input = cc::vector<u32>();
    for (auto i = 0; i < count; ++i)
        input.push_back(u32(i * 3 + 1));
    auto const values = ctx->persistent.create_buffer_from_data(cc::move(input), sg::buffer_usage::readonly_buffer);
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const swapped = ctx->persistent.create_buffer_from_data(cc::vector<u32>::create_defaulted(count), usage);
    auto const sums = ctx->persistent.create_buffer_from_data(cc::vector<u32>::create_defaulted(count), usage);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(*cmd, group_layout,
                                                           shaders::wave_io{.values = values.as_readonly_buffer(),
                                                                            .swapped = swapped.as_readwrite_buffer(),
                                                                            .sums = sums.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(count);
    auto const swapped_back = cmd->download.data_from_buffer(swapped);
    auto const sums_back = cmd->download.data_from_buffer(sums);
    ctx->submit_command_list(cc::move(cmd));

    // every invocation's horizontal neighbour differs from it in the lowest bit of its index alone
    auto const got_swapped = co_await swapped_back.data();
    REQUIRE(got_swapped.size() == isize(count));
    for (auto i = 0; i < count; ++i)
        CHECK(got_swapped[i] == 1u);
    // the subgroups' sums add up to the workgroup's, which the uniform load hands every thread of it
    auto const got_sums = co_await sums_back.data();
    REQUIRE(got_sums.size() == isize(count));
    for (auto i = 0; i < count; ++i)
    {
        auto const first = i / 64 * 64;
        auto expected = u32(0);
        for (auto k = first; k < first + 64; ++k)
            expected += u32(k * 3 + 1);
        CHECK(got_sums[i] == expected);
    }
}
