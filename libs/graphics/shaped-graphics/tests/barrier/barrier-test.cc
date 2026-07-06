#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh> // cc::byte
#include <nexus/test.hh>
#include <shaped-graphics/buffer.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/types.hh>

// Backend-agnostic tests for the barrier / access-tracking system, run against every available backend.
// The intra-list-ordering payoff (upload→download / upload→copy→download of the same resource in one
// list) is covered by tests/transfer and tests/copy; these add the cases specific to the new system:
// concurrent command-list recording (slot allocation) and a self-copy's combined read+write ordering.

namespace
{
auto pattern = [](int i) { return cc::byte(i & 0xFF); };

sg::buffer_handle make_buffer(sg::context_handle const& ctx, cc::isize size)
{
    auto buf = ctx->persistent.create_buffer(size, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    CC_ASSERT(buf.has_value(), "barrier test buffer allocation failed");
    return buf.value();
}
} // namespace

INVOCABLE_TEST("sg - two concurrent command lists record and submit independently", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);
    auto const a = make_buffer(ctx, cc::isize(16) * sizeof(int));
    auto const b = make_buffer(ctx, cc::isize(16) * sizeof(int));

    int va[16];
    int vb[16];
    for (int i = 0; i < 16; ++i)
    {
        va[i] = i;
        vb[i] = 1000 + i;
    }

    // Both lists open at once — each holds a distinct access-tracking slot. c1 submits while c2 is still
    // open (not the last list); c2 submits last. Both must round-trip their own buffer correctly.
    auto c1 = ctx->create_command_list();
    auto c2 = ctx->create_command_list();
    REQUIRE(c1.has_value());
    REQUIRE(c2.has_value());

    c1.value()->upload.data_to_buffer(a, cc::span<int const>(va, 16));
    c2.value()->upload.data_to_buffer(b, cc::span<int const>(vb, 16));
    auto fa = c1.value()->download.data_from_buffer<int>(a, 0, 16);
    auto fb = c2.value()->download.data_from_buffer<int>(b, 0, 16);

    ctx->submit_command_list(cc::move(c1.value()));
    ctx->submit_command_list(cc::move(c2.value()));

    auto const da = fa.wait_get_data();
    auto const db = fb.wait_get_data();
    REQUIRE(da.has_value());
    REQUIRE(db.has_value());
    CHECK(da.value()[0] == 0);
    CHECK(da.value()[15] == 15);
    CHECK(db.value()[0] == 1000);
    CHECK(db.value()[15] == 1015);
}

INVOCABLE_TEST("sg - self-copy within one buffer orders read+write in one list", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);
    auto const buf = make_buffer(ctx, 256);

    cc::byte first[128];
    for (int i = 0; i < 128; ++i)
        first[i] = pattern(i);

    // upload → first half; self-copy first half → second half (read+write the same buffer); download the
    // second half. The tracker must order the copy after the upload and the download after the copy.
    auto cmd = ctx->create_command_list();
    REQUIRE(cmd.has_value());
    cmd.value()->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(first, 128), 0);
    cmd.value()->copy.buffer_bytes_region(
        {.src = buf, .dst = buf, .size_in_bytes = 128, .src_offset_in_bytes = 0, .dst_offset_in_bytes = 128});
    auto future = cmd.value()->download.bytes_from_buffer(buf, 128, 128);
    ctx->submit_command_list(cc::move(cmd.value()));

    auto const bytes = future.wait_get_bytes();
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == 128);
    bool matches = true;
    for (int i = 0; i < 128; ++i)
        if (bytes.value()[i] != pattern(i))
            matches = false;
    CHECK(matches);
}
