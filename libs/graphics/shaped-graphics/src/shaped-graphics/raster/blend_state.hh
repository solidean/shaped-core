#pragma once

#include <clean-core/common/flags.hh>
#include <shaped-graphics/fwd.hh>

/// Color-blend state for one render target of a raster pipeline: how a fragment's output combines with the value already in the target.
/// Backend-neutral — each enumerator maps to the per-backend factor / op.

/// A blend factor — the coefficient a source or destination color/alpha is multiplied by before the
/// blend op combines them.
enum class sg::blend_factor
{
    zero,                // DX12 BLEND_ZERO           / Vk FACTOR_ZERO
    one,                 // DX12 BLEND_ONE            / Vk FACTOR_ONE
    src_color,           // DX12 BLEND_SRC_COLOR      / Vk FACTOR_SRC_COLOR
    one_minus_src_color, // DX12 BLEND_INV_SRC_COLOR  / Vk FACTOR_ONE_MINUS_SRC_COLOR
    dst_color,           // DX12 BLEND_DEST_COLOR     / Vk FACTOR_DST_COLOR
    one_minus_dst_color, // DX12 BLEND_INV_DEST_COLOR / Vk FACTOR_ONE_MINUS_DST_COLOR
    src_alpha,           // DX12 BLEND_SRC_ALPHA      / Vk FACTOR_SRC_ALPHA
    one_minus_src_alpha, // DX12 BLEND_INV_SRC_ALPHA  / Vk FACTOR_ONE_MINUS_SRC_ALPHA
    dst_alpha,           // DX12 BLEND_DEST_ALPHA     / Vk FACTOR_DST_ALPHA
    one_minus_dst_alpha, // DX12 BLEND_INV_DEST_ALPHA / Vk FACTOR_ONE_MINUS_DST_ALPHA
};

/// How the weighted source and destination values are combined.
enum class sg::blend_op
{
    add,              // DX12 BLEND_OP_ADD          / Vk BLEND_OP_ADD
    subtract,         // DX12 BLEND_OP_SUBTRACT     / Vk BLEND_OP_SUBTRACT      (src - dst)
    reverse_subtract, // DX12 BLEND_OP_REV_SUBTRACT / Vk BLEND_OP_REVERSE_SUBTRACT (dst - src)
    min,              // DX12 BLEND_OP_MIN          / Vk BLEND_OP_MIN
    max,              // DX12 BLEND_OP_MAX          / Vk BLEND_OP_MAX
};

/// One color channel a render target write may touch.
/// A set of them is a `color_write_mask` — combine with `|`, test with `has`.
/// Maps to DX12 D3D12_COLOR_WRITE_ENABLE / Vk VkColorComponentFlags.
enum class sg::color_channel : sg::u8
{
    r,
    g,
    b,
    a,
};

CC_FLAG_ENUM_INDEXED(sg, color_channel, u8);

namespace sg
{
/// A SET of color_channel — which channels a render target write touches, never the bare enum.
using color_write_mask = cc::flags<color_channel>;

/// Every channel — the default write mask, since the empty set discards the fragment's output entirely.
inline constexpr color_write_mask color_write_mask_all
    = color_channel::r | color_channel::g | color_channel::b | color_channel::a;
} // namespace sg

/// One channel group's blend: `src * source + dst * ...` combined by `op`.
/// Used for the color and alpha groups independently.
struct sg::blend_component
{
    blend_factor source = blend_factor::one;
    blend_factor target = blend_factor::zero;
    blend_op op = blend_op::add;

    [[nodiscard]] friend constexpr bool operator==(blend_component, blend_component) = default;
};

/// The color-blend equation for a render target, with separate color and alpha components — DX12 and Vulkan both blend RGB and A independently.
/// Present on a color_target_state only when blending is enabled.
/// Equality is what lets a pipeline cache key on a blend, since a blend is baked into the PSO while a sampler or a
/// push constant is not.
struct sg::blend_state
{
    blend_component color = {};
    blend_component alpha = {};

    [[nodiscard]] friend constexpr bool operator==(blend_state, blend_state) = default;
};
