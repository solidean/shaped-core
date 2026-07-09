#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh> // cc::byte
#include <nexus/test.hh>
#include <shaped-graphics/binding.hh>
#include <shaped-graphics/binding_group.hh> // sg::named_view
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/raw_buffer.hh>
#include <shaped-graphics/types.hh>

// Backend-agnostic tests for the transient lifetime scope (ctx->transient): per-epoch scratch buffers, the
// shared bump-allocated heap and its deferred budget, and transient binding-group instantiation. Each is an
// INVOCABLE_TEST run against every available backend (see tests/context/context-test.cc for the mechanism).
// Per-backend transient internals — the dx12 bump-heap placement granularity and the CPU-fed descriptor
// ring recycling end-to-end — live with the backend (backends/dx12/tests/dx12-transient-test.cc and
// dx12-compute-test.cc). See libs/graphics/shaped-graphics/docs/testing.md for the split.

namespace
{
auto pattern = [](int i) { return cc::byte(i & 0xFF); };

sg::buffer_usage const copy_both = sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst;

// Uploads 256 bytes of pattern(seed+i) into a fresh transient buffer, downloads them, and returns whether
// every byte matched — the round-trip that proves a transient buffer names live, distinct storage.
bool transient_round_trip(sg::context_handle const& ctx, int seed)
{
    auto buf = ctx->transient.create_raw_buffer(256, copy_both);
    if (!buf)
        return false;

    cc::byte src[256];
    for (int i = 0; i < 256; ++i)
        src[i] = pattern(seed + i);

    auto up = ctx->create_command_list();
    if (!up)
        return false;
    up->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(src, 256));
    ctx->submit_command_list(cc::move(up));

    auto down = ctx->create_command_list();
    if (!down)
        return false;
    auto future = down->download.bytes_from_buffer(buf, 0, 256);
    ctx->submit_command_list(cc::move(down));

    auto const bytes = ctx->wait_for(future);
    if (!bytes.has_value() || bytes.value().size() != 256)
        return false;
    for (int i = 0; i < 256; ++i)
        if (bytes.value()[i] != pattern(seed + i))
            return false;
    return true;
}

struct particle
{
    sg::u32 a, b, c, d;
};
} // namespace

INVOCABLE_TEST("sg - transient buffer has the requested shape", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto buf = ctx->transient.create_raw_buffer(1024, sg::buffer_usage::uniform_buffer);
    REQUIRE(buf != nullptr);
    CHECK(buf->size_in_bytes() == 1024);
    CHECK(sg::has_flag(buf->usage(), sg::buffer_usage::uniform_buffer));
    CHECK(buf->is_valid()); // fresh: created in the current epoch
}

INVOCABLE_TEST("sg - zero-size transient buffer allocates nothing", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto buf = ctx->transient.create_raw_buffer(0, sg::buffer_usage::none);
    REQUIRE(buf != nullptr);
    CHECK(buf->size_in_bytes() == 0);
    CHECK(buf->is_valid());
}

INVOCABLE_TEST("sg - transient buffer round-trips within its epoch", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    CHECK(transient_round_trip(ctx, 0));
}

INVOCABLE_TEST("sg - transient buffers in one epoch are independent", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto a = ctx->transient.create_raw_buffer(128, copy_both);
    auto b = ctx->transient.create_raw_buffer(128, copy_both);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    cc::byte src_a[128];
    cc::byte src_b[128];
    for (int i = 0; i < 128; ++i)
    {
        src_a[i] = cc::byte(0xA0 + (i & 0xF));
        src_b[i] = cc::byte(0xB0 + (i & 0xF));
    }

    auto up = ctx->create_command_list();
    REQUIRE(up != nullptr);
    up->upload.bytes_to_buffer(a, cc::span<cc::byte const>(src_a, 128));
    up->upload.bytes_to_buffer(b, cc::span<cc::byte const>(src_b, 128));
    ctx->submit_command_list(cc::move(up));

    auto down = ctx->create_command_list();
    REQUIRE(down != nullptr);
    auto future_a = down->download.bytes_from_buffer(a, 0, 128);
    auto future_b = down->download.bytes_from_buffer(b, 0, 128);
    ctx->submit_command_list(cc::move(down));

    auto const bytes_a = ctx->wait_for(future_a);
    auto const bytes_b = ctx->wait_for(future_b);
    REQUIRE(bytes_a.has_value());
    REQUIRE(bytes_b.has_value());
    bool ok = true;
    for (int i = 0; i < 128; ++i)
    {
        if (bytes_a.value()[i] != cc::byte(0xA0 + (i & 0xF)))
            ok = false;
        if (bytes_b.value()[i] != cc::byte(0xB0 + (i & 0xF)))
            ok = false;
    }
    CHECK(ok);
}

INVOCABLE_TEST("sg - transient buffer expires once its epoch passes", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto buf = ctx->transient.create_raw_buffer(256, copy_both);
    REQUIRE(buf != nullptr);
    CHECK(buf->is_valid());
    CHECK(!buf->is_expired());

    ctx->advance_epoch_and_wait_for_idle(); // its epoch has passed -> auto-expired at advance
    CHECK(buf->is_expired());               // using it now (transfer / binding) would be a hard error
    CHECK(!buf->is_valid());
}

// The transient heap resets its bump head each epoch, so successive epochs alias the same storage. Every
// epoch's data must still round-trip while a couple of epochs stay in flight.
INVOCABLE_TEST("sg - transient buffer storage is reused across epochs", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    for (int e = 0; e < 8; ++e)
    {
        CHECK(transient_round_trip(ctx, e * 7 + 1));
        ctx->advance_epoch(2); // keep at most 2 epochs in flight
    }
}

// set_budget is deferred: it records a pending budget the next advance_epoch applies (draining in-flight
// work, resizing the heap). Data must keep round-tripping across a shrink and a grow.
INVOCABLE_TEST("sg - transient budget change applies at the next epoch", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    CHECK(transient_round_trip(ctx, 0)); // epoch 0 on the default budget (heap created here)

    ctx->transient.set_budget(cc::isize(512) * 1024);
    for (int e = 1; e <= 4; ++e)
    {
        ctx->advance_epoch(2); // first advance drains + resizes to the pending 512 KiB
        CHECK(transient_round_trip(ctx, e));
    }

    ctx->transient.set_budget(cc::isize(2) * 1024 * 1024);
    for (int e = 5; e <= 8; ++e)
    {
        ctx->advance_epoch(2);
        CHECK(transient_round_trip(ctx, e));
    }
}

// The setter is repeatable and touches no GPU state; the last value before an advance wins. (The old API
// asserted on a second call / after first use — this pins the new, forgiving contract.)
INVOCABLE_TEST("sg - transient budget setter is repeatable before an advance", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    ctx->transient.set_budget(cc::isize(1) * 1024 * 1024);
    ctx->transient.set_budget(cc::isize(256) * 1024);
    ctx->transient.set_budget(cc::isize(768) * 1024); // last write wins at the next advance

    ctx->advance_epoch_and_wait_for_idle();
    CHECK(transient_round_trip(ctx, 3));
}

INVOCABLE_TEST("sg - transient binding group instantiates a persistent layout", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    sg::binding const b{
        .name = "Data",
        .set = 0,
        .index = 0,
        .count = 1,
        .type = sg::binding_type::readwrite_structured_buffer,
    };
    auto layout = ctx->uncached.create_binding_layout(cc::span<sg::binding const>(&b, 1));
    REQUIRE(layout != nullptr);

    auto buf = ctx->persistent.create_raw_buffer(256, sg::buffer_usage::readwrite_buffer);
    REQUIRE(buf != nullptr);

    sg::named_view const nv{.name = "Data", .view = buf->as_readwrite_buffer<particle>()};
    auto group = ctx->transient.create_binding_group(layout, cc::span<sg::named_view const>(&nv, 1));
    REQUIRE(group != nullptr);
    CHECK(group != nullptr);
}

INVOCABLE_TEST("sg - transient binding group rejects an unknown binding name", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    sg::binding const b{
        .name = "Data",
        .set = 0,
        .index = 0,
        .count = 1,
        .type = sg::binding_type::readwrite_structured_buffer,
    };
    auto layout = ctx->uncached.create_binding_layout(cc::span<sg::binding const>(&b, 1));
    REQUIRE(layout != nullptr);

    auto buf = ctx->persistent.create_raw_buffer(256, sg::buffer_usage::readwrite_buffer);
    REQUIRE(buf != nullptr);

    // A view bound to a name the layout does not declare is rejected, not silently ignored. The fallible
    // core surfaces it as an error; the throwing façade (create_binding_group) would raise
    // sg::binding_group_exception instead (see tests/error-handling).
    sg::named_view const wrong{.name = "Nope", .view = buf->as_readwrite_buffer<particle>()};
    auto group = ctx->transient.try_create_binding_group(layout, cc::span<sg::named_view const>(&wrong, 1));
    CHECK(group.has_error());
}
