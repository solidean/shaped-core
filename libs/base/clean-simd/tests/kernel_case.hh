#pragma once

#include <clean-simd/all.hh>

// Runs `f.template operator()<K>()` for every kernel this test TU can compile.
// A kernel above the build's floor is not here; its tests go through cimd_dispatch, the way production code does.
template <class F>
void for_each_kernel(F&& f)
{
    f.template operator()<cimd::scalar>();
#if CIMD_HAS_SSE2
    f.template operator()<cimd::sse2>();
#endif
#if CIMD_HAS_SSE42
    f.template operator()<cimd::sse42>();
#endif
#if CIMD_HAS_AVX2
    f.template operator()<cimd::avx2>();
#endif
#if CIMD_HAS_AVX512
    f.template operator()<cimd::avx512>();
#endif
#if CIMD_HAS_NEON
    f.template operator()<cimd::neon>();
#endif
#if CIMD_HAS_SIMD128
    f.template operator()<cimd::simd128>();
#endif
}

// Runs `f.template operator()<K, N>()` for every kernel and every 32-bit lane count all kernels generate flat.
template <class F>
void for_each_kernel_and_width(F&& f)
{
    for_each_kernel(
        [&]<class K>
        {
            f.template operator()<K, 4>();
            f.template operator()<K, 8>();
            f.template operator()<K, 16>();
            f.template operator()<K, 32>();
        });
}

/// The reduction order every kernel promises: lane i with lane i + n/2, recursively.
template <class T, class Op>
T reference_tree(T const* v, int n, Op op)
{
    T tmp[64];
    for (auto i = 0; i < n; ++i)
        tmp[i] = v[i];
    while (n > 1)
    {
        n /= 2;
        for (auto i = 0; i < n; ++i)
            tmp[i] = op(tmp[i], tmp[i + n]);
    }
    return tmp[0];
}
