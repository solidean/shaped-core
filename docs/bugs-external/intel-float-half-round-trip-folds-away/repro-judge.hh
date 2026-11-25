// What both hosts share: the 256 inputs, and the verdict on what the shader wrote.
// The inputs are shaped-core's differential `floats` setup, so element for element this is the run that found it.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace judge
{
constexpr std::uint32_t inputs = 256;
constexpr std::uint32_t outputs = 2 * inputs;

inline std::uint32_t bits_of(float f)
{
    auto u = std::uint32_t{};
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

inline std::uint32_t hashed(std::uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

/// -64 to 64, never zero: small fractions, whole numbers and values just off them, as the corpus's setup writes them.
/// Every one is a normal half's magnitude or more, so a half holds it exactly where its low 13 bits are 0.
inline std::vector<float> corpus_inputs()
{
    auto floats = std::vector<float>();
    for (auto i = std::uint32_t(0); i < inputs; ++i)
    {
        auto const unit = float(hashed(i + 0x9e3779b9u) >> 8) / float(1 << 24);
        auto const magnitude = (i % 4 == 0) ? 64.0f * unit : (i % 4 == 1) ? unit : float(i % 17) + 0.5f * unit;
        auto const value = magnitude > 1.0f / 1024.0f ? magnitude : 1.0f / 1024.0f;
        floats.push_back(i % 2 == 0 ? value : -value);
    }
    return floats;
}

/// How many of `count` results from `got` are no half: a half holds 11 significant bits, so a normal float with any of
/// its low 13 bits set is none, whichever way the conversion rounded.
inline int not_halves(float const* got, std::uint32_t count)
{
    auto n = 0;
    for (auto i = std::uint32_t(0); i < count; ++i)
        n += (bits_of(got[i]) & 0x1fffu) != 0 ? 1 : 0;
    return n;
}

/// Prints the verdict on `got` as one line, and whether every result is a half.
/// The first input no half holds is named, with what each spelling gave for it.
inline bool judged(std::vector<float> const& in, float const* got, char const* adapter)
{
    auto const casts = not_halves(got, inputs);
    auto const intrinsics = not_halves(got + inputs, inputs);
    std::printf("casts not a half=%d intrinsics not a half=%d of %u", casts, intrinsics, inputs);
    for (auto i = std::uint32_t(0); i < inputs; ++i)
        if ((bits_of(in[i]) & 0x1fffu) != 0)
        {
            std::printf(" (input [%u] 0x%08x: casts 0x%08x, intrinsics 0x%08x)", i, bits_of(in[i]), bits_of(got[i]),
                        bits_of(got[inputs + i]));
            break;
        }
    std::printf(" | %s\n", adapter);
    return casts == 0 && intrinsics == 0;
}
} // namespace judge
