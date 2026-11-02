#pragma once

#include <clean-core/common/utility.hh>
#include <shaped-rendering/fwd.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/scalar.hh>

// The sub-pixel sequence sr hands every member that leaves the sequence to its caller: MetalFX's two scalers and DLSS.
// FSR has one of its own, which `fsr_upscale_routine::jitter` answers instead.

namespace sr::impl
{
/// Element `index` of the Halton sequence in `base`, in [0, 1).
[[nodiscard]] inline f32 halton(u32 index, u32 base)
{
    auto f = 1.0f;
    auto r = 0.0f;
    for (auto i = index; i > 0; i /= base)
    {
        f /= f32(base);
        r += f * f32(i % base);
    }
    return r;
}

/// Frame `frame_index`'s offset, in input pixels in [-0.5, 0.5), as sr's jitter convention reads it.
/// A Halton (2, 3) sequence whose period is 8 times the square of the ratio of the two extents, as FSR's.
[[nodiscard]] inline tg::vec2f halton_jitter(u32 frame_index, tg::vec2i input_extent, tg::vec2i output_extent)
{
    auto const ratio = input_extent[0] > 0 ? f32(output_extent[0]) / f32(input_extent[0]) : 1.0f;
    auto const phases = cc::max(u32(1), u32(tg::ceil(8.0f * ratio * ratio)));
    // Halton's first element is 0 in either base, a sample on the centre; the sequence starts at 1 to skip it.
    auto const i = frame_index % phases + 1;
    return tg::vec2f(halton(i, 2) - 0.5f, halton(i, 3) - 0.5f);
}
} // namespace sr::impl
