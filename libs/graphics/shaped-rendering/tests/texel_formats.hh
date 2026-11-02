#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/half_float.hh>
#include <typed-geometry/scalar/scalar.hh>

// The few formats sr's GPU tests upload, write and read back, packed and unpacked on the CPU.
//
// A member that writes the caller's image stores in that image's own format, so a test that only ever hands it rgba32_float says nothing about the others.
// These are what a test sweeps instead.

namespace sr_test
{
using namespace cc::primitive_defines;

/// The output formats a member is swept over: its own working format, the half float sv hands it, and a packed unorm one outside the portable set.
inline constexpr sg::pixel_format k_swept_formats[]
    = {sg::pixel_format::rgba32_float, sg::pixel_format::rgba16_float, sg::pixel_format::rgb10a2_unorm};

/// Bytes per texel of a format these helpers know; anything else is a test's mistake.
[[nodiscard]] inline int texel_bytes(sg::pixel_format f)
{
    switch (f)
    {
    case sg::pixel_format::rgba32_float:
        return 16;
    case sg::pixel_format::rg32_float:
    case sg::pixel_format::rgba16_float:
        return 8;
    case sg::pixel_format::rgb10a2_unorm:
        return 4;
    default:
        CC_UNREACHABLE("texel_formats.hh knows rgba32_float, rg32_float, rgba16_float and rgb10a2_unorm");
    }
}

/// `v` as a store into `f` rounds it: to a half, or clamped to [0, 1] and onto the unorm grid.
/// rg32_float keeps its two channels and zeroes the rest, as a readback of one does.
[[nodiscard]] inline tg::vec4f quantized(sg::pixel_format f, tg::vec4f v)
{
    auto const unorm = [](f32 x, f32 steps) { return tg::round(cc::clamp(x, 0.0f, 1.0f) * steps) / steps; };
    switch (f)
    {
    case sg::pixel_format::rgba32_float:
        return v;
    case sg::pixel_format::rg32_float:
        return tg::vec4f(v[0], v[1], 0, 0);
    case sg::pixel_format::rgba16_float:
        return tg::vec4f(tg::f16(v[0]).to_f32(), tg::f16(v[1]).to_f32(), tg::f16(v[2]).to_f32(), tg::f16(v[3]).to_f32());
    case sg::pixel_format::rgb10a2_unorm:
        return tg::vec4f(unorm(v[0], 1023.0f), unorm(v[1], 1023.0f), unorm(v[2], 1023.0f), unorm(v[3], 3.0f));
    default:
        CC_UNREACHABLE("texel_formats.hh knows rgba32_float, rg32_float, rgba16_float and rgb10a2_unorm");
    }
}

/// One step of `f`'s grid in `channel`, relative for a float format and absolute for a unorm one.
/// A GPU's store and `quantized` may round a value near a grid point to its two sides, so a comparison allows one step.
[[nodiscard]] inline f64 store_step(sg::pixel_format f, int channel)
{
    switch (f)
    {
    case sg::pixel_format::rgba16_float:
        return 1.0 / 1024.0;
    case sg::pixel_format::rgb10a2_unorm:
        return channel == 3 ? 1.0 / 3.0 : 1.0 / 1023.0;
    default:
        return 0.0;
    }
}

/// `texels` packed as `f` stores them.
[[nodiscard]] inline cc::vector<byte> packed(sg::pixel_format f, cc::span<tg::vec4f const> texels)
{
    auto const size = texel_bytes(f);
    auto out = cc::vector<byte>::create_filled(texels.size() * size, byte(0));
    for (auto i = isize(0); i < texels.size(); ++i)
    {
        auto const v = quantized(f, texels[i]);
        auto* const dst = out.data() + i * size;
        if (f == sg::pixel_format::rgba16_float)
        {
            u16 const halves[4]
                = {tg::f16(v[0]).bits(), tg::f16(v[1]).bits(), tg::f16(v[2]).bits(), tg::f16(v[3]).bits()};
            cc::memcpy(dst, halves, sizeof(halves));
        }
        else if (f == sg::pixel_format::rgb10a2_unorm)
        {
            auto const bits = u32(tg::round(v[0] * 1023.0f)) | u32(tg::round(v[1] * 1023.0f)) << 10
                            | u32(tg::round(v[2] * 1023.0f)) << 20 | u32(tg::round(v[3] * 3.0f)) << 30;
            cc::memcpy(dst, &bits, sizeof(bits));
        }
        else
            cc::memcpy(dst, &v, size);
    }
    return out;
}

/// `count` texels of `f` read out of `bytes`, widened to four channels with zeros.
[[nodiscard]] inline cc::vector<tg::vec4f> unpacked(sg::pixel_format f, cc::span<byte const> bytes, isize count)
{
    auto const size = texel_bytes(f);
    CC_ASSERT(bytes.size() >= count * size, "a readback holds fewer texels than asked for");
    auto out = cc::vector<tg::vec4f>();
    for (auto i = isize(0); i < count; ++i)
    {
        auto const* const src = bytes.data() + i * size;
        auto v = tg::vec4f(0, 0, 0, 0);
        if (f == sg::pixel_format::rgba16_float)
        {
            u16 halves[4] = {};
            cc::memcpy(halves, src, sizeof(halves));
            for (auto c = 0; c < 4; ++c)
                v[c] = tg::f16::make_from_bits(halves[c]).to_f32();
        }
        else if (f == sg::pixel_format::rgb10a2_unorm)
        {
            auto bits = u32(0);
            cc::memcpy(&bits, src, sizeof(bits));
            v = tg::vec4f(f32(bits & 0x3ffu) / 1023.0f, f32(bits >> 10 & 0x3ffu) / 1023.0f,
                          f32(bits >> 20 & 0x3ffu) / 1023.0f, f32(bits >> 30) / 3.0f);
        }
        else
            cc::memcpy(&v, src, size);
        out.push_back(v);
    }
    return out;
}

/// The worst disagreement between what a pass stored into `f` and what `expected` rounds to there, in units of the reference's own size.
/// Each channel is allowed one step of `f`'s grid on top, since the GPU and `quantized` round separately; the result is what remains beyond that.
[[nodiscard]] inline f64 worst_stored(sg::pixel_format f,
                                      cc::span<tg::vec4f const> got,
                                      cc::span<tg::vec4f const> expected,
                                      int channels)
{
    auto worst = 0.0;
    for (auto i = isize(0); i < expected.size(); ++i)
    {
        auto const e = quantized(f, expected[i]);
        for (auto c = 0; c < channels; ++c)
        {
            auto const scale = cc::max(1.0, tg::abs(f64(e[c])));
            auto const off = tg::abs(f64(got[i][c]) - f64(e[c])) / scale;
            worst = cc::max(worst, cc::max(0.0, off - store_step(f, c)));
        }
    }
    return worst;
}
} // namespace sr_test
