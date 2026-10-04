#pragma once

#include <clean-simd/kernel.hh>

/// `cimd::simd<T, N, K>`: N lanes of T, compiled for kernel K — and its storage and mask counterparts.
///
/// **The layout is the same on every kernel.**
/// `sizeof == N * sizeof(T)`, `alignof == min(sizeof, 64)`, and lane `i` lives at byte `i * sizeof(T)`.
/// So `f32x8<sse2>` and `f32x8<avx512>` hold the same bytes, and `cimd::storage<T, N>` is the kernel-free form a data
/// structure keeps: it converts to and from every kernel's type with one aligned load or store.
///
/// The value types are generated per element type and register count (generated/fixed/), on top of one register
/// layer per kernel (generated/<kernel>/).
/// Everything up to eight registers is flat code with no loop; `T::is_loop_free` is the static fact a hot path asserts.
/// shuffle and permute are the exception: they go through memory, picking one lane at a time.
///
/// A mask is opaque (`cimd::mask<Bits, N, K>`, `m32x8<K>`): one per lane width, so a float comparison selects integers.
/// It has no layout guarantee — on avx512 it is a k-register.

namespace cimd::impl
{
template <class T, int N>
inline constexpr int alignment = int(sizeof(T)) * N < 64 ? int(sizeof(T)) * N : 64;

/// The register width kernel `K` holds `N` lanes of `LaneBits` in: the type's own width up to the kernel's native one.
template <int LaneBits, int N, class K>
inline constexpr int lane_reg_bits = LaneBits * N < K::native_bits ? LaneBits * N : K::native_bits;

/// How many registers of `lane_reg_bits` the type spans; 1, 2, 4 and 8 are generated flat.
template <int LaneBits, int N, class K>
inline constexpr int lane_reg_count = LaneBits * N / lane_reg_bits<LaneBits, N, K>;

template <class T, int N, class K>
inline constexpr int reg_bits = lane_reg_bits<int(8 * sizeof(T)), N, K>;

template <class T, int N, class K>
inline constexpr int reg_count = lane_reg_count<int(8 * sizeof(T)), N, K>;

template <class T, int N>
inline constexpr bool valid_shape = N > 0 && (N & (N - 1)) == 0 && int(sizeof(T)) * N >= 16;

} // namespace cimd::impl

/// N lanes of T for data structures: no kernel, no operations, converts to and from every `simd<T, N, K>`.
template <class T, int N>
struct alignas(cimd::impl::alignment<T, N>) cimd::storage
{
    T lanes[N];
};

/// The primary template is reached only by a shape or a kernel that has no code here, so all it does is say which.
template <class T, int N, class K>
struct cimd::simd
{
    static_assert(impl::valid_shape<T, N>,
                  "cimd::simd<T, N, K>: N must be a power of two and the type at least 128 bits");
    static_assert(is_available<K>,
                  "cimd::simd<T, N, K>: this TU's flags do not allow kernel K. Code above the build's floor is "
                  "compiled "
                  "through cimd_dispatch (clean-simd/dispatch.hh); local code uses cimd::local");
    static_assert(impl::reg_count<T, N, K> <= 8, "cimd::simd<T, N, K>: more than eight registers is not generated yet");
    static constexpr bool generated = false;
};

template <int LaneBits, int N, class K>
struct cimd::mask
{
    static_assert(is_available<K>, "cimd::mask: this TU's flags do not allow kernel K; see cimd::simd");
    static constexpr bool generated = false;
};

namespace cimd
{
/// Kernel K's widest register of T.
template <class K, class T>
using native = simd<T, native_lanes<K, T>, K>;

} // namespace cimd

// f32x8<K>, m32x8<K>, f32x8_storage and the rest, for every element.
#include <clean-simd/generated/aliases.hh>
