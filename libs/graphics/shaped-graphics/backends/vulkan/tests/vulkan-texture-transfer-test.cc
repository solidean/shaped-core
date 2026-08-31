#include "vulkan-test-common.hh"

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/all.hh>

using namespace cc::primitive_defines;

// What the vulkan transfer queue does to a texture's layout.
//
// Tier 2 rather than tier 1, and that placement is the finding: this is a guarantee vulkan makes and dx12 does not.
// A D3D12 copy queue cannot run layout barriers, so an async texture copy there requires the resource in COMMON and
// an inline list that left it in COPY_DEST breaks it — see libs/graphics/shaped-graphics/docs/TODO.md's prepare-for-async entry.
//
// The rule vulkan holds to, and what this pins: the transfer queue **borrows** a texture's layout.
// It transitions from the layout the texture rests in and hands it straight back in that same layout, so canonical
// keeps a single writer — a graphics list's finalize — and a list recorded around the transfer still finds the texture
// where its own barriers say it is.
//
// Before this, the async paths handed the texture back in `general` and wrote that to the tracker, making the
// transfer queue a second writer of canonical on a decoupled timeline.
// Once prepare-for-async lands, the pattern below stops being something a caller may write at all, and this test is
// restated in its terms.

namespace
{
constexpr int k_extent = 16;
constexpr isize k_bytes = isize(k_extent) * k_extent * 4;

byte pattern_at(isize i, int salt)
{
    return byte((int(i) * 7 + salt) & 0xFF);
}

cc::vector<byte> pattern(int salt)
{
    cc::vector<byte> out;
    out.reserve(k_bytes);
    for (isize i = 0; i < k_bytes; ++i)
        out.push_back(pattern_at(i, salt));
    return out;
}

bool matches(cc::span<byte const> bytes, int salt)
{
    if (bytes.size() != k_bytes)
        return false;
    for (isize i = 0; i < k_bytes; ++i)
        if (bytes[i] != pattern_at(i, salt))
            return false;
    return true;
}
} // namespace

TEST("sg vulkan - an async texture readback hands the texture back in the layout it found")
{
    auto const ctx = sg::backend::vulkan::test::make_context();
    if (ctx == nullptr)
        SKIP("no vulkan device");

    // Sampled as well as copyable, so the resting layout is a specific one rather than a transfer layout — which is
    // what makes borrowing observable at all.
    auto const tex = ctx->persistent.create_raw_texture(
        {.format = sg::pixel_format::rgba8_unorm,
         .dimension = sg::texture_dimension::d2,
         .width = k_extent,
         .height = k_extent,
         .usage = sg::texture_usage::copy_src | sg::texture_usage::copy_dst | sg::texture_usage::readonly_texture});
    REQUIRE(tex != nullptr);

    auto const first = pattern(13);
    {
        auto up = ctx->create_command_list();
        REQUIRE(up != nullptr);
        up->upload.bytes_to_texture(tex, cc::span<byte const>(first));
        ctx->submit_command_list(cc::move(up));
    }

    // The borrow: reads the texture on the transfer queue, and must leave the layout as it was.
    auto const streamed = ctx->wait_for(ctx->download.bytes_from_texture(tex));
    REQUIRE(streamed.has_value());
    CHECK(matches(streamed.value(), 13));

    // A list recorded after the borrow, whose barriers were computed against that unchanged layout.
    // If the transfer had left the image in `general`, these name a layout it is not in.
    auto const second = pattern(29);
    {
        auto up = ctx->create_command_list();
        REQUIRE(up != nullptr);
        up->upload.bytes_to_texture(tex, cc::span<byte const>(second));
        ctx->submit_command_list(cc::move(up));
    }

    auto down = ctx->create_command_list();
    REQUIRE(down != nullptr);
    auto future = down->download.bytes_from_texture(tex);
    ctx->submit_command_list(cc::move(down));

    auto const bytes = ctx->wait_for(future);
    REQUIRE(bytes.has_value());
    CHECK(matches(bytes.value(), 29)).context("the texture did not survive an async transfer taken in between");

    ctx->advance_epoch_and_wait_for_idle();
}
