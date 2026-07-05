#include <clean-core/fwd.hh> // cc::u64: epoch is an enum over u64
#include <nexus/test.hh>
#include <shaped-graphics/buffer.hh> // sg::buffer::size_in_bytes (buffer_handle operator-> target)
#include <shaped-graphics/context.hh>
#include <shaped-graphics/types.hh>

// Backend-agnostic sg API tests. Each is an INVOCABLE_TEST taking a live context, so it runs against every
// available backend: the per-backend entry driver (tests/backends/<backend>-entry.cc) creates a context and
// invokes all of these. An alias (tests/api/aliases.cc) makes each runnable by its own name across backends,
// e.g. `uv run dev.py test "sg - allocates a persistent buffer"`. To target one backend, run its driver:
// `dev.py test "sg dx12 backend" -c dx12 "sg - allocates a persistent buffer"`.

INVOCABLE_TEST("sg - context is live", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);
    CHECK(!ctx->is_shut_down());
}

INVOCABLE_TEST("sg - allocates a persistent buffer", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);

    auto buf = ctx->persistent.create_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf.has_value());
    CHECK(buf.value()->size_in_bytes() == 256);

    // A zero-size buffer is valid and allocates nothing.
    auto empty = ctx->persistent.create_buffer(0, sg::buffer_usage::none);
    REQUIRE(empty.has_value());
    CHECK(empty.value()->size_in_bytes() == 0);
}

INVOCABLE_TEST("sg - advances an epoch", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);

    auto const before = ctx->current_epoch();
    ctx->advance_epoch_and_wait_for_idle();
    CHECK(cc::u64(ctx->current_epoch()) > cc::u64(before));
    CHECK(cc::u64(ctx->completed_epoch()) >= cc::u64(before)); // the epoch we started in is now done
}
