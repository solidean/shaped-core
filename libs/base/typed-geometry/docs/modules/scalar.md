# Module: scalar

> Module docs answer **"what belongs here?"** and **"why is it this way?"** — [docs/_index.md](../_index.md#module-docs) states the contract they follow.

## What this module is

`scalar/` is the bottom of typed-geometry, and owns everything about the **element type `T`** the geometric types are generic over.
That is the capability seam (`scalar_traits`), the scalar free functions built on it (`one`, `sqrt`, `sin`, `cos`, `sin_cos`, `atan2`), and constants such as `pi`.
Scalar-like *newtypes* that are still one number in spirit belong here too — `angle` today.
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

`f32`/`f64` are fully featured: sqrt plus trigonometry.
**Every integer type** gets `one`/`is_zero`/`is_one` from a single constrained specialization, including `signed char` and `unsigned char`, which are genuine small integers.
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

## See also

- [scalar/traits.hh](../../src/typed-geometry/scalar/traits.hh) — the seam and per-scalar specializations.
- [coding-guidelines](../coding-guidelines.md) — the "route scalars through traits", "don't require
  more of `T` than needed", and "qualify every call" rules this module embodies.
- [cheat-sheet](../../cheat-sheet.md) — the scalar API at a glance.
