#include <nexus/test.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/raw_buffer.hh> // sg::raw_buffer::size_in_bytes / usage (raw_buffer_handle operator-> target)
#include <shaped-graphics/types.hh>

// Backend-agnostic tests for persistent buffer allocation through `ctx->persistent.create_raw_buffer`. Each is an
// INVOCABLE_TEST run against every available backend (see tests/context/context-test.cc for the mechanism).

INVOCABLE_TEST("sg - allocates a persistent buffer", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);

    auto buf = ctx->persistent.create_raw_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf.has_value());
    CHECK(buf.value()->size_in_bytes() == 256);
    CHECK(sg::has_flag(buf.value()->usage(), sg::buffer_usage::copy_src));
    CHECK(sg::has_flag(buf.value()->usage(), sg::buffer_usage::copy_dst));
    CHECK(!sg::has_flag(buf.value()->usage(), sg::buffer_usage::vertex_buffer));
}

INVOCABLE_TEST("sg - allocates buffers across usages", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);

    // A spread of usages a backend must accept at creation; the shape round-trips through the handle.
    sg::buffer_usage const usages[] = {
        sg::buffer_usage::vertex_buffer,
        sg::buffer_usage::index_buffer,
        sg::buffer_usage::uniform_buffer,
        sg::buffer_usage::readonly_buffer | sg::buffer_usage::readwrite_buffer,
        sg::buffer_usage::copy_src | sg::buffer_usage::readonly_buffer,
    };
    for (auto const u : usages)
    {
        auto buf = ctx->persistent.create_raw_buffer(1024, u);
        REQUIRE(buf.has_value());
        CHECK(buf.value()->size_in_bytes() == 1024);
        CHECK(buf.value()->usage() == u);
    }
}

INVOCABLE_TEST("sg - zero-size buffer allocates nothing", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);

    // A zero-size buffer is valid and allocates nothing.
    auto empty = ctx->persistent.create_raw_buffer(0, sg::buffer_usage::none);
    REQUIRE(empty.has_value());
    CHECK(empty.value()->size_in_bytes() == 0);
}

INVOCABLE_TEST("sg - allocates a large buffer", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);

    // A few MiB — comfortably past the inline rings, exercising a real dedicated allocation.
    auto const size = cc::isize(8) * 1024 * 1024;
    auto buf = ctx->persistent.create_raw_buffer(size, sg::buffer_usage::readwrite_buffer);
    REQUIRE(buf.has_value());
    CHECK(buf.value()->size_in_bytes() == size);
}
