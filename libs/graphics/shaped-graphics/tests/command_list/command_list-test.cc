#include <clean-core/fwd.hh> // cc::u64
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/fwd.hh> // sg::submission_token
#include <shaped-graphics/resource/raw_buffer.hh>
#include <shaped-graphics/types.hh>

using namespace cc::primitive_defines;

// Backend-agnostic command-list lifecycle: create → submit / drop, epoch stamping, and submission-token completion.
// Run against every available backend — see tests/context/context-test.cc for the mechanism.

INVOCABLE_TEST("sg - a fresh command list is stamped with the current epoch", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto cmd = ctx->create_command_list();
    REQUIRE(cmd != nullptr);
    CHECK(cmd->created_in_epoch() == ctx->current_epoch());

    ctx->drop_command_list(cc::move(cmd)); // dropping an unsubmitted list is safe
}

// Regression, narrowed down from a transfer-fuzz finding: a command list left neither submitted nor dropped must NOT leak the open-list count.
// If it did, a later advance_epoch would wrongly trip its "every list must be submitted or dropped before advancing" assert.
// That is exactly how the fuzz's shared context got polluted across replays and reported a false [mk_trace, advance] failure.
// Letting a list leave scope auto-drops it, clearing the count, and prints one warning to stderr — expected here.
ASYNC_INVOCABLE_TEST("sg - an unsubmitted command list auto-drops on scope exit", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    {
        auto cmd = ctx->create_command_list();
        REQUIRE(cmd != nullptr);
        // cmd leaves scope here, neither submitted nor dropped -> auto-dropped (with a warning), not leaked
    }

    // The open-list count is back to zero, so this must not assert "open lists before advancing".
    ctx->advance_epoch();
    co_await ctx->idle_completion();

    // ...and the context is still fully usable afterwards.
    auto next = ctx->create_command_list();
    REQUIRE(next != nullptr);
    ctx->drop_command_list(cc::move(next));
}

ASYNC_INVOCABLE_TEST("sg - a submitted list completes after the GPU drains", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto cmd = ctx->create_command_list();
    REQUIRE(cmd != nullptr);
    auto const token = ctx->submit_command_list(cc::move(cmd));

    // not_submitted is the always-pending sentinel and is never complete.
    CHECK(!ctx->is_submission_complete(sg::submission_token::not_submitted));

    ctx->advance_epoch();
    co_await ctx->idle_completion(); // fully drain: the token's work is now finished
    CHECK(ctx->is_submission_complete(token));
}

ASYNC_INVOCABLE_TEST("sg - submission tokens advance across submits", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto a = ctx->create_command_list();
    REQUIRE(a != nullptr);
    auto const first = ctx->submit_command_list(cc::move(a));

    auto b = ctx->create_command_list();
    REQUIRE(b != nullptr);
    auto const second = ctx->submit_command_list(cc::move(b));

    // Distinct submissions get distinct, monotonically increasing tokens.
    CHECK(u64(second) > u64(first));

    ctx->advance_epoch();
    co_await ctx->idle_completion();
    CHECK(ctx->is_submission_complete(first));
    CHECK(ctx->is_submission_complete(second));
}

// A recorded download is a promise to settle the future, and a list that never runs still has to keep it — as a
// cancellation, which is what sg::bytes_future documents.
// Leaving it unsettled is the failure this pins: the caller waits on bytes that will never arrive.

INVOCABLE_TEST("sg - dropping a list cancels the downloads it recorded", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto buffer = ctx->persistent.create_raw_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buffer != nullptr);

    auto cmd = ctx->create_command_list();
    auto const download = cmd->download.bytes_from_buffer(buffer, 0, 256);
    REQUIRE(download.is_valid());
    CHECK(!download.is_ready());

    ctx->drop_command_list(cc::move(cmd));

    // Settled, and settled on the error channel — which is what try_get_bytes reporting nothing means once ready.
    CHECK(download.is_ready());
    CHECK(!download.try_get_bytes().has_value());
}

INVOCABLE_TEST("sg - a list destroyed without submit or drop cancels its downloads", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto buffer = ctx->persistent.create_raw_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buffer != nullptr);

    auto download = sg::bytes_future();
    {
        auto cmd = ctx->create_command_list();
        download = cmd->download.bytes_from_buffer(buffer, 0, 256);
        // cmd leaves scope neither submitted nor dropped, and prints one warning — expected here
    }

    CHECK(download.is_ready());
    CHECK(!download.try_get_bytes().has_value());
}
