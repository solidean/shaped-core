# Module: scalar

> Module docs answer **"what belongs here?"** and **"why is it this way?"** — [docs/_index.md](../_index.md#module-docs) states the contract they follow.

## What this module is

`scalar/` is the bottom of typed-geometry, and owns everything about the **element type `T`** the geometric types are generic over.
That is the capability seam (`scalar_traits`), the scalar free functions built on it (`one`, `sqrt`, `abs`, `sin`, `cos`, `sin_cos`, `atan2`, `pow`, `log`, `round`), and constants such as `pi`.
Scalar-like *newtypes* that are still one number in spirit belong here too — `angle`, `fixed_int` and `half_float` today.
It must not depend on `linalg` or anything above it — geometric types are instantiated over scalar types, never the other way around.

## What belongs here

- Scalar **capability declarations** and the operations that dispatch through them.
- Scalar **constants** (`pi`, later `e`, `tau`, epsilons, …).
- Scalar-like **newtypes** that are still "one number" in spirit: `angle` today; `complex`,
  `interval`, forward/reverse-mode autodiff scalars, and (likely) `bigint`/`bigrat` later.
- Anything a downstream module needs to know about `T` *without* knowing about vectors.

## What does NOT belong here

- Vectors, matrices, geometry — those are `linalg` and above.
- Algorithms that are about *shapes*, not *numbers*.

## Key decisions

### Every scalar capability goes through `scalar_traits<T>`

tg is generic over the scalar type, and we want it to work for far more than the built-in floats:
expression trees (`vec<3, expr>`), double-double, intervals, autodiff duals, `bigint`/`bigrat`.
`std::is_floating_point_v` and `<cmath>` cannot describe those, so tg routes **every** scalar
capability through `tg::scalar_traits<T>` — a primary template specialized per scalar type — and
exposes thin wrappers (`tg::sqrt`, the `tg::traits::has_*` flags, …). Adding a scalar means
specializing one trait; no geometric code changes.

`<cmath>` is allowed in this module, but only inside the `scalar_traits` specializations, and it never leaks into the geometric types.
The current `std::sqrt` routing is a placeholder we intend to drop, since it honors `errno` and costs codegen for nothing — [TODO.md](../TODO.md) tracks it.

### `one()` is a function, and `is_zero`/`is_one` are trait operations

`one<T>()` is a call into the trait rather than a bare `T(1)` literal because **not every scalar is constructible from an `int`**.
`tg::traits::is_zero` / `is_one` route through the trait for the same kind of reason.
A symbolic or exact scalar can answer meaningfully where `== 0` / `== 1` would be wrong or ambiguous.
Geometric code asking "is this the additive or multiplicative identity?" must go through the trait, never compare literally.

### Which types count as scalars

`f32`/`f64` are fully featured: sqrt, abs, trigonometry, exponentials and rounding.
**Every integer type** gets `one`/`is_zero`/`is_one` plus `abs` from a single constrained specialization, including `signed char` and `unsigned char`, which are genuine small integers.
Integers claim no rounding: `round`/`floor`/`ceil` on one is the identity, so asking for it means the caller meant a float.
**Plain `char` is deliberately excluded** — it models text, not a number.
It falls through to the capability-less primary template, so arithmetic-geometry over `char` is a hard compile error.
`bool` is a scalar, but a degenerate one, and has its own specialization (`one() == true`, `is_zero(b) == !b`).

### `angle` is a unit-checked newtype, not a wrapped float

Mixing radians and degrees is a classic, costly bug.
`angle<T>` stores radians, is constructible *only* via `make_from_radians` / `make_from_degree`, and is read back *only* via `.radians()` / `.degree()`.
There is no implicit conversion to or from a bare scalar.
It supports addition and scalar multiplication — it is a 1D vector space — but **deliberately does not wrap around** at 2π.
It is a unit-checked *number*, not a modular angle in `[0, 2π)`; normalizing into a range, if that is ever needed, will be an explicit operation.
The `_rad_f`/`_deg_f`/… literals exist so the safe path is also the short one (`90_deg_f`).

The unit-checking extends to trigonometry.
The forward functions (`sin`/`cos`/`tan`/`sin_cos`, and the reciprocals `sec`/`csc`/`cot`) take a `tg::angle` rather than a bare radian scalar.
The inverse ones (`asin`/`acos`/`atan`/`atan2`) *return* one.
Each forward function exists as a member (`a.sin()`) and as a free function (`tg::sin(a)`), the free form delegating to the member so there is one implementation.
The raw `scalar_traits` kernels still work in plain radian `T`: the angle typing is added only at the public layer, so a new scalar type never has to know about `angle`.

### Capabilities are grouped, and rounding returns the scalar

The `has_*` flags name families rather than single functions, so a new scalar opts into a coherent set at a time.
Today: `has_sqrt`, `has_abs`, `has_trigonometry`, `has_exponential` (`pow`/`exp`/`log`), `has_rounding` (`round`/`floor`/`ceil`)
and `has_pow2` (`pow2_by_int`/`scale_by_pow2`/`exponent_of`/`split_pow2`).
A scalar that can do one member of a family can essentially always do the rest, and a per-function flag would multiply the seam without buying precision.

`round`/`floor`/`ceil` return the **scalar** type, not an integer.
An integer result forces a width choice tg has no basis to make, and the `f32` → `i32` narrowing is exactly the step a caller should be writing out (`int(tg::round(x))`).

`pow` takes base and exponent as the *same* `T`.
A mixed-type overload would silently promote, which is how a `f32` pipeline quietly becomes `f64` — the cast belongs at the call site.

### Base two is exact, so it is its own family

`has_pow2` sits apart from `has_exponential` because the two answer different questions.
`pow`/`exp`/`log` are approximate transcendentals over a real exponent.
`pow2_by_int`/`scale_by_pow2`/`exponent_of`/`split_pow2` are **exact** operations on the binary exponent — an exponent-field adjustment, never a multiply.
`tg::pow(2.0f, e)` is not a substitute for `tg::pow2_by_int<f32>(e)`, and a symbolic or fixed-point scalar can easily have one family without the other.
Integers stay out: shifting one truncates, which is a different operation wearing the same name.

The names are deliberately not C's `frexp` and `ldexp` — those abbreviate nothing a reader can recover — and one of the semantics is deliberately different with them.
**`split_pow2`'s significand is in `[1, 2)`, not `frexp`'s `[0.5, 1)`.**
That is what the IEEE-754 exponent field already means, so `exponent_of(x)` is `floor(log2(|x|))` and `split_pow2(8.0f)` reads as `{1, 3}` rather than `{0.5, 4}`.
Porting `frexp`-shaped code means adjusting the exponent by one, and the different name is precisely what makes that adjustment visible rather than a silent factor of two.

Zero and the non-finites are a **precondition**, not a fallback: `frexp` quietly reports exponent 0 for all three, which turns a bug into plausible-looking data.
`split_pow2` and `exponent_of` assert instead, and every real caller has already branched on zero for its own reasons.
Subnormals are normalized rather than reported with a zero exponent — which is the trap a naive read of the exponent field falls into.

### `fixed_int` is modular arithmetic plus explicit widths

`tg::fixed_int<Bits>` / `tg::fixed_uint<Bits>` (spelled `fi32` … `fi256`, `fu32` … `fu256`) exist for exact geometry predicates.
A predicate's intermediates have bounds known in advance, so what it needs is not a bigint.
It needs integers of a chosen width, and arithmetic whose result width the caller picks: `tg::mul<fi192>(a, b)` for two `fi128` below `2^80` and `2^90`.

- **Every operator wraps modulo `2^Bits`, and takes one type on both sides.**
  No width changes silently: the constructor only widens, `x.truncated_to<T>()` narrows, and mixing widths is a compile error.
  That is what makes `fi192 r = a * b` over `fi128` fail to compile, since it would wrap at 128 bits before widening.
- **`tg::add` / `sub` / `mul<R>` compute the exact result into `R`, and `R` is a claim.**
  Unchecked it wraps; `SC_CHECK_WIDE_ARITH` checks it, and `tg::checked_*` always does.
  The checks are not `CC_ASSERT`s because these calls sit in the hottest predicate loops, where the default dev preset must not pay for them.
- **An operation on one value is a member, one combining two values is free.**
  `x.truncated_to<T>()`, `x.shifted_left<T>(n)`, `x.to_f64()` against `tg::mul<R>(a, b)` and `tg::div_floor(a, b)`.
- **Up to 256 bits the arithmetic is generated, loop-free and branch-free.**
  `tools/gen-fixed-int.py` picks the `(R, A, B)` triples by the bounds the operands' widths imply and writes one header per result width into `fixed_int/generated/`; its docstring has the rule.
  Everything else runs the generic bodies in `fixed_int/impl/core.hh`, which are a second, independent formulation the tests hold the generated ones against.
  Regenerate with `uv run libs/base/typed-geometry/tools/gen-fixed-int.py --write`; `dev.py check` fails when the committed output drifts.
- **Division truncates, like the builtins, and floor and ceiling are named.**
  `tg::div_floor<Q>(x, w)` is the case predicates need: a quotient known to fit `Q` (32 bits) comes from one estimate plus one exact remainder rather than a long division.
  The estimate is a `cc::udiv128` of the top words, measured ~20% faster than an f64 estimate and ~2.7× faster than Knuth D on x64.
  MSVC ARM64 has no 128 ÷ 64 instruction, so there it is a software division, and still correct.
- **Float conversions are correctly rounded, with no fast variant.**
  `to_f64` / `to_f32` round to nearest with ties to even, via a sticky bit over everything below the top 64 bits.
  That costs ~20% against dropping the sticky bit, measured on fi256, which is too little to be worth a second, subtly different function.
- **`x.sign()` is the predicate's answer**: -1, 0 or +1 from the OR of the limbs and the sign bit, without a branch or a comparison.
- **A `fixed_int` is a scalar**, so `vec<3, fi64>` exists — and its operations wrap at the element width, so a dot product over `fi64` is computed in `fi64`.
  Width-aware `dot` and `cross` belong to a predicate layer on top of this one.

### `half_float` is binary16, with a GPU's arithmetic

`tg::half_float`, spelled `tg::f16`, is IEEE 754 binary16: a struct over its 16 bits, the same type on every compiler.
The compilers' `_Float16` is not used as the type, because it does not exist under MSVC and brings C's implicit conversions and excess precision where it does.
The hardware is still used, inside the conversions.

- **Conversions are explicit both ways**, as every tg constructor is, so `h + 1.0f` does not compile.
  Widening is exact.
  Narrowing rounds to nearest with ties to even, overflows to infinity, and turns a NaN into a quiet NaN keeping its sign and the top of its payload.
  An f64 and an integer narrow directly; through f32 they could round twice.
- **The conversion is a portable bit-level kernel, with hardware only where the build target already guarantees it.**
  That is F16C where `__F16C__` is defined (cl.exe: `/arch:AVX2`), and AArch64's native conversion.
  A run-time CPU check cannot help a single conversion: an F16C body cannot be inlined into a caller compiled for baseline x64.
  Constant evaluation always takes the portable kernel, so every conversion is `constexpr`.
  The F16C path was checked bit-identical to the portable kernel over all 2^32 narrowings and all 65,536 widenings.
- **Arithmetic computes in f32 and rounds once, which is exactly binary16's own arithmetic for `+ - * /` and `sqrt`.**
  f32 carries 24 bits, and double rounding is harmless from 2p+2 = 24; the tests hold it against an f64 reference.
  So CPU code over f16 computes what a GPU computing in half does, and every operation in a chain rounds.
  For heavy math the pattern is to widen once, compute in f32 and narrow at the end.
  An ARM64 target with native half arithmetic uses it, since the results are the same bits.
  Fused multiply-add does not share the property: via f32 it is not correctly rounded, via f64 it is.
- **An operation works on the bits when it is a pure bit operation, or when baseline x64 would otherwise pay a library call.**
  Negation, `abs`, classification, comparison, `floor`/`ceil`/`round` and the base-two family do; everything that rounds goes through f32.
  Baseline x64 has no rounding instruction and converts in software, and there the bits win 1.5× on a comparison, 2.2× on `floor` and 2.5× on `scale_by_pow2`.
  `tests/benchmarks/half_float-benchmark.cc` measures both formulations.
- **f16 claims every capability family**, the transcendental ones computing in f32's libm with one final rounding — the same kind of error f32's own libm has.
- **`{}` prints the shortest digits that read back as the same f16**, so `tg::f16(0.1f)` prints `0.1`, and the largest value `65500`.
  A precision or a presentation type prints the exact value instead.
- **No span conversion and no vector aliases yet.** Both are niche until a caller needs them; `tg::vec<3, tg::f16>` works as it is.

## See also

- [scalar/traits.hh](../../src/typed-geometry/scalar/traits.hh) — the seam and per-scalar specializations.
- [coding-guidelines](../coding-guidelines.md) — the "route scalars through traits", "don't require
  more of `T` than needed", and "qualify every call" rules this module embodies.
- [cheat-sheet](../../cheat-sheet.md) — the scalar API at a glance.
