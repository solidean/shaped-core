#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/binding/staging_binding_group.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// dispatch.sgl's entry points, dispatched and read back: the dispatch shapes, inline constants set per dispatch, and
// what a binding group's views and lifetimes mean once the GPU runs them.

ASYNC_INVOCABLE_TEST("sg - dispatch_threads rounds each axis up to whole workgroups, and inline constants change per "
                     "dispatch",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // `stamp` runs in 4 × 2 workgroups over a grid 16 cells wide, and each thread stamps its cell.
    // dispatch_threads(10, 3) rounds up to 3 × 2 workgroups, so 12 × 4 threads run, at rows 0 to 3.
    // dispatch_groups(2, 1) runs 8 × 2 threads, which the second dispatch's inline row moves to rows 4 and 5.
    constexpr int width = 16;
    constexpr int height = 8;
    auto const pipeline = co_await shaders::dispatch.stamp.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::grid>();
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const stamps
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4i>::create_defaulted(width * height), usage);
    auto const counts = ctx->persistent.create_buffer_from_data(cc::vector<i32>::create_defaulted(width * height), usage);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout, shaders::grid{.stamps = stamps.as_readwrite_buffer(), .counts = counts.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.set_inline_constants(shaders::pass{.row = 0, .marker = 1});
    cmd->compute.dispatch_threads(10, 3);
    cmd->compute.set_inline_constants(shaders::pass{.row = 4, .marker = 2});
    cmd->compute.dispatch_groups(2, 1);
    auto const stamps_back = cmd->download.data_from_buffer(stamps);
    auto const counts_back = cmd->download.data_from_buffer(counts);
    ctx->submit_command_list(cc::move(cmd));

    auto const got = co_await stamps_back.data();
    auto const got_counts = co_await counts_back.data();
    REQUIRE(got.size() == width * height);
    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
        {
            auto const first = y < 4 && x < 12;
            auto const second = y >= 4 && y < 6 && x < 8;
            auto const thread_y = second ? y - 4 : y;
            auto const local = (x % 4) + 4 * (thread_y % 2);
            auto const expected = first || second
                                    ? tg::vec4i(x, thread_y, x / 4 + 10 * (thread_y / 2), local * 1000 + (first ? 1 : 2))
                                    : tg::vec4i(0, 0, 0, 0);
            CHECK(got[y * width + x] == expected).context(cc::format("cell ({}, {})", x, y));
            // a workgroup's first thread writes how many threads its workgroup counted: all 8, even at the edge
            CHECK(got_counts[y * width + x] == (local == 0 && (first || second) ? 8 : 0))
                .context(cc::format("count at ({}, {})", x, y));
        }
}

ASYNC_INVOCABLE_TEST("sg - int, uint and float4 buffer elements keep their types, through persistent, transient and "
                     "staging groups alike",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // A shift of -8 is arithmetic on an int and gives -4; 0x80000010 is shifted logically as a uint.
    // A float4 comes back reversed and doubled.
    constexpr int count = 64;
    auto const pipeline = co_await shaders::dispatch.typed_step.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::typed>();
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;

    struct buffers
    {
        sg::buffer<i32> ints;
        sg::buffer<u32> uints;
        sg::buffer<tg::vec4f> vectors;
    };
    auto const make = [&]
    {
        return buffers{
            .ints = ctx->persistent.create_buffer_from_data(cc::vector<i32>::create_filled(count, -8), usage),
            .uints = ctx->persistent.create_buffer_from_data(cc::vector<u32>::create_filled(count, 0x80000010u), usage),
            .vectors = ctx->persistent.create_buffer_from_data(
                cc::vector<tg::vec4f>::create_filled(count, tg::vec4f(1, 2, 3, 4)), usage),
        };
    };
    auto const views = [](buffers const& b)
    {
        return shaders::typed{.ints = b.ints.as_readwrite_buffer(),
                              .uints = b.uints.as_readwrite_buffer(),
                              .vectors = b.vectors.as_readwrite_buffer()};
    };
    buffers const all[] = {make(), make(), make()};

    auto cmd = ctx->create_command_list();
    auto const persistent = ctx->persistent.create_binding_group(layout, views(all[0]));
    auto const transient = ctx->transient.create_binding_group(*cmd, layout, views(all[1]));
    // A staging group is set binding by binding, by the name the generated struct gives each member.
    auto staging = ctx->persistent.create_staging_binding_group(layout);
    staging->set_binding("typed.ints", all[2].ints.as_readwrite_buffer());
    staging->set_binding("typed.uints", all[2].uints.as_readwrite_buffer());
    staging->set_binding("typed.vectors", all[2].vectors.as_readwrite_buffer());
    auto const snapshot = staging->snapshot();
    cmd->compute.bind_pipeline(*pipeline);
    for (auto const* group : {persistent.get(), transient.get(), snapshot.get()})
    {
        cmd->compute.bind_group(0, *group);
        cmd->compute.dispatch_threads(count);
    }
    auto futures = cc::vector<sg::data_future<i32>>();
    auto ufutures = cc::vector<sg::data_future<u32>>();
    auto vfutures = cc::vector<sg::data_future<tg::vec4f>>();
    for (auto const& b : all)
    {
        futures.push_back(cmd->download.data_from_buffer(b.ints));
        ufutures.push_back(cmd->download.data_from_buffer(b.uints));
        vfutures.push_back(cmd->download.data_from_buffer(b.vectors));
    }
    ctx->submit_command_list(cc::move(cmd));

    constexpr char const* kinds[] = {"persistent", "transient", "staging"};
    for (auto k = 0; k < 3; ++k)
    {
        auto const ints = co_await futures[k].data();
        auto const uints = co_await ufutures[k].data();
        auto const vectors = co_await vfutures[k].data();
        for (auto i = 0; i < count; ++i)
        {
            CHECK(ints[i] == -4).context(cc::format("{} group, element {}", kinds[k], i));
            CHECK(uints[i] == 0x40000008u).context(cc::format("{} group, element {}", kinds[k], i));
            CHECK(vectors[i] == tg::vec4f(8, 6, 4, 2)).context(cc::format("{} group, element {}", kinds[k], i));
        }
    }
}

ASYNC_INVOCABLE_TEST("sg - a buffer view's offset is where the shader's index 0 lands, and a read-only and a "
                     "read-write view of one buffer work side by side",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // One buffer of 4 × 64 ints: quarter 1 is the source view, quarter 2 the target view, and quarters 0 and 3 are
    // bystanders a view that ignored its offset or its size would reach.
    // 64 ints are 256 bytes, which is the strictest storage-offset alignment any backend asks for.
    constexpr int quarter = 64;
    auto const pipeline = co_await shaders::dispatch.copy_within.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::within>();
    auto initial = cc::vector<i32>();
    for (auto i = 0; i < 4 * quarter; ++i)
        initial.push_back(i);
    auto const all = ctx->persistent.create_buffer_from_data(cc::move(initial), sg::buffer_usage::readonly_buffer
                                                                                    | sg::buffer_usage::readwrite_buffer
                                                                                    | sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout,
        shaders::within{.source = all.as_readonly_buffer({.offset = quarter, .size = quarter}),
                        .target = all.as_readwrite_buffer({.offset = 2 * quarter, .size = quarter})});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(quarter);
    auto const back = cmd->download.data_from_buffer(all);
    ctx->submit_command_list(cc::move(cmd));

    auto const got = co_await back.data();
    REQUIRE(got.size() == 4 * quarter);
    for (auto i = 0; i < 4 * quarter; ++i)
    {
        auto const expected = i >= 2 * quarter && i < 3 * quarter ? i - quarter + 1 : i;
        CHECK(got[i] == expected).context(cc::format("element {}", i));
    }
}

ASYNC_INVOCABLE_TEST("sg - two groups rebound between dispatches, and one group shared by two pipelines of one layout",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // double_values.sgl: `scaled` multiplies `work` by `factor`, and `main` doubles `work`, one layout for `work` in
    // both, at slot 1 in `scaled` and slot 0 in `main`.
    // Three dispatches: scaled by 3 through one factor group, scaled by 5 through another rebound in its place,
    // then doubled by `main` through the same `work` group: 1 × 3 × 5 × 2.
    constexpr int count = 64;
    auto const scaled = co_await shaders::double_values.scaled.acquire_pipeline(*ctx);
    auto const doubled = co_await shaders::double_values.main.acquire_pipeline(*ctx);
    auto const factor_layout = ctx->cached.acquire_binding_group_layout<shaders::factor>();
    auto const work_layout = ctx->cached.acquire_binding_group_layout<shaders::work>();
    auto const values = ctx->persistent.create_buffer_from_data(
        cc::vector<float>::create_filled(count, 1.0f), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const three
        = ctx->persistent.create_buffer_from_data(cc::vector<float>{3.0f}, sg::buffer_usage::readonly_buffer);
    auto const five = ctx->persistent.create_buffer_from_data(cc::vector<float>{5.0f}, sg::buffer_usage::readonly_buffer);

    auto const work
        = ctx->persistent.create_binding_group(work_layout, shaders::work{.values = values.as_readwrite_buffer()});
    auto const by_three
        = ctx->persistent.create_binding_group(factor_layout, shaders::factor{.by = three.as_readonly_buffer()});
    auto const by_five
        = ctx->persistent.create_binding_group(factor_layout, shaders::factor{.by = five.as_readonly_buffer()});

    auto cmd = ctx->create_command_list();
    cmd->compute.bind_pipeline(*scaled);
    cmd->compute.bind_group(0, *by_three);
    cmd->compute.bind_group(1, *work);
    cmd->compute.dispatch_threads(count);
    cmd->compute.bind_group(0, *by_five);
    cmd->compute.dispatch_threads(count);
    cmd->compute.bind_pipeline(*doubled);
    cmd->compute.bind_group(0, *work);
    cmd->compute.dispatch_threads(count);
    auto const back = cmd->download.data_from_buffer(values);
    ctx->submit_command_list(cc::move(cmd));

    auto const got = co_await back.data();
    REQUIRE(got.size() == count);
    for (auto i = 0; i < count; ++i)
        CHECK(got[i] == 30.0f).context(cc::format("element {}", i));
}
