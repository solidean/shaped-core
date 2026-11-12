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

ASYNC_INVOCABLE_TEST("sg - SGL's workgroup memory, barriers and atomics agree across a workgroup on the GPU",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const pipeline = co_await shaders::sync::reduce.acquire_pipeline(*ctx);
    auto const group_layout = ctx->cached.acquire_binding_group_layout<shaders::sync::tally>();

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
                                                           shaders::sync::tally{.values = values.as_readonly_buffer(),
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

ASYNC_INVOCABLE_TEST("sg - SGL's strong and weak compare-exchange each claim every slot of a hash table once, under "
                     "every thread at once",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // `insert` takes the strong one, and `insert_weak` the weak one in its retry loop: the same table either way
    auto const strong = co_await shaders::sync::insert.acquire_pipeline(*ctx);
    auto const weak = co_await shaders::sync::insert_weak.acquire_pipeline(*ctx);
    auto const group_layout = ctx->cached.acquire_binding_group_layout<shaders::sync::table>();
    for (auto const is_weak : {false, true})
    {
        auto const* const kind = is_weak ? "weak" : "strong";
        constexpr auto slot_count = 1024;
        constexpr auto count = 4 * slot_count;
        constexpr auto groups = count / 64;
        auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
        auto const slots = ctx->persistent.create_buffer_from_data(cc::vector<u32>::create_defaulted(slot_count), usage);
        auto const found_at = ctx->persistent.create_buffer_from_data(cc::vector<u32>::create_defaulted(count), usage);
        auto const winners = ctx->persistent.create_buffer_from_data(cc::vector<i32>::create_defaulted(groups), usage);

        auto cmd = ctx->create_command_list();
        auto const group
            = ctx->transient.create_binding_group(*cmd, group_layout,
                                                  shaders::sync::table{.slots = slots.as_readwrite_buffer(),
                                                                       .found_at = found_at.as_readwrite_buffer(),
                                                                       .winners = winners.as_readwrite_buffer()});
        cmd->compute.bind_pipeline(is_weak ? *weak : *strong);
        cmd->compute.bind_group(0, *group);
        cmd->compute.dispatch_threads(count);
        auto const slots_back = cmd->download.data_from_buffer(slots);
        auto const found_back = cmd->download.data_from_buffer(found_at);
        auto const winners_back = cmd->download.data_from_buffer(winners);
        ctx->submit_command_list(cc::move(cmd));

        // 1024 keys fill 1024 slots, so every key stands exactly once, and every thread found the slot that holds its key
        auto const got_slots = co_await slots_back.data();
        REQUIRE(got_slots.size() == isize(slot_count));
        auto seen = cc::vector<i32>::create_defaulted(slot_count + 1);
        for (auto const key : got_slots)
        {
            REQUIRE(key >= 1u);
            REQUIRE(key <= u32(slot_count));
            ++seen[key];
        }
        for (auto key = 1; key <= slot_count; ++key)
            CHECK(seen[key] == 1).context(cc::format("{}: key {}", kind, key));
        auto const got_found = co_await found_back.data();
        REQUIRE(got_found.size() == isize(count));
        for (auto i = 0; i < count; ++i)
        {
            REQUIRE(got_found[i] < u32(slot_count));
            CHECK(got_slots[got_found[i]] == u32(i % slot_count + 1)).context(cc::format("{}: thread {}", kind, i));
        }

        // of the 64 threads of a workgroup that race to replace -1, exactly one wins
        auto const got_winners = co_await winners_back.data();
        REQUIRE(got_winners.size() == isize(groups));
        for (auto g = 0; g < groups; ++g)
            CHECK(got_winners[g] == 1).context(cc::format("{}: workgroup {}", kind, g));
    }
}

ASYNC_INVOCABLE_TEST("sg - an SGL binding array is filled with a view per element and indexed per thread",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::binding_arrays))
        SKIP("this context has no binding arrays");

    auto const pipeline = co_await shaders::binding_arrays::gather.acquire_pipeline(*ctx);
    auto const group_layout = ctx->cached.acquire_binding_group_layout<shaders::binding_arrays::lanes>();

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
        shaders::binding_arrays::lanes{
            .sources = {s0.as_readonly_buffer(), s1.as_readonly_buffer(), s2.as_readonly_buffer()},
            .merged = merged.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    // sg asks the dispatch which elements of a bound array it touches, since the index is the shader's to pick.
    // The declaration is in force while it lives, and the submit ends it.
    auto const reads = cc::vector<sg::array_buffer_access>{{.index = 0, .access = sg::access_flag::shader_read},
                                                           {.index = 1, .access = sg::access_flag::shader_read},
                                                           {.index = 2, .access = sg::access_flag::shader_read}};
    auto const declared
        = cmd->compute.declare_array_buffer_access(shaders::binding_arrays::lanes::arrays::sources, reads);
    cmd->compute.dispatch_threads(count);
    auto const back = cmd->download.data_from_buffer(merged);
    ctx->submit_command_list(cc::move(cmd));

    auto const got = co_await back.data();
    REQUIRE(got.size() == isize(count));
    for (auto i = 0; i < count; ++i)
        CHECK(got[i] == (i % 3) * 1000 + i);
}

ASYNC_INVOCABLE_TEST("sg - an SGL unbounded binding array takes as many views as the group is given (CHK-490)",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::unbounded_binding_arrays))
        SKIP("this context has no unbounded binding arrays");

    auto const pipeline = co_await shaders::unbounded_arrays::gather_unbounded.acquire_pipeline(*ctx);
    auto const group_layout = ctx->cached.acquire_binding_group_layout<shaders::unbounded_arrays::parts>();

    // five sources, a count the shader never names, chosen here when the group is made
    constexpr auto count = 64;
    constexpr auto lanes = 5;
    auto sources = cc::vector<sg::buffer<i32>>();
    for (auto lane = 0; lane < lanes; ++lane)
    {
        auto data = cc::vector<i32>();
        for (auto i = 0; i < count; ++i)
            data.push_back(lane * 1000 + i);
        sources.push_back(ctx->persistent.create_buffer_from_data(cc::move(data), sg::buffer_usage::readonly_buffer));
    }
    auto const merged = ctx->persistent.create_buffer_from_data(
        cc::vector<i32>::create_defaulted(count), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto parts = shaders::unbounded_arrays::parts{.merged = merged.as_readwrite_buffer()};
    auto reads = cc::vector<sg::array_buffer_access>();
    for (auto lane = 0; lane < lanes; ++lane)
    {
        parts.sources.push_back(sources[lane].as_readonly_buffer());
        reads.push_back({.index = lane, .access = sg::access_flag::shader_read});
    }
    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(*cmd, group_layout, parts);
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    auto const declared
        = cmd->compute.declare_array_buffer_access(shaders::unbounded_arrays::parts::arrays::sources, reads);
    cmd->compute.dispatch_threads(count);
    auto const back = cmd->download.data_from_buffer(merged);
    ctx->submit_command_list(cc::move(cmd));

    auto const got = co_await back.data();
    REQUIRE(got.size() == isize(count));
    for (auto i = 0; i < count; ++i)
        CHECK(got[i] == (i % lanes) * 1000 + i);
}

ASYNC_INVOCABLE_TEST("sg - a staging group over an unbounded binding array holds its capacity, set and replaced in "
                     "place",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::unbounded_binding_arrays))
        SKIP("this context has no unbounded binding arrays");

    auto const pipeline = co_await shaders::unbounded_arrays::gather_unbounded.acquire_pipeline(*ctx);
    auto const group_layout = ctx->cached.acquire_binding_group_layout<shaders::unbounded_arrays::parts>();

    constexpr auto count = 64;
    constexpr auto lanes = 5;
    auto const source = [&](i32 base)
    {
        auto data = cc::vector<i32>();
        for (auto i = 0; i < count; ++i)
            data.push_back(base + i);
        return ctx->persistent.create_buffer_from_data(cc::move(data), sg::buffer_usage::readonly_buffer);
    };
    auto sources = cc::vector<sg::buffer<i32>>();
    for (auto lane = 0; lane < lanes; ++lane)
        sources.push_back(source(lane * 1000));
    auto const merged = ctx->persistent.create_buffer_from_data(
        cc::vector<i32>::create_defaulted(count), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    // room for 16, of which the shader reads the first five and the rest stay vacant
    auto const staging = ctx->persistent.create_staging_binding_group(group_layout, 16);
    auto const slot = staging->slot_of(shaders::unbounded_arrays::parts::arrays::sources.name);
    REQUIRE(staging->array_size(slot) == 16);
    staging->set_binding(staging->slot_of("parts.merged"), merged.as_readwrite_buffer());
    auto views = cc::vector<sg::raw_view>();
    for (auto const& b : sources)
        views.push_back(b.as_readonly_buffer());
    staging->set_array(slot, views);

    auto const run = [&]() -> cc::shared_async<cc::vector<i32>>
    {
        auto reads = cc::vector<sg::array_buffer_access>();
        for (auto lane = 0; lane < lanes; ++lane)
            reads.push_back({.index = lane, .access = sg::access_flag::shader_read});
        auto cmd = ctx->create_command_list();
        cmd->compute.bind_pipeline(*pipeline);
        cmd->compute.bind_group(0, *staging->snapshot());
        auto const declared
            = cmd->compute.declare_array_buffer_access(shaders::unbounded_arrays::parts::arrays::sources, reads);
        cmd->compute.dispatch_threads(count);
        auto const back = cmd->download.data_from_buffer(merged);
        ctx->submit_command_list(cc::move(cmd));
        auto const got = co_await back.data();
        auto out = cc::vector<i32>();
        for (auto const v : got)
            out.push_back(v);
        co_return out;
    };

    auto const first = co_await run();
    REQUIRE(first.size() == isize(count));
    for (auto i = 0; i < count; ++i)
        CHECK(first[i] == (i % lanes) * 1000 + i);

    // one element replaced, and the next snapshot reads it
    auto const replaced = source(70000);
    staging->set_array_element(slot, 2, replaced.as_readonly_buffer());
    auto const second = co_await run();
    REQUIRE(second.size() == isize(count));
    for (auto i = 0; i < count; ++i)
        CHECK(second[i] == (i % lanes == 2 ? 70000 : (i % lanes) * 1000) + i);
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
        auto const pipeline = co_await shaders::bytes::repack.acquire_pipeline(*ctx);
        auto const layout = ctx->cached.acquire_binding_group_layout<shaders::bytes::raw>();
        auto const in = ctx->persistent.create_buffer_from_data(cc::move(records), sg::buffer_usage::readonly_buffer);
        auto const out
            = ctx->persistent.create_buffer_from_data(cc::vector<u32>::create_defaulted(count * 4),
                                                      sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
        auto cmd = ctx->create_command_list();
        auto const group = ctx->transient.create_binding_group(
            *cmd, layout,
            shaders::bytes::raw{.records = as_bytes(in, false).as_readonly<byte>(),
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
    auto const pipeline = co_await shaders::bytes::pick.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::bytes::blocks>();
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
        shaders::bytes::blocks{.tables = {as_bytes(b0, false).as_readonly<byte>(), as_bytes(b1, false).as_readonly<byte>(),
                                          as_bytes(b2, false).as_readonly<byte>()},
                               .merged = merged.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    auto const reads = cc::vector<sg::array_buffer_access>{{.index = 0, .access = sg::access_flag::shader_read},
                                                           {.index = 1, .access = sg::access_flag::shader_read},
                                                           {.index = 2, .access = sg::access_flag::shader_read}};
    auto const declared = cmd->compute.declare_array_buffer_access(shaders::bytes::blocks::arrays::tables, reads);
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

    auto const pipeline = co_await shaders::subgroups::reduce.acquire_pipeline(*ctx);
    auto const group_layout = ctx->cached.acquire_binding_group_layout<shaders::subgroups::wave_io>();

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
    auto const group
        = ctx->transient.create_binding_group(*cmd, group_layout,
                                              shaders::subgroups::wave_io{.values = values.as_readonly_buffer(),
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

ASYNC_INVOCABLE_TEST("sg - SGL's shuffles, ballot, quad reads and inclusive sums read the lanes they name",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::subgroups))
        SKIP("this context has no subgroup operations");

    auto const pipeline = co_await shaders::subgroups::exchange.acquire_pipeline(*ctx);
    auto const group_layout = ctx->cached.acquire_binding_group_layout<shaders::subgroups::wave_exchange>();

    constexpr auto count = 2 * 64;
    constexpr auto per = 12;
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const words = ctx->persistent.create_buffer_from_data(cc::vector<u32>::create_defaulted(count * per), usage);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, group_layout, shaders::subgroups::wave_exchange{.words = words.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(count);
    auto const words_back = cmd->download.data_from_buffer(words);
    ctx->submit_command_list(cc::move(cmd));

    auto const got = co_await words_back.data();
    REQUIRE(got.size() == isize(count * per));
    for (auto i = 0; i < count; ++i)
    {
        auto const* const w = got.data() + i * per;
        auto const lane = w[10];
        auto const size = w[11];
        REQUIRE(size >= 4u);
        REQUIRE(lane < size);
        CHECK(w[0] == (lane + 1u) % size).context(cc::format("shuffle, lane {} of {}", lane, size));
        CHECK(w[1] == (lane ^ 1u)).context(cc::format("shuffle_xor, lane {}", lane));
        if (lane > 0)
            CHECK(w[2] == lane - 1u).context(cc::format("shuffle_up, lane {}", lane));
        if (lane + 1u < size)
            CHECK(w[3] == lane + 1u).context(cc::format("shuffle_down, lane {}", lane));
        // the even lanes of the subgroup's first 32
        auto const lanes = size < 32u ? size : 32u;
        auto ballot = u32(0);
        for (auto k = u32(0); k < lanes; k += 2)
            ballot |= 1u << k;
        CHECK(w[4] == ballot).context(cc::format("ballot, size {}", size));
        CHECK(w[5] == (lane & ~3u) + 2u).context(cc::format("quad_broadcast, lane {}", lane));
        CHECK(w[6] == (lane ^ 2u)).context(cc::format("quad_swap_y, lane {}", lane));
        CHECK(w[7] == (lane ^ 3u)).context(cc::format("quad_swap_diagonal, lane {}", lane));
        CHECK(w[8] == lane + 1u).context(cc::format("inclusive add, lane {}", lane));
        CHECK(w[9] == lane + 1u).context(cc::format("inclusive float add, lane {}", lane));
    }
}

ASYNC_INVOCABLE_TEST("sg - SGL's subgroup sum after a loop each lane leaves at its own iteration counts every lane",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::subgroups))
        SKIP("this context has no subgroup operations");

    // CHK-284: a loop left only by `break` reconverges after it, so the sum sees every lane's count
    auto const pipeline = co_await shaders::subgroups::reduce_after_loop.acquire_pipeline(*ctx);
    auto const group_layout = ctx->cached.acquire_binding_group_layout<shaders::subgroups::wave_staggered>();

    constexpr auto count = 4 * 128;
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const sums = ctx->persistent.create_buffer_from_data(cc::vector<u32>::create_defaulted(count), usage);
    auto const sizes = ctx->persistent.create_buffer_from_data(cc::vector<u32>::create_defaulted(count), usage);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, group_layout,
        shaders::subgroups::wave_staggered{.sums = sums.as_readwrite_buffer(), .sizes = sizes.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(count);
    auto const sums_back = cmd->download.data_from_buffer(sums);
    auto const sizes_back = cmd->download.data_from_buffer(sizes);
    ctx->submit_command_list(cc::move(cmd));

    // lane k left after k iterations, so a whole subgroup of size s sums 0 + 1 + ... + (s - 1)
    auto const got_sums = co_await sums_back.data();
    auto const got_sizes = co_await sizes_back.data();
    REQUIRE(got_sums.size() == isize(count));
    REQUIRE(got_sizes.size() == isize(count));
    for (auto i = 0; i < count; ++i)
    {
        auto const size = got_sizes[i];
        CHECK(size > 0u);
        CHECK(got_sums[i] == size * (size - 1u) / 2u);
    }
}

ASYNC_INVOCABLE_TEST("sg - an SGL buffer's length and a bytes' length are what the bound memory holds (CHK-460)",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const pipeline = co_await shaders::bytes::measure.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::bytes::measured>();
    auto const sizes = ctx->persistent.create_buffer_from_data(
        cc::vector<u32>::create_defaulted(8), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const raw = ctx->persistent.create_buffer_from_data(cc::vector<u32>::create_defaulted(12),
                                                             sg::buffer_usage::readonly_buffer);
    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout,
        shaders::bytes::measured{.sizes = sizes.as_readwrite_buffer(),
                                 .raw = raw.raw()->as_raw_readonly().as_readonly<byte>()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(1);
    auto const back = cmd->download.data_from_buffer(sizes);
    ctx->submit_command_list(cc::move(cmd));

    auto const got = co_await back.data();
    REQUIRE(got.size() == 8);
    CHECK(got[0] == 8u);
    CHECK(got[1] == 48u);
}

ASYNC_INVOCABLE_TEST("sg - an SGL unbounded binding array's length in a staging group is its capacity, which sg writes "
                     "without the host (CHK-491)",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::unbounded_binding_arrays))
        SKIP("this context has no unbounded binding arrays");

    auto const pipeline = co_await shaders::unbounded_arrays::gather_counted.acquire_pipeline(*ctx);
    auto const group_layout = ctx->cached.acquire_binding_group_layout<shaders::unbounded_arrays::counted>();
    constexpr auto count = 64;
    constexpr auto lanes = 3;
    auto sources = cc::vector<sg::buffer<i32>>();
    for (auto lane = 0; lane < lanes; ++lane)
    {
        auto data = cc::vector<i32>();
        for (auto i = 0; i < count; ++i)
            data.push_back(lane * 1000 + i);
        sources.push_back(ctx->persistent.create_buffer_from_data(cc::move(data), sg::buffer_usage::readonly_buffer));
    }
    auto const merged = ctx->persistent.create_buffer_from_data(
        cc::vector<i32>::create_defaulted(count), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    // a capacity of three, every element set: the shader wraps by three, a length nobody but sg wrote
    auto const staging = ctx->persistent.create_staging_binding_group(group_layout, lanes);
    auto const slot = staging->slot_of(shaders::unbounded_arrays::counted::arrays::sources.name);
    staging->set_binding(staging->slot_of("counted.merged"), merged.as_readwrite_buffer());
    auto views = cc::vector<sg::raw_view>();
    for (auto const& b : sources)
        views.push_back(b.as_readonly_buffer());
    staging->set_array(slot, views);

    auto reads = cc::vector<sg::array_buffer_access>();
    for (auto lane = 0; lane < lanes; ++lane)
        reads.push_back({.index = lane, .access = sg::access_flag::shader_read});
    auto cmd = ctx->create_command_list();
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *staging->snapshot());
    auto const declared
        = cmd->compute.declare_array_buffer_access(shaders::unbounded_arrays::counted::arrays::sources, reads);
    cmd->compute.dispatch_threads(count);
    auto const back = cmd->download.data_from_buffer(merged);
    ctx->submit_command_list(cc::move(cmd));

    auto const got = co_await back.data();
    REQUIRE(got.size() == isize(count));
    for (auto i = 0; i < count; ++i)
        CHECK(got[i] == (i % lanes) * 1000 + i).context(cc::format("thread {}", i));
}

ASYNC_INVOCABLE_TEST("sg - an SGL unbounded binding array's length is the views its group was given (CHK-491)",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::unbounded_binding_arrays))
        SKIP("this context has no unbounded binding arrays");

    auto const pipeline = co_await shaders::unbounded_arrays::gather_counted.acquire_pipeline(*ctx);
    auto const group_layout = ctx->cached.acquire_binding_group_layout<shaders::unbounded_arrays::counted>();
    constexpr auto count = 64;
    auto sources = cc::vector<sg::buffer<i32>>();
    for (auto lane = 0; lane < 4; ++lane)
    {
        auto data = cc::vector<i32>();
        for (auto i = 0; i < count; ++i)
            data.push_back(lane * 1000 + i);
        sources.push_back(ctx->persistent.create_buffer_from_data(cc::move(data), sg::buffer_usage::readonly_buffer));
    }

    // three views, then four: the shader wraps by whichever the group holds
    for (auto const lanes : {3, 4})
    {
        auto const merged = ctx->persistent.create_buffer_from_data(
            cc::vector<i32>::create_defaulted(count), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
        auto counted = shaders::unbounded_arrays::counted{.merged = merged.as_readwrite_buffer()};
        auto reads = cc::vector<sg::array_buffer_access>();
        for (auto lane = 0; lane < lanes; ++lane)
        {
            counted.sources.push_back(sources[lane].as_readonly_buffer());
            reads.push_back({.index = lane, .access = sg::access_flag::shader_read});
        }
        auto cmd = ctx->create_command_list();
        auto const group = ctx->transient.create_binding_group(*cmd, group_layout, counted);
        cmd->compute.bind_pipeline(*pipeline);
        cmd->compute.bind_group(0, *group);
        auto const declared
            = cmd->compute.declare_array_buffer_access(shaders::unbounded_arrays::counted::arrays::sources, reads);
        cmd->compute.dispatch_threads(count);
        auto const back = cmd->download.data_from_buffer(merged);
        ctx->submit_command_list(cc::move(cmd));

        auto const got = co_await back.data();
        REQUIRE(got.size() == isize(count));
        for (auto i = 0; i < count; ++i)
            CHECK(got[i] == (i % lanes) * 1000 + i).context(cc::format("{} views, thread {}", lanes, i));
    }
}
