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
```

## Types

```cpp
cimd::simd<T, N, K>                  // N a power of two, N * sizeof(T) >= 16, up to eight registers for now
cimd::f32x8<K>  i32x8<K>  u32x8<K>   // also x4, x16
cimd::m32x8<K>                       // mask over 32-bit lanes: what f32x8, i32x8 AND u32x8 compares return
cimd::f32x8_storage                  // kernel-free bytes for data structures; implicit both ways, an aligned load/store
V::is_loop_free  V::registers  V::generated
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
a.min(b)  a.max(b)  a.abs()          // NaN in min/max is unspecified
a.mul_add(b, c)                      // a*b + c — fused only where K::has_native_fma
a.reduce_add()  reduce_min()  reduce_max()  // lane i with i + N/2, recursively: the same bits on every kernel
f.convert<i32>()  i.convert<f32>()   // truncating; out of range is unspecified
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

## Gotchas

- **No `value & mask`.** `mask.select(value, 0)` — equal code on AVX2, one masked move on AVX-512.
- **`cimd::local` differs between TUs built with different flags.** A function shared between TUs takes `K` as a template parameter.
- **Generated code is committed.** Edit `tools/gen_simd/` and run `uv run libs/base/clean-simd/tools/gen-simd.py --write`; `dev.py check` fails a stale or hand-edited file.
