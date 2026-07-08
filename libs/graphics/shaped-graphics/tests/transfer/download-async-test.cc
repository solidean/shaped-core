#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh> // cc::byte
#include <nexus/test.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/raw_buffer.hh>
#include <shaped-graphics/types.hh>

// Backend-agnostic async buffer download (ctx.download): GPU→CPU streaming on a dedicated copy queue, run
// against every available backend. These pin the public contract — a fire-and-return-future readback with
// automatic per-resource sync in BOTH directions (the read waits on the last writer; a later writer waits
// on the read), and drop-the-future cancellation — while the copy pipelining / staging internals live with
// the backend (backends/dx12/tests/dx12-download-async-test.cc). See
// libs/graphics/shaped-graphics/docs/concepts/download.async.md and libs/graphics/shaped-graphics/docs/testing.md.

namespace
{
auto pattern = [](int i) { return cc::byte(i & 0xFF); };

sg::raw_buffer_handle make_transfer_buffer(sg::context_handle const& ctx, cc::isize size)
{
    auto buf = ctx->persistent.create_raw_buffer(size, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    CC_ASSERT(buf != nullptr, "async download test buffer allocation failed");
    return buf;
}

// Seeds `buf` with fn(i) via an inline command-list upload (direct queue) and submits it. Returns the
// submission token so a test can reason about ordering. The async download then reads committed bytes by
// auto-waiting on this list (forward sync) — and this path avoids the pending-async-upload block.
void seed_buffer(sg::context_handle const& ctx, sg::raw_buffer_handle const& buf, cc::isize n, auto&& fn)
{
    cc::vector<cc::byte> data;
    data.reserve(n);
    for (cc::isize i = 0; i < n; ++i)
        data.push_back(cc::byte(fn(i)));

    auto cmd = ctx->create_command_list();
    CC_ASSERT(cmd != nullptr, "seed command list creation failed");
    cmd->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(data)); // copied during record
    ctx->submit_command_list(cc::move(cmd));
}
} // namespace

INVOCABLE_TEST("sg - async download round-trips", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const buf = make_transfer_buffer(ctx, 256);
    seed_buffer(ctx, buf, 256, [](cc::isize i) { return int(i); });

    // Fire-and-return-future: the read auto-waits on the seed list (forward sync), no manual barrier.
    auto future = ctx->download.bytes_from_buffer(buf, 0, 256);

    auto const bytes = ctx->wait_for(future);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == 256);
    bool matches = true;
    for (int i = 0; i < 256; ++i)
        if (bytes.value()[i] != pattern(i))
            matches = false;
    CHECK(matches);
}

INVOCABLE_TEST("sg - async typed download round-trips", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const buf = make_transfer_buffer(ctx, cc::isize(4) * sizeof(int));

    cc::vector<int> in;
    in.push_back(5);
    in.push_back(6);
    in.push_back(7);
    in.push_back(8);
    {
        auto cmd = ctx->create_command_list();
        REQUIRE(cmd != nullptr);
        cmd->upload.data_to_buffer(buf, in);
        ctx->submit_command_list(cc::move(cmd));
    }

    auto future = ctx->download.data_from_buffer<int>(buf, 0, 4);
    auto const data = ctx->wait_for(future);
    REQUIRE(data.has_value());
    REQUIRE(data.value().size() == 4);
    CHECK(data.value()[0] == 5);
    CHECK(data.value()[3] == 8);
}

INVOCABLE_TEST("sg - async download of zero bytes is a ready empty future", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const buf = make_transfer_buffer(ctx, 16);

    auto future = ctx->download.bytes_from_buffer(buf, 0, 0);
    CHECK(future.is_ready()); // ready on construction, no GPU work
    auto const bytes = ctx->wait_for(future);
    REQUIRE(bytes.has_value());
    CHECK(bytes.value().size() == 0);
}

// A later command list that reads (via a device copy) a buffer the async readback also read still works —
// two reads don't conflict, so no reverse wait is folded, and both observe the seeded bytes.
INVOCABLE_TEST("sg - async download shares a buffer with a later reader", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const src = make_transfer_buffer(ctx, 128);
    auto const dst = make_transfer_buffer(ctx, 128);
    seed_buffer(ctx, src, 128, [](cc::isize i) { return 0x40 + (int(i) & 0xF); });

    auto down = ctx->download.bytes_from_buffer(src, 0, 128);

    // A copy that READS src into dst, submitted after the async read was issued.
    auto copy = ctx->create_command_list();
    REQUIRE(copy != nullptr);
    copy->copy.buffer_bytes_region({.src = src, .dst = dst, .size_in_bytes = 128});
    ctx->submit_command_list(cc::move(copy));

    auto const bytes = ctx->wait_for(down);
    REQUIRE(bytes.has_value());
    bool matches = true;
    for (int i = 0; i < 128; ++i)
        if (bytes.value()[i] != pattern(0x40 + (i & 0xF)))
            matches = false;
    CHECK(matches);
}

// Reverse per-resource sync: an async download reads the buffer; THEN a command list that WRITES it is
// submitted immediately. The write must defer until the copy-queue read has finished, so the download reads
// the *pre-write* bytes and the write still lands. Both hold only with the automatic reverse sync.
INVOCABLE_TEST("sg - a later write waits on an in-flight async download", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    constexpr int n = 256;
    auto const buf = make_transfer_buffer(ctx, n);
    seed_buffer(ctx, buf, n, [](cc::isize i) { return int(i); }); // the async read must see this

    // Async read issued first; it stamps the buffer so the write below waits on it.
    auto down = ctx->download.bytes_from_buffer(buf, 0, n);

    // Direct-queue write of a distinct pattern, submitted at once: it must land *after* the read.
    cc::byte overwrite[n];
    for (int i = 0; i < n; ++i)
        overwrite[i] = cc::byte(0xBB);
    auto up = ctx->create_command_list();
    REQUIRE(up != nullptr);
    up->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(overwrite, n));
    ctx->submit_command_list(cc::move(up));

    // The async read observes the seeded bytes, not 0xBB.
    auto const bytes = ctx->wait_for(down);
    REQUIRE(bytes.has_value());
    bool read_saw_seed = true;
    for (int i = 0; i < n; ++i)
        if (bytes.value()[i] != pattern(i))
            read_saw_seed = false;
    CHECK(read_saw_seed);

    // ...and the write still landed: an inline re-download shows 0xBB.
    auto again = ctx->create_command_list();
    REQUIRE(again != nullptr);
    auto after = again->download.bytes_from_buffer(buf, 0, n);
    ctx->submit_command_list(cc::move(again));
    auto const after_bytes = ctx->wait_for(after);
    REQUIRE(after_bytes.has_value());
    bool write_landed = true;
    for (int i = 0; i < n; ++i)
        if (after_bytes.value()[i] != cc::byte(0xBB))
            write_landed = false;
    CHECK(write_landed);
}

// Two async downloads of the same buffer both deliver the seeded bytes (concurrent reads are independent).
INVOCABLE_TEST("sg - two async downloads of one buffer", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const buf = make_transfer_buffer(ctx, 256);
    seed_buffer(ctx, buf, 256, [](cc::isize i) { return int(i); });

    auto a = ctx->download.bytes_from_buffer(buf, 0, 256);
    auto b = ctx->download.bytes_from_buffer(buf, 0, 256);

    auto const ba = ctx->wait_for(a);
    auto const bb = ctx->wait_for(b);
    REQUIRE(ba.has_value());
    REQUIRE(bb.has_value());
    bool both = true;
    for (int i = 0; i < 256; ++i)
        if (ba.value()[i] != pattern(i) || bb.value()[i] != pattern(i))
            both = false;
    CHECK(both);
}

// Dropping the future cancels the copy — but its reverse-sync completion value is still signaled, so a later
// writer that folded it does not hang. We drop the future without waiting, then write the buffer (folding the
// dropped download's value) and advance-and-wait: pre-fix (no signal on the cancelled read) this hangs.
INVOCABLE_TEST("sg - dropping an async download future never hangs a later writer", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    constexpr int n = 64 * 1024; // large enough that the read is worth cancelling
    auto const buf = make_transfer_buffer(ctx, n);
    seed_buffer(ctx, buf, n, [](cc::isize i) { return int(i); });

    // Issue the async download and drop its future immediately (before waiting) — cancels the copy.
    {
        auto dropped = ctx->download.bytes_from_buffer(buf, 0, n);
        (void)dropped;
    }

    // A direct-queue write of the buffer folds the dropped download's completion value and waits on it.
    cc::byte fill = cc::byte(0x5A);
    cc::vector<cc::byte> overwrite;
    overwrite.reserve(n);
    for (int i = 0; i < n; ++i)
        overwrite.push_back(fill);
    auto up = ctx->create_command_list();
    REQUIRE(up != nullptr);
    up->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(overwrite));
    ctx->submit_command_list(cc::move(up));

    // Pre-fix (cancelled read leaves a hole in the download fence) this never returns.
    ctx->advance_epoch_and_wait_for_idle();

    // The write still landed.
    auto down = ctx->create_command_list();
    REQUIRE(down != nullptr);
    auto future = down->download.bytes_from_buffer(buf, 0, n);
    ctx->submit_command_list(cc::move(down));
    auto const bytes = ctx->wait_for(future);
    REQUIRE(bytes.has_value());
    bool write_landed = true;
    for (int i = 0; i < n; ++i)
        if (bytes.value()[i] != fill)
            write_landed = false;
    CHECK(write_landed);
}

// The fringe path: an async UPLOAD followed by an async DOWNLOAD of the same buffer with no intervening
// direct-queue op. The download blocks the caller on the upload (a documented v1 simplification, with a
// warning), then reads the uploaded bytes.
INVOCABLE_TEST("sg - async download after an async upload of the same buffer", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    constexpr int n = 256;
    auto const buf = make_transfer_buffer(ctx, n);

    cc::vector<cc::byte> up;
    up.reserve(n);
    for (int i = 0; i < n; ++i)
        up.push_back(pattern(i));
    ctx->upload.bytes_to_buffer(buf, cc::make_pinned_data(cc::move(up)));

    auto future = ctx->download.bytes_from_buffer(buf, 0, n); // blocks on the pending upload, then reads it
    auto const bytes = ctx->wait_for(future);
    REQUIRE(bytes.has_value());
    bool matches = true;
    for (int i = 0; i < n; ++i)
        if (bytes.value()[i] != pattern(i))
            matches = false;
    CHECK(matches);
}
