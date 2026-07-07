#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh> // cc::byte
#include <nexus/test.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/raw_buffer.hh>
#include <shaped-graphics/types.hh>

// Backend-agnostic inline buffer transfer: upload / download over the public sg API, run against every
// available backend (see tests/context/context-test.cc for the invocable/alias mechanism). These pin the
// CPU↔GPU contract sg promises; per-backend mechanics (ring reclaim, actor cancellation) live with the
// backend (e.g. backends/dx12/tests/dx12-transfer-test.cc).

namespace
{
// Fills a byte pattern; value(i) keeps producer/checker in sync.
auto pattern = [](int i) { return cc::byte(i & 0xFF); };

sg::raw_buffer_handle make_transfer_buffer(sg::context_handle const& ctx, cc::isize size)
{
    auto buf = ctx->persistent.create_raw_buffer(size, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    CC_ASSERT(buf.has_value(), "transfer test buffer allocation failed");
    return buf.value();
}
} // namespace

INVOCABLE_TEST("sg - upload then download the same buffer in one list", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const buf = make_transfer_buffer(ctx, 256);

    cc::byte src[256];
    for (int i = 0; i < 256; ++i)
        src[i] = pattern(i);

    // Single command list: the backend must order the download read after the upload write.
    auto cmd = ctx->create_command_list();
    REQUIRE(cmd.has_value());
    cmd.value()->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(src, 256));
    auto future = cmd.value()->download.bytes_from_buffer(buf, 0, 256);
    ctx->submit_command_list(cc::move(cmd.value()));

    auto const bytes = ctx->wait_for(future);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == 256);
    bool matches = true;
    for (int i = 0; i < 256; ++i)
        if (bytes.value()[i] != pattern(i))
            matches = false;
    CHECK(matches);
}

INVOCABLE_TEST("sg - upload and download across separate lists", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const buf = make_transfer_buffer(ctx, 256);

    cc::byte src[256];
    for (int i = 0; i < 256; ++i)
        src[i] = pattern(i);

    auto up = ctx->create_command_list();
    REQUIRE(up.has_value());
    up.value()->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(src, 256));
    ctx->submit_command_list(cc::move(up.value()));

    auto down = ctx->create_command_list();
    REQUIRE(down.has_value());
    auto future = down.value()->download.bytes_from_buffer(buf, 0, 256);
    ctx->submit_command_list(cc::move(down.value()));

    auto const bytes = ctx->wait_for(future);
    REQUIRE(bytes.has_value());
    CHECK(bytes.value().size() == 256);
    CHECK(bytes.value()[100] == pattern(100));
}

INVOCABLE_TEST("sg - typed upload/download round-trips", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const buf = make_transfer_buffer(ctx, cc::isize(4) * sizeof(int));

    int const in[4] = {5, 6, 7, 8};
    auto cmd = ctx->create_command_list();
    REQUIRE(cmd.has_value());
    cmd.value()->upload.data_to_buffer(buf, cc::span<int const>(in, 4));
    auto future = cmd.value()->download.data_from_buffer<int>(buf, 0, 4);
    ctx->submit_command_list(cc::move(cmd.value()));

    auto const data = ctx->wait_for(future);
    REQUIRE(data.has_value());
    REQUIRE(data.value().size() == 4);
    CHECK(data.value()[0] == 5);
    CHECK(data.value()[3] == 8);
}

INVOCABLE_TEST("sg - upload at an offset, download a partial range", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const buf = make_transfer_buffer(ctx, 256);

    // Upload only the middle 128 bytes; download exactly that window back.
    cc::byte mid[128];
    for (int i = 0; i < 128; ++i)
        mid[i] = pattern(0x40 + i);

    auto up = ctx->create_command_list();
    REQUIRE(up.has_value());
    up.value()->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(mid, 128), 64);
    ctx->submit_command_list(cc::move(up.value()));

    auto down = ctx->create_command_list();
    REQUIRE(down.has_value());
    auto future = down.value()->download.bytes_from_buffer(buf, 64, 128);
    ctx->submit_command_list(cc::move(down.value()));

    auto const bytes = ctx->wait_for(future);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == 128);
    bool matches = true;
    for (int i = 0; i < 128; ++i)
        if (bytes.value()[i] != pattern(0x40 + i))
            matches = false;
    CHECK(matches);
}

INVOCABLE_TEST("sg - multiple uploads in one list, last writer wins", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const buf = make_transfer_buffer(ctx, 16);

    cc::byte first[16];
    cc::byte second[16];
    for (int i = 0; i < 16; ++i)
    {
        first[i] = cc::byte(0xAA);
        second[i] = cc::byte(0xBB);
    }

    auto cmd = ctx->create_command_list();
    REQUIRE(cmd.has_value());
    cmd.value()->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(first, 16));
    cmd.value()->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(second, 16)); // overwrites
    auto future = cmd.value()->download.bytes_from_buffer(buf, 0, 16);
    ctx->submit_command_list(cc::move(cmd.value()));

    auto const bytes = ctx->wait_for(future);
    REQUIRE(bytes.has_value());
    bool all_second = true;
    for (int i = 0; i < 16; ++i)
        if (bytes.value()[i] != cc::byte(0xBB))
            all_second = false;
    CHECK(all_second);
}

INVOCABLE_TEST("sg - empty transfers are no-ops", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const buf = make_transfer_buffer(ctx, 16);

    auto cmd = ctx->create_command_list();
    REQUIRE(cmd.has_value());
    cmd.value()->upload.bytes_to_buffer(buf, cc::span<cc::byte const>()); // no-op
    auto future = cmd.value()->download.bytes_from_buffer(buf, 0, 0);
    CHECK(future.is_valid());
    CHECK(future.is_ready()); // zero-size read is immediately ready

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    CHECK(bytes.value().empty());
    ctx->submit_command_list(cc::move(cmd.value()));
}

// A submitted readback is deliverable without advancing the epoch: ctx.wait_for blocks until the
// download actor has copied the bytes back — no advance_epoch / wait_for_idle needed. (This replaces an
// earlier is_ready()-after-wait_for_idle assumption that flaked under transfer-fuzz seed 1: idle drains
// the GPU but not the actor, so is_ready() can lag it; wait_for is the actual completion guarantee.)
INVOCABLE_TEST("sg - wait_for delivers a submitted readback without an epoch advance", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const buf = make_transfer_buffer(ctx, 256);

    cc::byte src[256];
    for (int i = 0; i < 256; ++i)
        src[i] = pattern(i);

    auto up = ctx->create_command_list();
    REQUIRE(up.has_value());
    up.value()->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(src, 256));
    ctx->submit_command_list(cc::move(up.value()));

    auto down = ctx->create_command_list();
    REQUIRE(down.has_value());
    auto future = down.value()->download.bytes_from_buffer(buf, 0, 256);
    ctx->submit_command_list(cc::move(down.value()));

    // No advance_epoch: the future is waitable as soon as its list is submitted.
    auto const bytes = ctx->wait_for(future);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == 256);
    CHECK(future.is_ready()); // wait_for delivered -> the non-blocking poll now agrees
    bool matches = true;
    for (int i = 0; i < 256; ++i)
        if (bytes.value()[i] != pattern(i))
            matches = false;
    CHECK(matches);
}

INVOCABLE_TEST("sg - wait_for on an invalid future yields nullopt", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    CHECK(!ctx->wait_for(sg::bytes_future{}).has_value());
    CHECK(!ctx->wait_for(sg::data_future<int>{}).has_value());
}

INVOCABLE_TEST("sg - readback survives an epoch advance", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const buf = make_transfer_buffer(ctx, cc::isize(8) * sizeof(int));

    int const in[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    auto cmd = ctx->create_command_list();
    REQUIRE(cmd.has_value());
    cmd.value()->upload.data_to_buffer(buf, cc::span<int const>(in, 8));
    auto future = cmd.value()->download.data_from_buffer<int>(buf, 0, 8);
    ctx->submit_command_list(cc::move(cmd.value()));

    ctx->advance_epoch_and_wait_for_idle(); // close the epoch and fully drain before reading back

    auto const data = ctx->wait_for(future);
    REQUIRE(data.has_value());
    REQUIRE(data.value().size() == 8);
    CHECK(data.value()[0] == 10);
    CHECK(data.value()[7] == 80);
}
