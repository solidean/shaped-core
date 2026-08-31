#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

using namespace cc::primitive_defines;

// Two readbacks recorded at the same time, on one context, reading different buffers.
//
// The staging behind `cmd.download` is one ring per context, and its space is reserved while a list *records* while
// the jobs that drain it are enqueued when a list *submits*.
// Those are two different orders the moment two lists are open at once, and the ring's own header claims they are the
// same one: "drains readbacks in enqueue order, which is also ring-allocation order".
//
// This is the test for that claim.
// It reserves in one order and submits in the other, deliberately, which is what a concurrent suite does by accident.

namespace
{
constexpr int k_count = 64;
constexpr isize k_bytes = isize(k_count) * isize(sizeof(u32));

// Two patterns that cannot be confused with each other, with zero, or with uninitialized memory.
[[nodiscard]] cc::vector<u32> pattern(u32 base)
{
    auto out = cc::vector<u32>();
    out.reserve(k_count);
    for (int i = 0; i < k_count; ++i)
        out.push_back(base + u32(i));
    return out;
}

[[nodiscard]] bool matches(cc::span<byte const> bytes, cc::span<u32 const> expected)
{
    if (bytes.size() != k_bytes)
        return false;
    auto const* const got = reinterpret_cast<u32 const*>(bytes.data());
    for (int i = 0; i < k_count; ++i)
        if (got[i] != expected[i])
            return false;
    return true;
}
} // namespace

INVOCABLE_TEST("sg - two readbacks recorded concurrently each get their own bytes", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto const first_data = pattern(0x1000'0000u);
    auto const second_data = pattern(0x2000'0000u);

    auto first = ctx->persistent.create_raw_buffer(k_bytes, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    auto second = ctx->persistent.create_raw_buffer(k_bytes, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    {
        auto seed = ctx->create_command_list();
        REQUIRE(seed != nullptr);
        seed->upload.bytes_to_buffer(first, cc::as_bytes(cc::span<u32 const>(first_data)));
        seed->upload.bytes_to_buffer(second, cc::as_bytes(cc::span<u32 const>(second_data)));
        ctx->submit_command_list(cc::move(seed));
    }

    // Both lists are open here, which is the whole point: the ring hands out space to A then B...
    auto a = ctx->create_command_list();
    auto b = ctx->create_command_list();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    auto future_a = a->download.bytes_from_buffer(first, 0, k_bytes);
    auto future_b = b->download.bytes_from_buffer(second, 0, k_bytes);

    // ...and they submit in the opposite order.
    ctx->submit_command_list(cc::move(b));
    ctx->submit_command_list(cc::move(a));

    auto const bytes_a = ctx->wait_for(future_a);
    auto const bytes_b = ctx->wait_for(future_b);
    REQUIRE(bytes_a.has_value());
    REQUIRE(bytes_b.has_value());

    // Each future must carry the buffer its own list read, whatever order the ring or the actor saw them in.
    CHECK(matches(bytes_a.value(), first_data)).context("the first list's readback did not return the first buffer");
    CHECK(matches(bytes_b.value(), second_data)).context("the second list's readback did not return the second buffer");

    ctx->advance_epoch_and_wait_for_idle();
}
