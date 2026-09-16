#include "metal-test-common.hh"

#include <clean-core/string/format.hh>
#include <nexus/test.hh>

// Inline transfer, smallest case first.
// Split one-list from two-list deliberately: the second needs the cross-list queue barrier pair and the first does not.
// A failure then tells you which half is wrong rather than only that a round trip broke.

namespace mtl = sg::backend::metal;
using namespace cc::primitive_defines; // the sized aliases are vocabulary; a test file pulls them in once

namespace
{
constexpr auto k_copy_both = sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst;

[[nodiscard]] cc::vector<byte> pattern_bytes(int seed, int count)
{
    auto out = cc::vector<byte>::create_uninitialized(count);
    for (auto i = 0; i < count; ++i)
        out[i] = byte(u8((seed + i * 7) & 0xFF));
    return out;
}
} // namespace

TEST("sg metal - an upload and a download in one list round-trip")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const buffer = ctx->persistent.create_raw_buffer(256, k_copy_both);
    auto const source = pattern_bytes(1, 256);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_buffer(buffer, source);
    auto future = cmd->download.bytes_from_buffer(buffer, 0, 256);
    ctx->submit_command_list(cc::move(cmd));

    ctx->block_until_idle();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == 256);

    auto mismatches = 0;
    for (auto i = 0; i < 256; ++i)
        if (bytes.value()[i] != source[i])
            ++mismatches;
    CHECK(mismatches == 0).context(cc::format("{} of 256 bytes differ", mismatches));
}

TEST("sg metal - an upload and a download in two lists round-trip")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // The cross-list case: the write is in one command buffer and the read in the next, so the ordering rests entirely
    // on the queue barrier pair the encoder carries.
    // Metal has no implicit decay to fall back on.
    auto const buffer = ctx->persistent.create_raw_buffer(256, k_copy_both);
    auto const source = pattern_bytes(2, 256);

    auto up = ctx->create_command_list();
    up->upload.bytes_to_buffer(buffer, source);
    ctx->submit_command_list(cc::move(up));

    auto down = ctx->create_command_list();
    auto future = down->download.bytes_from_buffer(buffer, 0, 256);
    ctx->submit_command_list(cc::move(down));

    ctx->block_until_idle();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());

    auto mismatches = 0;
    for (auto i = 0; i < 256; ++i)
        if (bytes.value()[i] != source[i])
            ++mismatches;
    CHECK(mismatches == 0).context(cc::format("{} of 256 bytes differ", mismatches));
}

TEST("sg metal - a device-to-device copy moves the bytes")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const src = ctx->persistent.create_raw_buffer(256, k_copy_both);
    auto const dst = ctx->persistent.create_raw_buffer(256, k_copy_both);
    auto const source = pattern_bytes(3, 256);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_buffer(src, source);
    cmd->copy.buffer_bytes_region({.src = src, .dst = dst, .size_in_bytes = 256});
    auto future = cmd->download.bytes_from_buffer(dst, 0, 256);
    ctx->submit_command_list(cc::move(cmd));

    ctx->block_until_idle();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());

    auto mismatches = 0;
    for (auto i = 0; i < 256; ++i)
        if (bytes.value()[i] != source[i])
            ++mismatches;
    CHECK(mismatches == 0).context(cc::format("{} of 256 bytes differ", mismatches));
}

TEST("sg metal - a copy between two lists sees the previous list's write")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // Three lists, which is the shape the tier-1 copy suite uses: the middle one both reads what the first wrote and
    // writes what the third reads, so it needs the queue barrier pair on both sides.
    auto const src = ctx->persistent.create_raw_buffer(256, k_copy_both);
    auto const dst = ctx->persistent.create_raw_buffer(256, k_copy_both);
    auto const source = pattern_bytes(5, 256);

    auto up = ctx->create_command_list();
    up->upload.bytes_to_buffer(src, source);
    ctx->submit_command_list(cc::move(up));

    auto cp = ctx->create_command_list();
    cp->copy.buffer_bytes_region({.src = src, .dst = dst, .size_in_bytes = 256});
    ctx->submit_command_list(cc::move(cp));

    auto down = ctx->create_command_list();
    auto future = down->download.bytes_from_buffer(dst, 0, 256);
    ctx->submit_command_list(cc::move(down));

    ctx->block_until_idle();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());

    auto mismatches = 0;
    for (auto i = 0; i < 256; ++i)
        if (bytes.value()[i] != source[i])
            ++mismatches;
    CHECK(mismatches == 0).context(cc::format("{} of 256 bytes differ", mismatches));
}

TEST("sg metal - a copy within one buffer moves the bytes")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // src and dst are the same buffer, at non-overlapping ranges — which sg allows and which declares both a read and
    // a write on one resource for a single op.
    auto const buffer = ctx->persistent.create_raw_buffer(256, k_copy_both);
    auto const source = pattern_bytes(6, 256);

    auto up = ctx->create_command_list();
    up->upload.bytes_to_buffer(buffer, source);
    ctx->submit_command_list(cc::move(up));

    auto cmd = ctx->create_command_list();
    cmd->copy.buffer_bytes_region(
        {.src = buffer, .dst = buffer, .size_in_bytes = 64, .src_offset_in_bytes = 0, .dst_offset_in_bytes = 128});
    auto future = cmd->download.bytes_from_buffer(buffer, 128, 64);
    ctx->submit_command_list(cc::move(cmd));

    ctx->block_until_idle();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == 64);

    auto mismatches = 0;
    for (auto i = 0; i < 64; ++i)
        if (bytes.value()[i] != source[i])
            ++mismatches;
    CHECK(mismatches == 0)
        .context(cc::format("{} of 64 bytes differ; first read back as {}, source[0]={}, source[128]={}", mismatches,
                            int(u8(bytes.value()[0])), int(u8(source[0])), int(u8(source[128]))));
}

TEST("sg metal - an epoch that stages nothing does not free a later epoch's bytes")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // The sequence the old head-and-tail ring got wrong.
    //
    // It rewound both cursors to zero inside reserve() whenever they met, and reclaimed by taking a caller-supplied
    // mark whenever that mark was larger than the tail.
    // An epoch that stages nothing captures the same mark as the epoch before it, so once the first retire made the
    // cursors meet and the next reservation rewound them, the second retire arrived carrying a pre-rewind mark, took
    // it, and left the tail past the head — after which the ring read as empty with a live reservation still in it.
    //
    // Cursors that only increase cannot express that, which is what this pins: the second retire frees nothing the
    // first did not, and a reservation taken in between stays reserved.
    auto& ring = ctx->upload_ring();

    auto const first = ring.reserve(1024);
    REQUIRE(first.is_valid());
    REQUIRE(first.owned == nullptr).context("the fixture must fit the ring, or it proves nothing about it");

    // Epoch A closes having staged those bytes; epoch B closes having staged none, so both checkpoints are recorded.
    auto const epoch_a = ctx->current_epoch();
    ring.on_epoch_advance(epoch_a);
    auto const epoch_b = sg::epoch(u64(epoch_a) + 1);
    ring.on_epoch_advance(epoch_b);

    auto const after_advances = ring.debug_cursor_state();
    CHECK(after_advances.checkpoints == 2);

    // Retired in separate sweeps, which is what the deferred callbacks do.
    ring.on_epochs_completed(epoch_a);
    auto const freed_by_a = ring.debug_cursor_state().freed_pos;
    CHECK(freed_by_a == after_advances.next_pos).context("epoch A owned everything staged so far");

    // A reservation between the two retires is exactly where the old shape rewound.
    auto const between = ring.reserve(1024);
    REQUIRE(between.is_valid());
    auto const next_after_between = ring.debug_cursor_state().next_pos;

    ring.on_epochs_completed(epoch_b);
    auto const after_b = ring.debug_cursor_state();

    // B staged nothing, so its checkpoint frees nothing beyond A's — and in particular not the reservation above.
    CHECK(after_b.freed_pos == freed_by_a);
    CHECK(after_b.next_pos == next_after_between);
    CHECK(after_b.freed_pos <= after_b.next_pos).context("the free watermark must never pass the bump cursor");
    CHECK(after_b.checkpoints == 0);

    // The live reservation's bytes are still spoken for, so the next one cannot be handed the same span.
    auto const after = ring.reserve(1024);
    REQUIRE(after.is_valid());
    CHECK(after.offset != between.offset).context("the ring handed the same bytes out twice");
}

TEST("sg metal - concurrent submits on one buffer see each other's writes")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

#if CC_HAS_THREADS
    // Submission is thread-safe, and what makes a token mean anything is that its order is the order the queue
    // signalled in: out-of-order signals move the shared event backwards and break is_submission_complete.
    //
    // Each thread uploads its own value into its own slot of one buffer, so the reads below fail if any list's work
    // was reported complete before it ran.
    constexpr auto k_threads = 8;
    auto const buffer = ctx->persistent.create_raw_buffer(k_threads * isize(sizeof(u32)),
                                                          k_copy_both | sg::buffer_usage::readwrite_buffer);

    auto threads = cc::vector<std::thread>::create_with_capacity(k_threads);
    for (auto i = 0; i < k_threads; ++i)
        threads.emplace_back(
            [&, i]
            {
                auto const value = u32(i + 1);
                auto cmd = ctx->create_command_list();
                cmd->upload.bytes_to_buffer(buffer, cc::as_bytes(cc::span<u32 const>(&value, 1)), i * isize(sizeof(u32)));
                // No CHECK on a spawned thread: it would not be attributed to the running test, and what is proven is
                // the aggregate read back below.
                ctx->submit_command_list(cc::move(cmd));
            });

    for (auto& t : threads)
        t.join();

    ctx->block_until_idle();

    auto read = ctx->create_command_list();
    auto future = read->download.bytes_from_buffer(buffer, 0, buffer->size_in_bytes());
    ctx->submit_command_list(cc::move(read));
    ctx->block_until_idle();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    auto const* const values = reinterpret_cast<u32 const*>(bytes.value().data());

    auto wrong = 0;
    for (auto i = 0; i < k_threads; ++i)
        if (values[i] != u32(i + 1))
            ++wrong;
    CHECK(wrong == 0).context(cc::format("{} of {} slots did not carry their thread's write", wrong, k_threads));

#else
    SKIP("a single-threaded build has no concurrent submits to order");
#endif
}
