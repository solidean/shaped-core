#include <clean-core/fwd.hh> // cc::u64: epoch is an enum over u64
#include <nexus/test.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/types.hh>

// Backend-agnostic sg API tests. Each is an INVOCABLE_TEST taking a live context, so it runs against every
// available backend: the per-backend entry driver (tests/backends/<backend>-entry.cc) creates a context and
// invokes all of these. An alias (tests/backends/backends.cc) makes each runnable by its own name across
// backends, e.g. `uv run dev.py test "sg - context is live"`. To target one backend, run its driver:
// `dev.py test "sg dx12 warp backend" -c dx12-warp "sg - context is live"`.

INVOCABLE_TEST("sg - context is live", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);
    CHECK(!ctx->is_shut_down());
}

INVOCABLE_TEST("sg - advances an epoch", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);

    auto const before = ctx->current_epoch();
    ctx->advance_epoch_and_wait_for_idle();
    CHECK(cc::u64(ctx->current_epoch()) > cc::u64(before));
    CHECK(cc::u64(ctx->completed_epoch()) >= cc::u64(before)); // the epoch we started in is now done
}

INVOCABLE_TEST("sg - completed epoch trails current across advances", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);

    // Draining (advance_epoch(0)) leaves nothing in flight: the just-closed epoch is completed, and
    // completed never overtakes current.
    for (int i = 0; i < 3; ++i)
    {
        auto const closing = ctx->current_epoch();
        ctx->advance_epoch(0); // fully drain the GPU
        CHECK(cc::u64(ctx->current_epoch()) > cc::u64(closing));
        CHECK(cc::u64(ctx->completed_epoch()) >= cc::u64(closing));
        CHECK(cc::u64(ctx->completed_epoch()) < cc::u64(ctx->current_epoch()));
    }
}

INVOCABLE_TEST("sg - epoch waits and reclaim are safe to call", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);

    // With nothing in flight these are no-ops, but must not fault or move the epoch backwards.
    ctx->process_completed_epochs();
    ctx->wait_for_next_inflight_epoch();
    ctx->wait_for_epoch(ctx->completed_epoch());
    CHECK(cc::u64(ctx->completed_epoch()) <= cc::u64(ctx->current_epoch()));
}
