#pragma once

#include <shaped-shader-library/fwd.hh>

/// A bool as GPU memory holds one: a single 32-bit lane, `false` == 0, `true` == 1.
/// A C++ `bool` is one byte, so it can never be a field of a constant block or a buffer element directly.
/// SGL's `bool32` is this type on the host, and a hand-written GPU struct spells its flags with it and assigns a plain `bool`.
/// A shader reads any non-zero lane as `true`, which is also why two gpu_bools compare by truth, not by bit pattern.
struct slib::gpu_bool
{
    u32 value = 0;

    gpu_bool() = default;
    constexpr gpu_bool(bool v) : value(v ? 1u : 0u) {}

    [[nodiscard]] constexpr explicit operator bool() const { return value != 0; }

    [[nodiscard]] friend constexpr bool operator==(gpu_bool a, gpu_bool b) { return (a.value != 0) == (b.value != 0); }
};

namespace slib
{
static_assert(sizeof(gpu_bool) == 4, "gpu_bool must occupy exactly one 32-bit lane");
} // namespace slib
