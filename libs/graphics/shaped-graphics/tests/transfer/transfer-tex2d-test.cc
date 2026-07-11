#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/pixel_format.hh>
#include <shaped-graphics/raw_texture.hh>
#include <shaped-graphics/types.hh>


// HANDOVER — KNOWN dx12 BUG: async texture transfer skips the cross-queue layout hand-off
// =======================================================================================
// The async (ctx.upload/download) texture path runs its copy on a D3D12 COPY queue and does the cross-queue
// *sync* (a fence signal→wait vs the last direct-queue user) but never performs the cross-queue *layout*
// transition. Interleaving it with the inline (cmd.upload/download, DIRECT queue) path therefore hands the
// COPY queue a texture in the wrong layout. Uncovered by transfer-tex2d-fuzz-test.cc, whose two async ops are
// #if 0'd off for the same reason. This is a real defect, not debug-layer noise — the bytes still round-trip
// (so no CHECK fails), but the D3D12 debug layer reports every occurrence.
//
// Why textures and not buffers (this is the subtle part)
// ------------------------------------------------------
// Under *enhanced* barriers a buffer has no state — only access + sync — so a cross-queue fence is a
// sufficient happens-before and no transition is needed. A **texture still has a layout**, and a D3D12 COPY
// queue supports only the COMMON layout for it (COPY_SOURCE / COPY_DEST are direct/compute-queue layouts).
// So a texture, unlike a buffer, cannot cross the queue boundary without first being transitioned to COMMON —
// the fence alone is necessary but not sufficient. We are fully on enhanced barriers for textures
// (d3d12_layout_from → D3D12_BARRIER_LAYOUT_*, emitted via ID3D12GraphicsCommandList7::Barrier in
// dx12_barrier.cc), so this layout requirement is live.
//
// The exact sequence (reproduced below)
// -------------------------------------
// 1. INLINE upload: transitions the texture to COPY_DEST on the DIRECT queue; at submit the last list using
//    it promotes that as the resting ("canonical") layout, so it is left physically in COPY_DEST
//    (dx12_command_list.cc upload_bytes_to_texture / download_bytes_from_texture + dx12_texture_access.hh
//    finalize).
// 2. ASYNC download: the COPY queue reads the texture while it is still COPY_DEST — illegal on a COPY queue,
//    which requires COMMON. (Debug layer: "must be in expected layout (COMMON) ... using
//    D3D12_COMMAND_LIST_TYPE_COPY".) The async path (dx12_upload_async.cc / dx12_download_async.cc) never
//    transitions it, because it does not participate in the layout tracker at all.
// 3. INLINE download: the COPY queue implicitly decayed the texture to COMMON, but the tracker's canonical
//    still reads copy_*, so this op emits a barrier whose layoutBefore no longer matches the resource.
//    (Debug layer: "does not match expected layout (COMMON) ... using D3D12_COMMAND_LIST_TYPE_DIRECT".)
//
// The fix (follow-up in this PR)
// ------------------------------
// Make the async texture path a first-class participant in the layout tracker: before its COPY-queue job,
// transition the texture COPY_* → COMMON on the DIRECT queue (the existing cross-queue fence is the
// happens-before), let the COPY queue read/write it in COMMON, and record COMMON as the new canonical so the
// next inline op has the correct layoutBefore. Cost is paid only when a texture is actually used cross-queue,
// NOT by blanket-resting every copy-touched texture in COMMON. Once it lands, this test should assert-clean
// and be re-enabled (drop the #if 0) together with the fuzz async ops.
//
// Related but out of scope: the buffer async path trips an analogous cross-queue error, but for a different
// reason — buffers are NOT yet on enhanced barriers; they still rely on legacy implicit state promotion/decay
// (dx12_resource_upload.hh: "no barrier is needed. TODO: a real ... barrier system lands later"), so the
// errors there are phrased in legacy D3D12_RESOURCE_STATE_* terms. The buffer fix is that enhanced-barrier
// migration (declare access, rely on the fence), after which buffers carry no state to coordinate.
//
// Disabled via #if 0 (not nx::config::disabled): an INVOCABLE_TEST is always run by the backend driver
// (nx::invoke_tests ignores the disabled config), so #if 0 is the only way to keep it out of the green suite.
// Confirmed to reproduce (4 barrier-layout errors from this one sequence). To see them, flip to #if 1 and:
//   uv run dev.py test "sg - tex2d inline+async cross-queue layout (KNOWN BUG)"
//   grep -E "must be in expected layout|does not match expected layout" \
//     build/*/run-logs/run-log-shaped-graphics-test.stderr.txt

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
