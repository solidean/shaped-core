#pragma once

#include <clean-simd/kernel.hh>

// The one place clean-simd includes an instruction set's intrinsics header.
// <immintrin.h> declares every x86 level whatever the flags; a level's intrinsics only compile where the TU's flags
// enable it, which is why each kernel's generated header is guarded by its CIMD_HAS_* macro.

#if CIMD_HAS_SSE2
#include <immintrin.h>
#endif

#if CIMD_HAS_NEON
#include <arm_neon.h>
#endif

#if CIMD_HAS_SIMD128
#include <wasm_simd128.h>
#endif
