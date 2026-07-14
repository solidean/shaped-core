#pragma once

#include <shaped-graphics/fwd.hh>

/// Color-blend state for one render target of a raster pipeline: how a fragment's output combines with
/// the value already in the target. Backend-neutral; each enumerator maps to the per-backend factor/op.

namespace sg
{
/// A blend factor — the coefficient a source or destination color/alpha is multiplied by before the
/// blend op combines them.
enum class blend_factor
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
enum class blend_op
{
    add,              // DX12 BLEND_OP_ADD          / Vk BLEND_OP_ADD
    subtract,         // DX12 BLEND_OP_SUBTRACT     / Vk BLEND_OP_SUBTRACT      (src - dst)
    reverse_subtract, // DX12 BLEND_OP_REV_SUBTRACT / Vk BLEND_OP_REVERSE_SUBTRACT (dst - src)
    min,              // DX12 BLEND_OP_MIN          / Vk BLEND_OP_MIN
    max,              // DX12 BLEND_OP_MAX          / Vk BLEND_OP_MAX
};

/// Which color channels a render target write touches. Bit flags — combine with `|`, test with
/// `has_flag`. Maps to DX12 D3D12_COLOR_WRITE_ENABLE / Vk VkColorComponentFlags.
enum class color_write_mask : u8
{
    none = 0,
    r = 1u << 0,
    g = 1u << 1,
    b = 1u << 2,
    a = 1u << 3,
    all = r | g | b | a,
};

[[nodiscard]] constexpr color_write_mask operator|(color_write_mask x, color_write_mask y)
{
    return color_write_mask(u8(x) | u8(y));
}

[[nodiscard]] constexpr color_write_mask operator&(color_write_mask x, color_write_mask y)
{
    return color_write_mask(u8(x) & u8(y));
}

/// True if every bit in `flag` is set in `mask`.
[[nodiscard]] constexpr bool has_flag(color_write_mask mask, color_write_mask flag)
{
    return (u8(mask) & u8(flag)) == u8(flag);
}

/// One channel group's blend: `src * source + dst * ...` combined by `op`. Used for the color and alpha
/// groups independently.
struct blend_component
{
    blend_factor source = blend_factor::one;
    blend_factor target = blend_factor::zero;
    blend_op op = blend_op::add;
};

/// The color-blend equation for a render target — separate color and alpha components (DX12/Vulkan both
/// blend RGB and A independently). Present on a color_target_state only when blending is enabled.
struct blend_state
{
    blend_component color = {};
    blend_component alpha = {};
};
} // namespace sg
