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
