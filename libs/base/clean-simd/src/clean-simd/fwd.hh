#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/record/domain_fwd.hh>

// The API index: every clean-simd type, declared once, with the header that defines it.

namespace cimd
{
using namespace cc::primitive_defines;

// kernels (kernel.hh) — one tag per instruction set
enum class kernel_id : u8;
struct scalar;  // plain C++, the reference
struct sse2;    // x86-64-v1
struct sse42;   // x86-64-v2
struct avx2;    // x86-64-v3
struct avx512;  // x86-64-v4, k-register masks
struct neon;    // arm64 AdvSIMD
struct simd128; // WebAssembly SIMD128

// values (simd.hh, generated/fixed/) — N lanes of T, compiled for kernel K
template <class T, int N, class K>
struct simd;
template <int LaneBits, int N, class K>
struct mask;
template <class T, int N>
struct storage; // kernel-free bytes for data structures

// dispatch (dispatch.hh) — one algorithm, compiled per kernel, picked per CPU
struct scoped_forced_kernel; // pins this thread's dispatch to one kernel, for tests and benchmarks

/// The domain every recording site in clean-simd is attributed to.
CC_REC_DECLARE_DOMAIN(g_rec_domain);

namespace impl
{
template <class T, class K, int Bits>
struct reg; // the register layer, generated per kernel
template <int LaneBits, class K, int Bits>
struct mreg;
template <class T, int L>
struct scalar_reg;
template <int L>
struct scalar_mreg;
} // namespace impl
} // namespace cimd
