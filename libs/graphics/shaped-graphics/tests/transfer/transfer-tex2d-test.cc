#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/pixel_format.hh>
#include <shaped-graphics/raw_texture.hh>
#include <shaped-graphics/types.hh>


// KNOWN-BUG reproduction — dx12 cross-queue 2D-texture layout desync between the inline (cmd.upload/download,
// DIRECT queue) and async (ctx.upload/download, COPY queue) transfer paths. Uncovered by
// transfer-tex2d-fuzz-test.cc (whose async ops are #if 0'd off for the same reason).
//
// DISABLED via #if 0: the sequence below is data-correct (no CHECK fails), so nothing but the D3D12 debug
// layer notices — it just floods stderr with barrier-layout errors. An INVOCABLE_TEST is always run by the
// backend driver (nx::invoke_tests ignores nx::config::disabled), so #if 0 is the only way to keep it out of
// the green suite. It has been confirmed to reproduce (4 barrier-layout errors from this one sequence); flip
// the guard to #if 1 and run it against dx12 to see them:
//   uv run dev.py test "sg - tex2d inline+async cross-queue layout (KNOWN BUG)"
//   grep -E "must be in expected layout|does not match expected layout" \
//     build/*/run-logs/run-log-shaped-graphics-test.stderr.txt
//
// What goes wrong
// ---------------
// The inline path transitions a texture to COPY_DEST / COPY_SOURCE on the DIRECT queue, and at submit the
// last command list using it promotes that copy layout as the resting ("canonical") layout — the texture is
// left physically in COPY_DEST / COPY_SOURCE (dx12_command_list.cc upload_bytes_to_texture /
// download_bytes_from_texture + dx12_texture_access.hh finalize). The async path runs CopyTextureRegion on a
// D3D12 COPY queue and does *only* fence cross-queue sync — it never touches the layout tracker
// (dx12_upload_async.cc / dx12_download_async.cc). But a COPY queue requires its resources in the COMMON
// layout, so:
//   1. an inline copy followed by an async copy hits the texture while it is still COPY_DEST / COPY_SOURCE —
//      illegal on the COPY queue; and
//   2. the COPY queue implicitly decays the texture to COMMON, but the tracker's canonical still reads
//      copy_*, so the *next* inline op emits a barrier whose layoutBefore no longer matches the resource.
//
// The fix is a barrier-system decision (rest copy-usage textures in COMMON at the inline<->async boundary);
// it lands in the same PR as a follow-up, after which this test should assert-clean and be re-enabled (drop
// the #if 0) alongside the fuzz async ops.

#if 0
namespace
{
constexpr int tex_w = 64;
constexpr int tex_h = 64;
constexpr int bpt = 4; // rgba8_unorm: 4 bytes per texel

// Deterministic per-byte pattern so producer and checker agree without the fuzzer.
cc::byte pattern(cc::isize i) { return cc::byte((i * 2654435761u) >> 24); }
} // namespace

INVOCABLE_TEST("sg - tex2d inline+async cross-queue layout (KNOWN BUG)", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    sg::texture_description desc;
    desc.format = sg::pixel_format::rgba8_unorm;
    desc.dimension = sg::texture_dimension::d2;
    desc.width = tex_w;
    desc.height = tex_h;
    desc.usage = sg::texture_usage::copy_src | sg::texture_usage::copy_dst;
    auto const tex = ctx->persistent.create_raw_texture(desc);
    REQUIRE(tex != nullptr);

    cc::isize const n = cc::isize(tex_w) * tex_h * bpt;
    auto pixels = cc::vector<cc::byte>::create_uninitialized(n);
    for (cc::isize i = 0; i < n; ++i)
        pixels[i] = pattern(i);

    // 1) INLINE upload of the whole subresource: leaves the texture physically in COPY_DEST (promoted to the
    //    canonical resting layout at submit).
    auto up = ctx->create_command_list();
    up->upload.bytes_to_texture(tex, cc::span<cc::byte const>(pixels));
    ctx->submit_command_list(cc::move(up));

    // 2) ASYNC download on the COPY queue while the texture is still COPY_DEST → the COPY queue requires
    //    COMMON, so this emits the barrier-layout debug errors (error kind 1). Data is still correct.
    auto const async_dl = ctx->download.bytes_from_texture(tex);
    auto const async_bytes = ctx->wait_for(async_dl);
    REQUIRE(async_bytes.has_value());
    REQUIRE(async_bytes.value().size() == n);
    bool async_ok = true;
    for (cc::isize i = 0; i < n; ++i)
        if (async_bytes.value()[i] != pattern(i))
            async_ok = false;
    CHECK(async_ok);

    // 3) INLINE download afterwards: the COPY queue implicitly decayed the texture to COMMON, but the tracker
    //    still believes it is copy_*, so this op's barrier layoutBefore mismatches the resource (error kind 2).
    auto down = ctx->create_command_list();
    auto const inline_dl = down->download.bytes_from_texture(tex);
    ctx->submit_command_list(cc::move(down));
    auto const inline_bytes = ctx->wait_for(inline_dl);
    REQUIRE(inline_bytes.has_value());
    REQUIRE(inline_bytes.value().size() == n);
    bool inline_ok = true;
    for (cc::isize i = 0; i < n; ++i)
        if (inline_bytes.value()[i] != pattern(i))
            inline_ok = false;
    CHECK(inline_ok);
}
#endif
