#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh> // cc::byte
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

#include <atomic>

using namespace cc::primitive_defines;

// Backend-agnostic async TEXTURE transfer (ctx.upload / ctx.download): the texture half of upload-async-test.cc and
// download-async-test.cc, which are buffer-only.
//
// The contract is the same one those files pin for buffers, and it is sg's rather than a backend's: an async transfer
// is ordered against command lists automatically, in both directions, and the caller writes no barrier and polls no
// future to get it.
//
// A texture carries one obligation a buffer does not — a layout — and it is what splits this file from its neighbour.
// The cases here need no command list at all, so nothing has to compose with a layout the transfer queue cannot
// settle for itself.
// The interleaved ones live in texture-async-interleave-test.cc, and the layout is settled for them by the direct
// queue before the transfer runs.
// See libs/graphics/shaped-graphics/docs/concepts/upload.async.md and libs/graphics/shaped-graphics/docs/concepts/barriers.md.

namespace
{
constexpr int k_extent = 16;
constexpr isize k_bytes = isize(k_extent) * k_extent * 4;

byte pattern_at(isize i, int salt)
{
    return byte((int(i) * 7 + salt) & 0xFF);
}

cc::pinned_data<byte const> pinned_pattern(isize n, int salt)
{
    cc::vector<byte> out;
    out.reserve(n);
    for (isize i = 0; i < n; ++i)
        out.push_back(pattern_at(i, salt));
    return cc::make_pinned_data(cc::move(out));
}

// copy_src + copy_dst, plus sampled — so the layout the texture starts in is a *specific* one rather than a transfer
// layout, which is what gives the fixup something to do.
sg::raw_texture_handle make_transfer_texture(sg::context_handle const& ctx)
{
    sg::texture_description desc;
    desc.format = sg::pixel_format::rgba8_unorm;
    desc.dimension = sg::texture_dimension::d2;
    desc.width = k_extent;
    desc.height = k_extent;
    desc.usage = sg::texture_usage::copy_src | sg::texture_usage::copy_dst | sg::texture_usage::readonly_texture;
    auto tex = ctx->persistent.create_raw_texture(desc);
    CC_ASSERT(tex != nullptr, "async texture transfer test allocation failed");
    return tex;
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

INVOCABLE_TEST("sg - async texture upload then download round-trips", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const tex = make_transfer_texture(ctx);

    // Fire-and-forget in both directions: no wait between them, so the readback has to order itself behind the copy.
    ctx->upload.bytes_to_texture(tex, pinned_pattern(k_bytes, 11));

    auto const back = ctx->wait_for(ctx->download.bytes_from_texture(tex));
    REQUIRE(back.has_value());
    CHECK(matches(back.value(), 11));
}

// The lifetime gate, mirroring "async upload to a dropped buffer still releases it".
// A texture whose last handle goes while a copy to it is still queued must not have its storage freed when the epoch
// retires — the transfer queue is decoupled from epochs, so only the copy's own completion value says it is done.
// The drop-before-stage race cannot be forced from the public API, but the release invariant must hold either way.
// A kept texture's later transfer drives the copy timeline past the dropped one's value, since the actor processes
// jobs in order.
INVOCABLE_TEST("sg - async upload to a dropped texture still releases it", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto released = std::make_shared<std::atomic<bool>>(false);
    auto const keep = make_transfer_texture(ctx);

    {
        auto const dropped = make_transfer_texture(ctx);
        dropped->add_finalizer([released] { released->store(true, std::memory_order_release); });
        ctx->upload.bytes_to_texture(dropped, pinned_pattern(k_bytes, 43));
    } // last handle gone: storage scheduled for deferred deletion, gated on the copy's completion value

    ctx->upload.bytes_to_texture(keep, pinned_pattern(k_bytes, 47));
    REQUIRE(ctx->wait_for(ctx->download.bytes_from_texture(keep)).has_value());

    // Two advances, so the epoch the dropped texture died in is fully retired and swept.
    ctx->advance_epoch_and_wait_for_idle();
    ctx->advance_epoch_and_wait_for_idle();
    ctx->process_completed_epochs();

    CHECK(released->load(std::memory_order_acquire));
}
