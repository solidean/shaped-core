#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/query/gpu_timestamp.hh>

// Backend-agnostic GPU-query round-trip: record two timestamps, submit, read them back.
// Gated on is_supported(), so it also asserts the unsupported path — an invalid query, with record still callable.
// Runs against every available backend (see tests/context/context-test.cc for the mechanism).

ASYNC_INVOCABLE_TEST("sg - gpu timestamps round-trip when supported", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto cmd = ctx->create_command_list();
    REQUIRE(cmd != nullptr);

    if (!cmd->query.is_supported())
    {
        // Unsupported backend: record is still callable and yields an invalid, never-ready query.
        auto const t = cmd->query.record_gpu_timestamp();
        CHECK(!t.is_valid());
        CHECK(!t.is_ready());
        ctx->drop_command_list(cc::move(cmd));
        co_return;
    }

    auto t0 = cmd->query.record_gpu_timestamp();
    auto t1 = cmd->query.record_gpu_timestamp();
    CHECK(t0.is_valid());
    CHECK(t1.is_valid());
    CHECK(!t0.is_ready()); // not submitted yet

    ctx->submit_command_list(cc::move(cmd));

    auto const tick0 = co_await t0.ticks();
    auto const tick1 = co_await t1.ticks();
    CHECK(tick1 >= tick0); // non-decreasing on a single queue

    REQUIRE(t1.is_ready());
    CHECK(t1.try_get_seconds().has_value());
}

// More lists in flight at once than a device has query heaps: metal grants a process 32 counter heaps, so a list that
// held one of its own for its two timestamps ran out after 32, and every later timestamp was invalid.
ASYNC_INVOCABLE_TEST("sg - timestamps from hundreds of lists in flight at once all read back",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!ctx->supports(sg::feature::timestamp_query))
        SKIP("this context has no timestamp queries");

    auto spans = cc::vector<cc::pair<sg::gpu_timestamp, sg::gpu_timestamp>>();
    for (auto i = 0; i < 256; ++i)
    {
        auto cmd = ctx->create_command_list();
        auto const from = cmd->query.record_gpu_timestamp();
        auto const to = cmd->query.record_gpu_timestamp();
        ctx->submit_command_list(cc::move(cmd));
        ctx->advance_epoch();
        spans.push_back({from, to});
    }

    auto invalid = 0;
    for (auto const& [from, to] : spans)
    {
        if (!from.is_valid() || !to.is_valid())
        {
            ++invalid;
            continue;
        }
        auto const tick_from = co_await from.ticks();
        auto const tick_to = co_await to.ticks();
        CHECK(tick_from > 0u);
        CHECK(tick_to >= tick_from);
    }
    CHECK(invalid == 0);
}
