#pragma once

#include <clean-core/container/fixed_array.hh>
#include <shaped-shader-library/fwd.hh>

/// A value followed by the padding GPU memory leaves after it: `Size` bytes in all.
/// SGL aligns a 3-vector to 16 bytes, so a `buffer[float3]` strides by 16 and its host element is
/// `gpu_padded<tg::vec3f, 16>`, which a `tg::vec3f` converts to.
/// A buffer whose elements must stay 12 bytes apart says so in the shader, with `@padding(.none)` on its member.
template <class T, int Size>
struct slib::gpu_padded
{
    static_assert(Size > int(sizeof(T)), "a gpu_padded value must leave room after it");

    T value;
    cc::fixed_array<cc::byte, Size - sizeof(T)> padding = {};

    gpu_padded() = default;
    constexpr gpu_padded(T v) : value(v) {}

    [[nodiscard]] constexpr operator T() const { return value; }
};
