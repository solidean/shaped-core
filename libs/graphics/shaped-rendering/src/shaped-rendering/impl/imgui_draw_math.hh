#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/error/optional.hh>
#include <shaped-rendering/fwd.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

/// The arithmetic behind the imgui renderer, lifted out of it so it can be tested without a device.
/// Deliberately free of imgui types: the caller converts ImVec2 / ImVec4 / ImTextureRect at the boundary, which keeps these functions checkable against hand-computed numbers.

namespace sr::impl
{
/// The 16-byte inline-constants payload imgui.hlsl's `imgui_constants` expects.
/// Layout must match the cbuffer exactly — two float2s packed into one 16-byte register.
struct imgui_ortho_constants
{
    tg::vec2f scale;
    tg::vec2f translate;
};
static_assert(sizeof(imgui_ortho_constants) == 16, "must match imgui.hlsl's imgui_constants cbuffer");

/// Maps imgui's display space (pixels, origin top-left, y down) onto clip space (origin center, y up).
/// The y axis flips, which is why scale.y is negative for a normal top-left display origin.
/// `display_pos` is nonzero only under multi-viewport; folding it in costs nothing and is what a per-viewport pass would otherwise have to add.
[[nodiscard]] imgui_ortho_constants compute_ortho_constants(tg::pos2f display_pos, tg::vec2f display_size);

/// Converts one ImDrawCmd::ClipRect into a scissor rect in framebuffer pixels: translate out of display space, scale to framebuffer pixels, then clamp to the target.
/// Returns nothing when the result is empty after clamping — a fully-offscreen draw, which must be skipped rather than submitted as a zero-area scissor.
[[nodiscard]] cc::optional<tg::aabb2i> compute_scissor(tg::aabb2f const& clip_rect,
                                                       tg::pos2f display_pos,
                                                       tg::vec2f framebuffer_scale,
                                                       tg::vec2i target_size);

/// Copies the `size` rect at `offset` out of a `pitch`-strided image into a freshly allocated, pinned, tightly packed buffer — the layout sg's texture uploads require.
///
/// A fresh allocation per call is deliberate, not wasteful: ctx.upload is fire-and-forget and holds the pin until the copy has consumed it,
/// so a buffer reused across uploads would be rewritten while a copy was still reading it.
///
/// `pixels` must cover the rect: offset + size within the image, pitch >= image width * bytes_per_pixel.
[[nodiscard]] cc::pinned_data<cc::byte const> pack_texture_rect(cc::byte const* pixels,
                                                                isize pitch,
                                                                isize bytes_per_pixel,
                                                                tg::pos2i offset,
                                                                tg::vec2i size);
} // namespace sr::impl
