#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh> // cc::byte
#include <nexus/test.hh>
#include <shaped-graphics/buffer.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/types.hh>

// Backend-agnostic async buffer upload (ctx.upload): CPU→GPU streaming on a dedicated copy queue, run
// against every available backend. These pin the public contract — automatic per-resource sync in BOTH
// directions so the CPU timeline (submit → async upload → submit) mirrors GPU ordering — while the copy
// pipelining / staging internals live with the backend (backends/dx12/tests/dx12-upload-async-test.cc).
// See libs/graphics/shaped-graphics/docs/concepts/upload.async.md and libs/graphics/shaped-graphics/docs/testing.md.

namespace
{
auto pattern = [](int i) { return cc::byte(i & 0xFF); };

sg::buffer_handle make_transfer_buffer(sg::context_handle const& ctx, cc::isize size)
{
    auto buf = ctx->persistent.create_buffer(size, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    CC_ASSERT(buf.has_value(), "async upload test buffer allocation failed");
    return buf.value();
}

// A pinned byte buffer filled by fn(i), moved into the pin (owns it, zero-copy).
cc::pinned_data<cc::byte const> pinned_bytes(cc::isize n, auto&& fn)
{
    cc::vector<cc::byte> data;
    data.reserve(n);
    for (cc::isize i = 0; i < n; ++i)
        data.push_back(cc::byte(fn(i)));
    return cc::make_pinned_data(cc::move(data));
}
} // namespace

INVOCABLE_TEST("sg - async upload then download round-trips", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const buf = make_transfer_buffer(ctx, 256);

    // Fire-and-forget: no wait, no advance. The later download must auto-wait on the copy.
    ctx->upload.bytes_to_buffer(buf, pinned_bytes(256, [](cc::isize i) { return int(i); }));

    auto down = ctx->create_command_list();
    REQUIRE(down.has_value());
    auto future = down.value()->download.bytes_from_buffer(buf, 0, 256);
    ctx->submit_command_list(cc::move(down.value()));

    auto const bytes = ctx->wait_for(future);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == 256);
    bool matches = true;
    for (int i = 0; i < 256; ++i)
        if (bytes.value()[i] != pattern(i))
            matches = false;
    CHECK(matches);
}

INVOCABLE_TEST("sg - async typed upload round-trips", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const buf = make_transfer_buffer(ctx, cc::isize(4) * sizeof(int));

    cc::vector<int> in;
    in.push_back(5);
    in.push_back(6);
    in.push_back(7);
    in.push_back(8);
    cc::pinned_data<int const> const pin = cc::make_pinned_data(cc::move(in));
    ctx->upload.data_to_buffer(buf, pin); // re-views the same pin as bytes, no copy

    auto down = ctx->create_command_list();
    REQUIRE(down.has_value());
    auto future = down.value()->download.data_from_buffer<int>(buf, 0, 4);
    ctx->submit_command_list(cc::move(down.value()));

    auto const data = ctx->wait_for(future);
    REQUIRE(data.has_value());
    REQUIRE(data.value().size() == 4);
    CHECK(data.value()[0] == 5);
    CHECK(data.value()[3] == 8);
}

INVOCABLE_TEST("sg - async upload of empty data is a no-op", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const buf = make_transfer_buffer(ctx, 16);

    ctx->upload.bytes_to_buffer(buf, cc::pinned_data<cc::byte const>()); // no-op, no crash
    CHECK(buf != nullptr);
}

// Reverse per-resource sync: a command list writes the buffer and is submitted; THEN an async upload to
// it must defer its copy until that list has finished, so it composes *after* it (not races it). The
// download reads the async value — with the reverse sync the buffer holds the async bytes, not the list's.
INVOCABLE_TEST("sg - async upload composes after a list that wrote the buffer", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const buf = make_transfer_buffer(ctx, 256);

    // Command list writes 0xAA, submitted first.
    cc::byte first[256];
    for (int i = 0; i < 256; ++i)
        first[i] = cc::byte(0xAA);
    auto up = ctx->create_command_list();
    REQUIRE(up.has_value());
    up.value()->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(first, 256));
    ctx->submit_command_list(cc::move(up.value()));

    // Async upload of a distinct pattern, issued after the submit → must land after it.
    ctx->upload.bytes_to_buffer(buf, pinned_bytes(256, [](cc::isize i) { return int(i); }));

    auto down = ctx->create_command_list();
    REQUIRE(down.has_value());
    auto future = down.value()->download.bytes_from_buffer(buf, 0, 256);
    ctx->submit_command_list(cc::move(down.value()));

    auto const bytes = ctx->wait_for(future);
    REQUIRE(bytes.has_value());
    bool async_won = true;
    for (int i = 0; i < 256; ++i)
        if (bytes.value()[i] != pattern(i)) // the async bytes, not 0xAA
            async_won = false;
    CHECK(async_won);
}

// Two async uploads to the same buffer compose in submission order — the second wins.
INVOCABLE_TEST("sg - two async uploads to one buffer, last wins", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const buf = make_transfer_buffer(ctx, 256);

    ctx->upload.bytes_to_buffer(buf, pinned_bytes(256, [](cc::isize) { return 0xAA; }));
    ctx->upload.bytes_to_buffer(buf, pinned_bytes(256, [](cc::isize i) { return int(i); })); // overwrites

    auto down = ctx->create_command_list();
    REQUIRE(down.has_value());
    auto future = down.value()->download.bytes_from_buffer(buf, 0, 256);
    ctx->submit_command_list(cc::move(down.value()));

    auto const bytes = ctx->wait_for(future);
    REQUIRE(bytes.has_value());
    bool second_won = true;
    for (int i = 0; i < 256; ++i)
        if (bytes.value()[i] != pattern(i))
            second_won = false;
    CHECK(second_won);
}

// Regression: the copy actor once deadlocked when one staging window batched two async uploads across an
// inline list that read the buffer between them — the window issues its reverse-sync wait once, hoisted
// ahead of its copies, so it ended up waiting on a submission token whose direct list waited on a copy the
// same window had not yet run. Hammer that exact shape: async upload; inline write of the same region +
// submit; ×256 (fast enough that the actor batches several async jobs per window), then advance and wait.
// Pre-fix this blocks forever; it must now return. See
// libs/graphics/shaped-graphics/docs/concepts/upload.async.md (the window-level acyclicity rule) and the
// re-enabled "async upload" op in transfer-fuzz-test.cc.
INVOCABLE_TEST("sg - async upload interleaved with inline writes does not deadlock", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    constexpr int n = 256;
    auto const buf = make_transfer_buffer(ctx, n);

    for (int it = 0; it < n; ++it)
    {
        // Async upload the whole region on the copy queue (ordered after already-submitted work).
        ctx->upload.bytes_to_buffer(buf, pinned_bytes(n, [it](cc::isize i) { return int(i) ^ it; }));

        // Inline write of the SAME region on the direct queue, submitted at once: it reads-after the async
        // upload (forward sync) and its submission becomes the next iteration's async reverse token — the
        // interlock that closed the cycle. Inline bytes are consumed during record, so a stack span is fine.
        auto cmd = ctx->create_command_list();
        REQUIRE(cmd.has_value());
        cc::byte inline_bytes[n];
        for (int i = 0; i < n; ++i)
            inline_bytes[i] = cc::byte((i + it) & 0xFF);
        cmd.value()->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(inline_bytes, n));
        ctx->submit_command_list(cc::move(cmd.value()));
    }

    // The pin: pre-fix the copy queue is deadlocked and this never returns.
    ctx->advance_epoch_and_wait_for_idle();

    // Bonus correctness: GPU order is A_0,B_0,…,A_255,B_255, so the last inline write wins the whole region.
    auto down = ctx->create_command_list();
    REQUIRE(down.has_value());
    auto future = down.value()->download.bytes_from_buffer(buf, 0, n);
    ctx->submit_command_list(cc::move(down.value()));
    auto const bytes = ctx->wait_for(future);
    REQUIRE(bytes.has_value());
    bool last_inline_won = true;
    for (int i = 0; i < n; ++i)
        if (bytes.value()[i] != cc::byte((i + (n - 1)) & 0xFF))
            last_inline_won = false;
    CHECK(last_inline_won);
}

// A later command list that reads the async-uploaded buffer via a device copy also auto-waits on it.
INVOCABLE_TEST("sg - async upload feeds a later on-queue copy", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const src = make_transfer_buffer(ctx, 128);
    auto const dst = make_transfer_buffer(ctx, 128);

    ctx->upload.bytes_to_buffer(src, pinned_bytes(128, [](cc::isize i) { return 0x40 + (int(i) & 0xF); }));

    auto copy = ctx->create_command_list();
    REQUIRE(copy.has_value());
    copy.value()->copy.buffer_bytes_region({.src = src, .dst = dst, .size_in_bytes = 128});
    ctx->submit_command_list(cc::move(copy.value()));

    auto down = ctx->create_command_list();
    REQUIRE(down.has_value());
    auto future = down.value()->download.bytes_from_buffer(dst, 0, 128);
    ctx->submit_command_list(cc::move(down.value()));

    auto const bytes = ctx->wait_for(future);
    REQUIRE(bytes.has_value());
    bool matches = true;
    for (int i = 0; i < 128; ++i)
        if (bytes.value()[i] != pattern(0x40 + (i & 0xF)))
            matches = false;
    CHECK(matches);
}
