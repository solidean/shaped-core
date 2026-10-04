# clean-simd — SIMD lane types, namespace `cimd`

Fixed-width SIMD values — `cimd::f32x8<K>`, eight floats — templated on the kernel `K` they are compiled for.
Depends on clean-core alone.

## The one thing to know first

**Every type is templated on a kernel, and production code stays templated on it.**
`template <class K> void query(…)` works with `cimd::f32x8<K>`, and one binary instantiates it for AVX2 and for AVX-512 and picks one at run time.
Code that only ever runs on the machine it was built for writes `cimd::f32x8<cimd::local>`; there is no default argument, so local code says so.

```cpp
#include <clean-simd/all.hh>

template <class K>
cimd::m32x8<K> overlaps(node const& n, box const& q)
{
    using f32x8 = cimd::f32x8<K>;
    return (f32x8(n.x_min) <= f32x8(q.max.x)) & (f32x8(n.x_max) >= f32x8(q.min.x));
}

auto const hit = overlaps<cimd::local>(n, q);
auto const children = hit.select(cimd::i32x8<cimd::local>(n.children), 0); // where the mask is clear, 0
for (auto b = hit.bits(); b != 0; b &= b - 1)
    visit(children.lane(cc::count_trailing_zeroes(b)));
```

## What is fixed, whatever the kernel

- **The layout.** `sizeof == N * sizeof(T)`, `alignof == min(sizeof, 64)`, lane `i` at byte `i * sizeof(T)`.
  `cimd::storage<T, N>` (`f32x8_storage`) is the kernel-free form a data structure holds, and converts to and from every kernel's type with one aligned load or store.
- **The results**, except where a kernel's hardware disagrees and the docs say so: `mul_add` fuses only where `K::has_native_fma`, and out-of-range conversions are unspecified.
  A NaN or a ±0 pair in `min`/`max` is where results differ, and only NEON differs: x86, scalar and SIMD128 return `a`, bit for bit.
  Reductions use one fixed order on every kernel, so they agree bit for bit.
  `rcp_approx` and `rsqrt_approx` are within a relative 2^-11 of exact everywhere, and their last bits differ by kernel.
- **No loops.** Every type up to eight registers is flat generated code; `T::is_loop_free` is the static fact a hot path asserts.
  `shuffle` and `permute` are the exception for now: they go through memory, a store and a reload around a pick per lane.

## What is not here, on purpose

- **No ISA flags.** The target sets none; the build's floor is `SC_X64_LEVEL`, and anything above it is a dispatched kernel.
  [docs/platforms.md](../../../docs/platforms.md#x86-64-feature-level-sc_x64_level) has the floor.
- **No `value & mask`.** A mask is opaque, so AVX-512 can keep it in a k-register; combining it with values is `mask.select(a, b)`.
- **No operator that hides a cost.** `+ - * << ==` exist only where AVX2, NEON and SIMD128 each do it in at most three lane-wise instructions.
  Everything has a named member twin (`a.mul(b)`) that works on every type.

## Elsewhere

- [cheat-sheet.md](cheat-sheet.md) — the API at a glance.
- [docs/_index.md](docs/_index.md) — the design, the kernels and the generator.
