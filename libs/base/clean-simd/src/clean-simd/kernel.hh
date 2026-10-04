#pragma once

#include <clean-core/common/macros.hh>
#include <clean-simd/fwd.hh>

/// The kernels: one tag type per instruction set a cimd type can be compiled for.
///
/// Every type is templated on a kernel, `cimd::f32x8<K>`, and production code stays templated on it so one binary
/// can dispatch between kernels at run time (dispatch.hh).
/// `cimd::local` names the best kernel this TU's flags allow, for code that only ever runs on the machine it was built
/// for; there is no default argument, so local code says so.
///
/// Every tag exists on every target, so a dispatch table can name `avx512` on arm64.
/// What exists only where the TU's flags allow it is the kernel's code: `cimd::is_available<K>` says which, and using a
/// type of an unavailable kernel is a static_assert naming the way out.

enum class cimd::kernel_id : cimd::u8
{
    scalar,
    sse2,
    sse42,
    avx2,
    avx512,
    neon,
    simd128,
};

/// Plain C++, one 128-bit "register" held as an array; the reference every other kernel is tested against.
struct cimd::scalar
{
    static constexpr kernel_id id = kernel_id::scalar;
    static constexpr int native_bits = 128;
    static constexpr bool has_native_fma = false;
};

/// x86-64-v1.
struct cimd::sse2
{
    static constexpr kernel_id id = kernel_id::sse2;
    static constexpr int native_bits = 128;
    static constexpr bool has_native_fma = false;
};

/// x86-64-v2: SSE4.2 and POPCNT.
struct cimd::sse42
{
    static constexpr kernel_id id = kernel_id::sse42;
    static constexpr int native_bits = 128;
    static constexpr bool has_native_fma = false;
};

/// x86-64-v3: AVX2, FMA, F16C, BMI1/2.
struct cimd::avx2
{
    static constexpr kernel_id id = kernel_id::avx2;
    static constexpr int native_bits = 256;
    static constexpr bool has_native_fma = true;
};

/// x86-64-v4: AVX-512 F, BW, CD, DQ and VL; masks are k-registers.
struct cimd::avx512
{
    static constexpr kernel_id id = kernel_id::avx512;
    static constexpr int native_bits = 512;
    static constexpr bool has_native_fma = true;
};

/// arm64 AdvSIMD, part of the base architecture.
struct cimd::neon
{
    static constexpr kernel_id id = kernel_id::neon;
    static constexpr int native_bits = 128;
    static constexpr bool has_native_fma = true;
};

/// WebAssembly SIMD128.
struct cimd::simd128
{
    static constexpr kernel_id id = kernel_id::simd128;
    static constexpr int native_bits = 128;
    static constexpr bool has_native_fma = false;
};

namespace cimd
{
template <class K>
concept kernel = requires {
    K::id;
    K::native_bits;
};
} // namespace cimd

// Which kernels this TU's flags allow, as 0/1 macros so a generated header can be skipped entirely where its
// intrinsics would not compile.
// MSVC reports __AVX2__ and the AVX-512 macros but none of the SSE levels, which is what CC_X64_LEVEL is for.

#if defined(CC_ARCH_X64)
#define CIMD_HAS_SSE2 1
#if defined(__SSE4_2__) || (defined(CC_X64_LEVEL) && CC_X64_LEVEL >= 2) || defined(__AVX2__)
#define CIMD_HAS_SSE42 1
#else
#define CIMD_HAS_SSE42 0
#endif
#if defined(__AVX2__) && (defined(__FMA__) || defined(CC_COMPILER_MSVC))
#define CIMD_HAS_AVX2 1
#else
#define CIMD_HAS_AVX2 0
#endif
#if defined(__AVX512F__) && defined(__AVX512VL__) && defined(__AVX512BW__) && defined(__AVX512DQ__) && CIMD_HAS_AVX2
#define CIMD_HAS_AVX512 1
#else
#define CIMD_HAS_AVX512 0
#endif
#else
#define CIMD_HAS_SSE2 0
#define CIMD_HAS_SSE42 0
#define CIMD_HAS_AVX2 0
#define CIMD_HAS_AVX512 0
#endif

#if defined(CC_ARCH_ARM64)
#define CIMD_HAS_NEON 1
#else
#define CIMD_HAS_NEON 0
#endif

#if defined(__wasm_simd128__)
#define CIMD_HAS_SIMD128 1
#else
#define CIMD_HAS_SIMD128 0
#endif

namespace cimd
{
/// Whether this TU can compile kernel `K`'s code.
template <class K>
inline constexpr bool is_available = //
    K::id == kernel_id::scalar ||    //
    (K::id == kernel_id::sse2 && CIMD_HAS_SSE2) || (K::id == kernel_id::sse42 && CIMD_HAS_SSE42)
    || (K::id == kernel_id::avx2 && CIMD_HAS_AVX2) || (K::id == kernel_id::avx512 && CIMD_HAS_AVX512)
    || (K::id == kernel_id::neon && CIMD_HAS_NEON) || (K::id == kernel_id::simd128 && CIMD_HAS_SIMD128);

/// The best kernel this TU's flags allow.
///
/// It differs between TUs compiled with different flags, so it belongs in code that runs where it was built; a
/// function shared between TUs takes `K` as a template parameter instead.
#if CIMD_HAS_AVX512
using local = avx512;
#elif CIMD_HAS_AVX2
using local = avx2;
#elif CIMD_HAS_SSE42
using local = sse42;
#elif CIMD_HAS_SSE2
using local = sse2;
#elif CIMD_HAS_NEON
using local = neon;
#elif CIMD_HAS_SIMD128
using local = simd128;
#else
using local = scalar;
#endif

/// Lanes of `T` in kernel `K`'s widest register.
template <class K, class T>
inline constexpr int native_lanes = K::native_bits / int(8 * sizeof(T));

/// The kernel's name as its tag is spelled, for diagnostics and test names.
constexpr char const* kernel_name(kernel_id id)
{
    switch (id)
    {
    case kernel_id::scalar:
        return "scalar";
    case kernel_id::sse2:
        return "sse2";
    case kernel_id::sse42:
        return "sse42";
    case kernel_id::avx2:
        return "avx2";
    case kernel_id::avx512:
        return "avx512";
    case kernel_id::neon:
        return "neon";
    case kernel_id::simd128:
        return "simd128";
    }
    return "?";
}
} // namespace cimd
