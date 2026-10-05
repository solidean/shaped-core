# clean-simd cheat sheet

Namespace `cimd`, include `<clean-simd/all.hh>`.
Every value type takes a kernel `K`; [readme.md](readme.md) says why.

## Kernels

```cpp
cimd::scalar  cimd::sse2  cimd::sse42  cimd::avx2  cimd::avx512  cimd::neon  cimd::simd128
cimd::local                          // the best kernel THIS TU's flags allow; for code that runs where it was built
cimd::is_available<K>                // whether this TU can compile K's code; using an unavailable K is a static_assert
K::native_bits; K::has_native_fma;   // 256 and true on avx2; scalar's "register" is a 128-bit array
cimd::native_lanes<K, f32>           // 8 on avx2, 4 on sse/neon/simd128/scalar
cimd::native<K, f32>                 // simd<f32, native_lanes, K> — on avx2 the SAME type as f32x8<K>
cimd::kernel_name(K::id)             // "avx2"
cimd::kernel<K>                      // concept: K has ::id and ::native_bits; nothing constrains on it yet
```

## Types

```cpp
cimd::simd<T, N, K>                  // T: f32 f64 i8 i16 i32 i64 u8 u16 u32 u64; N a power of two, 128 bits to 64 KiB
cimd::f32x8<K>  f64x4<K>  i8x16<K>   // every element at 128, 256 and 512 bits: f32x4/x8/x16, u8x16/x32/x64, …
cimd::m32x8<K>                       // mask over 32-bit lanes: what f32x8, i32x8 AND u32x8 compares return; m8/m16/m64 too
cimd::f32x8_storage                  // kernel-free bytes for data structures; implicit both ways, an aligned load/store
V::is_loop_free  V::registers  V::generated   // flat up to 8 registers; above, every operation loops (is_loop_free false)
```

## Values

```cpp
auto v = cimd::f32x8<K>(1.f);        // broadcast — ONLY from exactly the element type: f32x8<K>(1) and v * 2.0 do not compile
V::zero();  V::iota();               // 0, 1, …, N-1
V::from_lanes(a, b, c, d, …);        // exactly N element-typed arguments
V::load(p);  V::load_aligned(p);  v.store(p);  v.store_aligned(p);
v.lane(i);  v.with_lane(i, x);       // through storage; not for hot loops
a + b  a - b  a * b  -a              // operators exist only where AVX2, NEON and SIMD128 are each <= 3 instructions
a.add(b)  a.sub(b)  a.mul(b)  a.neg()// the member twin of every operator
a & b  a | b  a ^ b  ~a              // integer types only; floats go through cc::bit_cast
a << n  a >> n                       // one int count for every lane, 0 <= n < bits (outside it results differ by kernel,
                                     // and the CC_ASSERT fires only with asserts on); >> is arithmetic on signed types
a / b  a.sqrt()  a.floor()  ceil()  round()  trunc()  a.copysign(b)   // floats; round is to nearest, ties to even
a.min(b)  a.max(b)  a.abs()          // a NaN or ±0 pair gives `a` on x86, scalar and simd128; NEON alone differs
a.mul_add(b, c)                      // a*b + c — fused only where K::has_native_fma
a.reduce_add()  reduce_min()  reduce_max()  // lane i with i + N/2, recursively: the same bits on every kernel
f.convert<i32>()  i.convert<f32>()   // f32<->i32, u32->f32, f64<->i64, u64->f64; truncating, out of range unspecified
f.convert_saturating<i32>()          // out of range clamps, NaN gives 0
a.rcp_approx()  a.rsqrt_approx()     // floats; relative error <= 2^-11 on every kernel, unspecified at 0, inf and below 0
a.reverse()                          // lane i takes lane N - 1 - i
a.shuffle<1, 0, 3, 2>()              // lane i takes lane I_i; exactly N indices in [0, N)
a.permute(idx)  V::gather(base, idx) // idx is simd<iB, N, K> for B the element's bits; permute reads it unsigned, modulo N
// No operator where AVX2, NEON or SIMD128 needs more than three instructions — the member twin is there instead:
// u8/i8 a.mul(b) (no 8-bit multiply on x86 or wasm), i64/u64 a.mul(b) (none below AVX-512), i8 a.shr(n)
```

## Masks

```cpp
a < b  a <= b  a == b  a != b …      // -> mask_t; float compares are ordered (false on NaN), != is true on NaN
m & n  m | n  m ^ n  ~m
m.select(a, b)                       // a where set, b where clear; b may be a scalar of exactly the element type
m.bits()                             // u32 (u64 beyond 32 lanes); not for more than 64 lanes
m.any()  m.all()  m.none()           // cheaper than bits() != 0 where the kernel has no movemask (neon)
M::from_bits(b)
```

## Dispatch

```cpp
// header: an ordinary template whose signature does not depend on K
template <class K> int query(bvh8 const& b, box q, i32* out, int cap);
CIMD_DISPATCH_DECLARE(bvh8_query, query);     // at GLOBAL scope — the generated definition lives there
CIMD_DISPATCH(bvh8_query)(b, q, out, cap);    // the best kernel this CPU runs, resolved once and logged at info ("cimd")
CIMD_DISPATCH_KERNEL(bvh8_query);             // -> kernel_id the next CIMD_DISPATCH on this thread runs, forced or not
cimd::cpu_supports(cimd::kernel_id::avx512);  // x86-64-v4 and the OS saving zmm
cimd::scoped_forced_kernel const f(cimd::kernel_id::avx2); // this thread only; nests; ignored (debug log) where the table or CPU lacks it
```

```cmake
cimd_dispatch(my-target NAME bvh8_query HEADER bvh8-query.hh FUNCTION query LINK typed-geometry)  # any directory; NAME unique
cimd_check_link_map(my-executable)            # on EACH final executable: fails the build if a kernel TU's copy of shared code won
```

- Kernels above `SC_X64_LEVEL` (`SC_SIMD_KERNELS`, default every one) are compiled in their own TUs, `<name>-<kernel>.cc`, with their own `-march`.
- A dispatched header must not define a non-template inline function that only kernel code reaches.
  Floor code always has its own copy of a helper it reaches, and that copy wins; one reached only from kernel code is harmless at run time, but the link-map check fails it.
- On COFF the floor's copy of shared inline code always wins; on ELF it usually does, and the link-map check is what makes sure.

## Gotchas

- **No `value & mask`.** `mask.select(value, 0)` — equal code on AVX2, one masked move on AVX-512.
- **`cimd::local` differs between TUs built with different flags.** A function shared between TUs takes `K` as a template parameter.
- **Generated code is committed.** Edit `tools/gen_simd/` and run `uv run libs/base/clean-simd/tools/gen-simd.py --write`; `dev.py check` fails a stale or hand-edited file.
