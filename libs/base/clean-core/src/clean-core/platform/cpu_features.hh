#pragma once

#include <clean-core/fwd.hh>

/// Which instruction-set extensions this CPU and OS let the process use — the question runtime dispatch asks.
///
/// A DESCRIPTION, like cc::system_info: nothing in it changes while the process runs, so it is computed once and
/// returned by reference.
/// It is kept out of cc::system_info because a dispatch path asks it, and that wants a few bools rather than a struct
/// that allocates and reads the OS on first use.
///
/// **A feature is reported only when the OS also saves its registers.**
/// AVX needs the OS to save ymm and AVX-512 needs it to save zmm and the k-masks; a CPU that has the instructions under
/// an OS that does not save them faults on use, so its flag here is false.
///
/// **A flag that is false means "not usable", never "unknown".**
/// On a target where a question cannot be answered — the x86 flags on arm64 — the answer is false, since nothing could
/// use the extension anyway.
/// Where an arm64 extension is not asked of the OS yet, it is absent from the struct rather than reported false.
///
/// On wasm the answer is the build's: a module that uses SIMD128 does not validate in an engine without it, so a module
/// that runs at all has exactly what it was compiled with.
struct cc::cpu_features
{
    // x86 — the individual extensions the x86-64 levels are made of.
    bool sse2 = false;
    bool sse3 = false;
    bool ssse3 = false;
    bool sse41 = false;
    bool sse42 = false;
    bool popcnt = false;
    bool cx16 = false;
    bool lahf = false;
    bool avx = false;
    bool avx2 = false;
    bool fma = false;
    bool f16c = false;
    bool bmi1 = false;
    bool bmi2 = false;
    bool lzcnt = false;
    bool movbe = false;
    bool avx512f = false;
    bool avx512bw = false;
    bool avx512cd = false;
    bool avx512dq = false;
    bool avx512vl = false;

    // x86 — the psABI levels a build is compiled for (SC_X64_LEVEL), each including the one below.
    bool x86_64_v2 = false; ///< SSE3, SSSE3, SSE4.1, SSE4.2, POPCNT, CX16, LAHF-SAHF
    bool x86_64_v3 = false; ///< v2 + AVX, AVX2, FMA, F16C, BMI1, BMI2, LZCNT, MOVBE, with ymm saved by the OS
    bool x86_64_v4 = false; ///< v3 + AVX-512 F, BW, CD, DQ, VL, with zmm and the k-masks saved by the OS

    // arm64 — AdvSIMD is part of the base architecture, so this is true on every arm64 CPU.
    bool neon = false;

    // wasm — what the module was compiled with, which is what the engine runs.
    bool wasm_simd128 = false;
};

namespace cc
{
/// The CPU's usable extensions, computed on the first call and reused by every later one.
///
/// Thread-safe, allocation-free, and the returned reference stays valid for the rest of the process.
/// On macOS x86-64, AVX-512 reads as absent: XNU enables its register state lazily, on a thread's first AVX-512 instruction, so XCR0's opmask and ZMM bits read clear.
[[nodiscard]] cc::cpu_features const& get_cpu_features();
} // namespace cc
