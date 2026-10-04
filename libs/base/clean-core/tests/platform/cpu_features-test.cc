#include <clean-core/platform/cpu_features.hh>
#include <nexus/test.hh>

// A test cannot know which CPU it runs on, so nothing here asserts a feature is present for its own sake.
// What it can know is what the build was compiled for: the binary is running, so the CPU has at least that.

TEST("cc cpu_features - the description is memoized rather than recomputed")
{
    auto const& a = cc::get_cpu_features();
    auto const& b = cc::get_cpu_features();
    CHECK(&a == &b);
}

TEST("cc cpu_features - the CPU has every extension the build was compiled for")
{
    auto const& f = cc::get_cpu_features();
    (void)f;

#if defined(CC_ARCH_X64)
    CHECK(f.sse2);
#endif
#if defined(__SSE4_2__)
    CHECK(f.sse42);
    CHECK(f.popcnt);
#endif
#if defined(__AVX2__)
    CHECK(f.avx);
    CHECK(f.avx2);
#endif
#if defined(__FMA__)
    CHECK(f.fma);
#endif
#if defined(__AVX512F__)
    CHECK(f.avx512f);
#endif
#if defined(CC_X64_LEVEL)
    CHECK((CC_X64_LEVEL < 2 || f.x86_64_v2));
    CHECK((CC_X64_LEVEL < 3 || f.x86_64_v3));
#endif
#if defined(CC_ARCH_ARM64)
    CHECK(f.neon);
#endif
#if defined(__wasm_simd128__)
    CHECK(f.wasm_simd128);
#endif
}

TEST("cc cpu_features - each x86-64 level implies the one below it and its own parts")
{
    auto const& f = cc::get_cpu_features();
    CHECK((!f.x86_64_v4 || f.x86_64_v3));
    CHECK((!f.x86_64_v3 || f.x86_64_v2));
    CHECK((!f.x86_64_v3 || (f.avx2 && f.fma && f.bmi2)));
    CHECK((!f.x86_64_v4 || (f.avx512f && f.avx512bw && f.avx512dq && f.avx512vl)));
    CHECK((!f.avx2 || f.avx));
    CHECK((!f.avx512vl || f.avx512f));
}
