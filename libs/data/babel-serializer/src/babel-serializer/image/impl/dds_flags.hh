#pragma once

#include <babel-serializer/image/dds.hh>

// The legacy pixel-format flags, shared by the reader and the decoder.

namespace babel::dds::impl
{
inline constexpr u32 pf_alpha_pixels = 0x1;
inline constexpr u32 pf_alpha = 0x2;
inline constexpr u32 pf_four_cc = 0x4;
inline constexpr u32 pf_rgb = 0x40;
inline constexpr u32 pf_luminance = 0x20000;

/// The alpha mask that counts: a legacy header's is meaningful only under one of the two alpha flags.
[[nodiscard]] inline u32 effective_alpha_mask(pixel_format_header const& pf)
{
    return (pf.flags & (pf_alpha_pixels | pf_alpha)) != 0 ? pf.a_mask : 0;
}
} // namespace babel::dds::impl
