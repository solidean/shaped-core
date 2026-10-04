# clean-simd design

The decisions behind clean-simd, each with the reason it was taken and what would reopen it.
clean-simd is the successor of ember-core's shaped-simd; where a decision reverses one of shaped-simd's, the entry says so.

## The driving consumer

An 8-wide BVH: every node stores eight child boxes as six planes of eight floats, and a query tests all eight branch-free.
That layout is fixed by the data structure, not by the machine.
So `f32x8` must exist, behave the same and occupy the same bytes on SSE, AVX2, AVX-512, NEON, wasm and plain C++.
And one shipped x64 binary must run on every CPU with AVX2 while using AVX-512 where the CPU has it.

## Every type is templated on a kernel

`cimd::simd<T, N, K>`, with `K` one of `scalar`, `sse2`, `sse42`, `avx2`, `avx512`, `neon`, `simd128`.

**Why not one ISA per build.**
A per-build choice — the compiler's flags pick one tier, every symbol in a tier-named namespace — gives the simplest spelling and was rejected outright.
One binary has to hold several kernels, and only a type parameter lets one algorithm be instantiated for each.

**`cimd::local` is explicit.**
It names the best kernel the TU's flags allow, and there is no default argument: a default makes it too easy to depend on the local kernel from inside code that is meant to stay templated.

**A kernel's code compiles only where the TU's flags allow it.**
On clang and GCC an intrinsic compiles only in a function whose target includes its ISA.
A template instantiation takes its target from where the template is *defined*, not from where it is called.
So each kernel's generated header is guarded by `CIMD_HAS_<KERNEL>`, and naming an unavailable kernel is a `static_assert` in the primary template.
MSVC accepts any intrinsic anywhere, which is how shaped-simd looked portable from Windows: it forced `-mavx2` PUBLIC on every consumer, which defeated its own template parameter.

## Runtime dispatch

`cimd_dispatch(<target> NAME … HEADER … FUNCTION …)` and `CIMD_DISPATCH(name)(args…)`, in `dispatch.hh`.
The algorithm is an ordinary `template <class K>` function in an ordinary header, and nothing else is annotated.
Kernels at or below the build's floor are instantiated in the target itself.
Each kernel above it gets a generated TU compiled with that kernel's `-march`, in a static library linked behind the target.
The table picks, once per process, the best kernel `cc::get_cpu_features()` says the CPU runs; `cimd::force_kernel` pins one per thread, for tests and benchmarks.

**Why not compile everything at the highest level.**
The compiler spends a `-march` flag in every function it emits.
At `x86-64-v4` a plain AABB loop with no intrinsics in it became `vscatterqps` with k-register masks, which faults on an AVX2 CPU before any dispatch is reached.
So the library compiles at the floor, and only a kernel's own code may be compiled above it.

**Why not a lambda or an attributed entry in one ordinary TU.**
With clang 22 under `-mavx2`, four spellings fail the same way:

- an entry function carrying `__attribute__((target("avx512f,…")))` that calls `algo<avx512>`;
- the same entry with `flatten`;
- a generic lambda inside it;
- an explicit instantiation inside a `#pragma clang attribute` target region.

The error is "always_inline function 'add' requires target feature 'avx512f', but would be inlined into function 'algo' that is compiled without support for 'avx512f'".
A template instantiation takes its target from where the template is defined.
Dropping `always_inline` compiles, and turns every op into an out-of-line call with its `__m512` passed through memory.
Only MSVC accepts the one-TU form, because it compiles any intrinsic anywhere.

**Why the kernel TUs live in a static library.**
A TU compiled with a kernel's flags emits every inline function it uses — clean-core's, typed-geometry's — under the same name the floor's copy has, and the linker keeps one program-wide.
Measured with two TUs each emitting `inline int which()` (floor returns 1, kernel 2), linked by clang-cl's objects:

| link line | both callers get |
|---|---|
| lld-link `main floor kernel` | 1 |
| lld-link `main kernel floor` | 2 |
| link.exe `main floor kernel` | 1 |
| link.exe `main kernel floor` | 2 |
| lld-link `main floor k.lib` | 1 |
| link.exe `main k.lib floor` | 1 |

The first copy seen wins, and a library member is seen only when it is pulled, so the floor's copy wins even with the library listed first.
That leaves one hole: an inline function the kernel TU uses that no floor *object* emits but a floor *library* does, where the pull order decides.
`cimd_check_link_map(<executable>)` closes it: the final link writes a map, and any code symbol a kernel object supplied without the kernel's type in its mangled name fails the build.
GNU ld, ELF lld and ThinLTO were not measured; the kernel libraries opt out of interprocedural optimization.

**Alternatives kept on record.**
Brackets around every K-templated definition — a `#pragma` target region per kernel, compiled by a define rather than a flag — are safe without relying on any linker.
They are the fallback if the map check proves noisy.
Building the whole library once per level, as separate shared libraries with a loader, needs no dispatch in the source at all, at the price of shipping the binary twice.

## Native widths below, fixed widths on top

Each kernel has a register layer, `impl::reg<T, K, Bits>`, generated per element and register width: one static function per operation.
The fixed layer, `simd<T, N, K>`, is generated per element and **register count**, not per kernel.
Its code depends only on how many registers the type spans, so `simd<f32, N, K>` with two registers is the same text whether they are SSE, NEON or 128-bit arrays.
It is a partial specialization constrained on `impl::reg_count<T, N, K>`, which keeps `simd` a real class template and keeps `template <class K> f(f32x8<K>)` deducible.

**Flat up to eight registers.**
Every operation is written out once per register, with no loop and no index sequence, so a debug build pays one call per operation and `T::is_loop_free` is a compile-time fact.
Wider types are not generated yet; the primary template says so.

**The scalar kernel's register is a 128-bit array**, so every kernel has a native width of at least 128 bits and fixed types start at 128 bits.
A narrower type (`f32x2`) is an addition for when a caller wants one.

## One layout on every kernel

`sizeof == N * sizeof(T)`, `alignof == min(sizeof, 64)`, lane `i` at byte `i * sizeof(T)`, trivially copyable.
Left to itself, alignment follows the register: `f32x8` would be 32-aligned on AVX2 and 16-aligned on SSE.
A node struct would then change padding between builds — wrong the moment it is written to a file, cached, sent to another process or uploaded to a GPU.

`cimd::storage<T, N>` is the kernel-free form a data structure holds.
A BVH built once and queried by whichever kernel the CPU picked cannot type its nodes on a kernel, so the nodes hold storage and each kernel converts implicitly, with one aligned load or store.

## Masks are opaque

A comparison returns `cimd::mask<LaneBits, N, K>` (`m32x8<K>`), one type per lane width, so a float comparison can select integers.
A mask has `& | ^ ~`, `select`, `bits`, `any`, `all`, `none` and `from_bits` — and no `&` with a value type, no lane access and no layout guarantee.

That is what lets the `avx512` kernel keep it in a k-register.
Measured with clang under `-march=x86-64-v4`, `mask.select(v, 0)` compiles to one zero-masked move, while `v & mask` has to rebuild a vector from the k-register first (`vpmovm2d`).
On AVX2 both are the same `vandps`.
shaped-simd allowed `value & mask` by bit cast, and porting it means rewriting each such site as a select.

`any()` sits beside `bits()` because NEON has no movemask: `bits()` costs about four instructions there and "any lane set" one `vmaxvq`.

## The operator rule

An operator exists where every reference kernel — AVX2, NEON and SIMD128 — implements it as one instruction or at most three lane-wise ones, such as a sign flip and a compare.
Below the reference kernels (SSE2, SSE4.2, scalar) the operator exists anyway and is emulated however it must be.
Every operator has a member twin (`add`, `mul`, `lt`, …), present on every type that supports the semantics at any cost, so generic code has one spelling.

The rule is code, not a list: each implementation in the generator's tables carries a cost class (`single`, `short`, `emulated`), and the operators are derived from it.
So `u32 <` (a sign flip on x86) has an operator, `u8 *` (no x86 level and no SIMD128 has an 8-bit multiply) will not, and `i64 *` (no 64-bit multiply on AVX2 or NEON) will not.
A hand-written list can drift: shaped-simd's cheat sheet promised an `i64 *` its generator never emitted.

**Scalars broadcast from exactly the element type.**
`v * 2.f` works; `v * 2.0` and `f32x8<K>(1)` do not compile, so a `double` never sneaks in as a silent conversion.

## Results that differ by kernel

- **`mul_add`** fuses where `K::has_native_fma` and rounds twice elsewhere, the scalar kernel included; the last bit is kernel-dependent.
- **NaN in `min`/`max`** and **out-of-range float-to-int conversion** are unspecified.
  Making them consistent costs a compare and a select on some kernel for every call, and SIMD hot paths keep NaN out.
- **Reductions** use one order on every kernel — lane `i` with lane `i + N/2`, recursively — so they agree bit for bit.
  It is x86's natural order; NEON's pairwise adds cost one extra step per level to match it.

## Committed codegen

`tools/gen-simd.py` writes `src/clean-simd/generated/`, on the pattern of typed-geometry's `gen-fixed-int.py`: a uv script, run by hand, with `--check` wired into `dev.py check`.
The kernel tables live beside it in `tools/gen_simd/`, one module per instruction-set family.
shaped-simd's generator was a 5.8k-line Rust crate that nothing checked was current; its intrinsic database is what carried over.

## Build floors

`SC_X64_LEVEL` (default `v3`) is what every function may assume, and `SC_WASM_SIMD` (default on) turns on SIMD128.
[docs/platforms.md](../../../../docs/platforms.md#x86-64-feature-level-sc_x64_level) has both.
A floor, never a ceiling: compiled at `v4`, a plain AABB loop with no intrinsics in it became AVX-512 scatters.
Anything above the floor is a dispatched kernel.
