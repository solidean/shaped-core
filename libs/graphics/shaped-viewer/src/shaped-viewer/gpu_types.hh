#pragma once

#include <shaped-viewer/fwd.hh>

/// A boolean packed the way a GPU constant buffer expects one: a single 32-bit lane, `false` == 0, `true` == 1.
/// A C++ `bool` is one byte, so it can never be a cbuffer field directly — every `*_gpu` struct that carries a
/// flag spells it `gpu_boolean` and assigns a plain `bool` to it.
/// The shader side may declare the lane `bool` or `uint`: any non-zero value reads as `true`, which is also why
/// two gpu_booleans compare by truth, not by bit pattern.
struct sv::gpu_boolean
{
    u32 value = 0;

    gpu_boolean() = default;
    constexpr gpu_boolean(bool v) : value(v ? 1u : 0u) {}

    [[nodiscard]] constexpr explicit operator bool() const { return value != 0; }

    [[nodiscard]] friend constexpr bool operator==(gpu_boolean a, gpu_boolean b)
    {
        return (a.value != 0) == (b.value != 0);
    }
};

namespace sv
{

static_assert(sizeof(gpu_boolean) == 4, "gpu_boolean must occupy exactly one 32-bit lane");
} // namespace sv
