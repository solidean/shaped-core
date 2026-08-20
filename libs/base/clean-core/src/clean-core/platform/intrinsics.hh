#pragma once

#include <clean-core/common/macros.hh>

// =========================================================================================================
// Minimal intrinsic declarations
// =========================================================================================================
//
// The sanctioned way to reach a compiler intrinsic in shaped-core, and the counterpart to win32_sanitized.hh.
// The difference is what it does.
// win32_sanitized.hh *includes* <Windows.h> behind guards, while this header includes nothing at all.
// It declares the handful of intrinsics we actually use, and lets the compiler recognize them as intrinsics.
//
// The reason is measured.
// <immintrin.h> is the whole x86 intrinsic surface — AVX-512, FP16, AMX, every extension the compiler knows:
//
//     a TU of `#include <immintrin.h>` + `_mm_pause();`   0.249 s, 45 files entered
//     the same TU declaring _mm_pause here                0.067 s, 12 files entered
//
// spin.hh reached for it to get one instruction, and spin.hh sits under mutex.hh, so most of the repo's TUs paid for the AVX-512 headers.
// docs/notes/build-times.md has the sweep behind that.
//
// -------------------------------------------------------------------------------------------------------
// Why declaring works
// -------------------------------------------------------------------------------------------------------
//
// These are not ordinary functions to be linked against.
//   * clang and gcc expose them as __builtin_*, which needs no declaration and no header whatsoever.
//     So those paths are a plain wrapper over a builtin and declare nothing at all.
//   * MSVC cl.exe recognizes a set of names as intrinsics once they are declared with the right signature.
//     `#pragma intrinsic` then tells it to emit the instruction inline rather than call a library function.
//     This is the documented pattern, and it is what <intrin.h> itself does behind its thousands of other declarations.
//
// clang-cl is CC_COMPILER_CLANG, not MSVC — it has the builtins, so it never takes the declaration path.
//
// **The MSVC declarations must sit at global scope**, which is why they are outside the namespace below.
// `#pragma intrinsic` names a global symbol, and a declaration nested in a namespace is a different function
// that the pragma does not apply to — it would compile and then fail to link.
//
// -------------------------------------------------------------------------------------------------------
// Adding to this file
// -------------------------------------------------------------------------------------------------------
//
// Only add an intrinsic something in clean-core actually calls, and check the MSVC signature against its
// documentation rather than guessing: a declaration that does not match exactly is either rejected or,
// worse, silently accepted as an ordinary function and left to the linker.

#if defined(CC_COMPILER_MSVC)

#if defined(CC_ARCH_X64) || defined(CC_ARCH_X86)
extern "C" void _mm_pause(void);
extern "C" unsigned __int64 __rdtsc(void);
extern "C" unsigned __int64 __rdtscp(unsigned int*);
#pragma intrinsic(_mm_pause, __rdtsc, __rdtscp)
#elif defined(CC_ARCH_ARM64) || defined(CC_ARCH_ARM32)
extern "C" void __yield(void);
#pragma intrinsic(__yield)
#endif

// Extended-precision arithmetic, for wide_arith.hh.
// clang and gcc have __int128 on every architecture and do the whole job in plain C++, so only cl.exe needs these.
#if defined(CC_ARCH_X64)
extern "C" unsigned __int64 _umul128(unsigned __int64, unsigned __int64, unsigned __int64*);
extern "C" __int64 _mul128(__int64, __int64, __int64*);
extern "C" unsigned char _addcarry_u64(unsigned char, unsigned __int64, unsigned __int64, unsigned __int64*);
extern "C" unsigned char _subborrow_u64(unsigned char, unsigned __int64, unsigned __int64, unsigned __int64*);
#pragma intrinsic(_umul128, _mul128, _addcarry_u64, _subborrow_u64)
#endif

#if defined(CC_ARCH_X64) || defined(CC_ARCH_ARM64)
extern "C" unsigned __int64 __umulh(unsigned __int64, unsigned __int64);
extern "C" __int64 __mulh(__int64, __int64);
#pragma intrinsic(__umulh, __mulh)
#endif

#endif // CC_COMPILER_MSVC

namespace cc::impl
{
/// The CPU's spin-wait hint, or nothing on an architecture without one.
/// cc::spin_pause (clean-core/thread/spin.hh) is the public spelling; this is only the per-compiler plumbing.
CC_FORCE_INLINE void cpu_pause()
{
#if defined(CC_ARCH_X64) || defined(CC_ARCH_X86)
#if defined(CC_COMPILER_MSVC)
    _mm_pause();
#else
    __builtin_ia32_pause();
#endif
#elif defined(CC_ARCH_ARM64) || defined(CC_ARCH_ARM32)
#if defined(CC_COMPILER_MSVC)
    __yield();
#else
    __asm__ __volatile__("yield");
#endif
#endif
}

/// Whether this architecture has a cheap userspace cycle counter, i.e. whether read_cycles() means anything.
constexpr bool has_cycle_counter()
{
#if defined(CC_ARCH_X64) || defined(CC_ARCH_X86)
    return true;
#else
    return false;
#endif
}

/// The raw timestamp counter, or 0 where there is none.
/// cc::current_cycles (clean-core/common/time.hh) is the public spelling.
CC_FORCE_INLINE unsigned long long read_cycles()
{
#if defined(CC_ARCH_X64) || defined(CC_ARCH_X86)
#if defined(CC_COMPILER_MSVC)
    return __rdtsc();
#else
    return __builtin_ia32_rdtsc();
#endif
#else
    // ARM's CNTVCT_EL0 is readable from userspace on Linux but not reliably elsewhere, and MSVC reaches
    // it only through <intrin.h> constants.
    // Not worth the header for a counter nothing measures on yet.
    return 0;
#endif
}

/// The timestamp counter plus the IA32_TSC_AUX word the OS puts the core id in, or 0 for both where there is none.
/// cc::current_cycles_and_core (clean-core/common/time.hh) is the public spelling.
///
/// RDTSCP costs roughly ten cycles more than RDTSC and waits for prior instructions to retire, which RDTSC does not.
/// It still does not stop LATER instructions from being hoisted above it, so a reading is ordered on one side only.
CC_FORCE_INLINE unsigned long long read_cycles_and_core(unsigned int& core_out)
{
#if defined(CC_ARCH_X64) || defined(CC_ARCH_X86)
#if defined(CC_COMPILER_MSVC)
    return __rdtscp(&core_out);
#else
    return __builtin_ia32_rdtscp(&core_out);
#endif
#else
    core_out = 0;
    return 0;
#endif
}
} // namespace cc::impl
