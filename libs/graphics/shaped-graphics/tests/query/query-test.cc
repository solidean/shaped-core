#include <nexus/test.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/gpu_timestamp.hh>

// Backend-agnostic GPU-query round-trip: record two timestamps, submit, read them back. Gated on
// is_supported() so it also asserts the unsupported path (an invalid query, record still callable).
// Runs against every available backend (see tests/context/context-test.cc for the mechanism).

INVOCABLE_TEST("sg - gpu timestamps round-trip when supported", (sg::context_handle const& ctx))
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
        return;
    }

    auto t0 = cmd->query.record_gpu_timestamp();
    auto t1 = cmd->query.record_gpu_timestamp();
    CHECK(t0.is_valid());
    CHECK(t1.is_valid());
    CHECK(!t0.is_ready()); // not submitted yet

    ctx->submit_command_list(cc::move(cmd));

    auto const tick0 = ctx->wait_for_ticks(t0);
    auto const tick1 = ctx->wait_for_ticks(t1);
    REQUIRE(tick0.has_value());
    REQUIRE(tick1.has_value());
    CHECK(tick1.value() >= tick0.value()); // non-decreasing on a single queue

    REQUIRE(t1.is_ready());
    CHECK(t1.try_get_seconds().has_value());
}
