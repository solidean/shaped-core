# typed-geometry coding guidelines

The repo-wide [coding-guidelines](../../../../docs/coding-guidelines.md) come first and still apply in full.
This file holds only the **tg-specific** rules, and the places where generic advice is wrong for tg for a non-obvious reason.

**Extend it as we go.** The signal is catching yourself making a style mistake by following generic advice that does not fit tg.

---

## Scalars go through `scalar_traits`, never `std::` directly

No `std::sqrt` or `std::is_floating_point_v` in tg's type or operation API.
Every scalar capability routes through `tg::scalar_traits<T>` ([scalar/traits.hh](../src/typed-geometry/scalar/traits.hh)).
Call it through the thin wrappers over it: `tg::traits::has_sqrt<T>`, the free `tg::sqrt()`.
[modules/scalar.md](modules/scalar.md) has the why: the trait is the one seam an exotic scalar specializes to opt in, and `std::` traits cannot be extended that way.

- Add a scalar operation by extending `scalar_traits<T>` with a capability flag plus the operation, then a thin `tg::` free function and `tg::traits::` alias over it.
  Mirror the existing `has_sqrt` / `tg::sqrt` pair.
- **Capability-gate** with `requires(tg::traits::has_*<T>)`, never by hard-coding floating-point.
  `length()` / `normalized()` / `distance()` require `has_sqrt<T>`; `length_sqr()` / `distance_sqr()` work for every scalar.
- `<cmath>` is permitted inside the `scalar_traits` specializations and nowhere else.
  clean-core forbids it outright; tg does not, because the specializations are where the platform math has to enter.

## Qualify every call: `tg::` or `this->`, never unqualified

Inside tg, **no call is unqualified.**
Free functions are called as `tg::foo(...)`, member functions as `this->foo(...)` — even from inside the same class.
Member *data* access is exempt — `data[0]` stays bare — because the rule is about calls.
A hidden friend has neither `tg::` nor `this->` available for the enclosing class's own statics, so a bare `make_from_radians(...)` there is the one place the rule does not reach.

```cpp
return tg::sqrt(this->length_sqr());   // free fn -> tg::, member fn -> this->
auto const l = this->length();         // not: length()
```

**Why:** tg instantiates a large free-function surface over user-provided scalar types that drag in their own namespaces.
An unqualified call invites ADL to pick up an overload from the scalar's namespace, and an unqualified member call can be ambiguous with a free function of the same name.
Explicit `tg::` / `this->` makes name resolution deterministic.
This is the repo-wide "prefix `cc::` even inside the library" rule extended to tg's own members.

## Don't require more of `T` than the operation needs

Avoid gratuitously constraining the scalar type.
The common offender is seeding a reduction with `T{}`, which requires default-constructibility, where the first element would do just as well.
Dimensions are always `D > 0` (`static_assert`ed), so `data[0]` exists:

```cpp
// avoid: requires T to be default-constructible
T s{};
for (int i = 0; i < D; ++i) s += data[i] * data[i];

// prefer: seeds from the first product, no T{} requirement
T s = data[0] * data[0];
for (int i = 1; i < D; ++i) s += data[i] * data[i];
```

**Why:** `T{}` may be meaningless, expensive or unsupported for an expression tree or an interval.
When a straightforward formulation avoids a requirement, take it.

## Storage: raw `T data[D]`, indexed access only

The linalg types store components in a single public C array member `data` (`T data[D]`; `mat` stores `vec<R> cols[C]`).
Access is through `data` or `operator[]` — there are deliberately **no `.x/.y/.z/.w`** members, not even as accessor functions.

**Why:** one generic implementation over `D`, where every operation is a loop, and uniform handling of every scalar type.
Named accessors would push toward the per-dimension specializations tg is avoiding.
Sugar can be reconsidered once the minimal form has been pushed as far as it goes.

The member is named `data` rather than `comp` because a member named `comp` collides with the injected-class-name of the `comp` type.
That makes `comp`'s own constructors and return types ill-formed.

## One generic type per family; dimensions via `requires`

`vec` / `pos` / `comp` are each a single `template <int D, class T>` — **no per-dimension specializations**.
Typedefs exist for D = 2/3/4, but the type stays generic.
Dimension-specific behavior, such as the 3-argument constructor, is selected with a `requires` clause rather than a specialization.

## Factory methods are `make_*`; special values are static constants

Every factory — a static method returning an instance — is named `make_*`.
`vec::make_from_values`, `vec::make_unit`, `mat::make_from_cols`, `mat::make_rotation_z`, `angle::make_from_degree`.
**Why:** C++ lets you call a static through an instance (`v.make_unit(0)`), so a bare verb like `unit()` or `from_values()` reads like a member operating on `v`.
The `make_` prefix makes "this constructs a fresh value" unambiguous at the call site, and groups the factories under autocomplete.
It is clean-core's `create_` rule under a different verb.

Distinguished constant values are **static data members**, not factories.
`vec::zero`, `pos::zero`, `comp::zero`, `bivec::zero`, `mat::zero`, `mat::identity`, `quat::zero`, `quat::identity`.
Provide them for a new type wherever a canonical value exists.

**At call sites, prefer the literal over the factory** where tg offers one.
`angle` has `_rad_f`/`_rad_d`/`_deg_f`/`_deg_d`, and they exist precisely so the safe path is also the short one:

```cpp
auto const fov = 60_deg_f;                            // prefer
auto const fov = tg::angle_f::make_from_degree(60);   // avoid — noise at every call site
```

Pull the literals in once, not per file.
A library with its own namespace does it in its `fwd.hh`, the same way it adopts `cc::primitive_defines`:

```cpp
namespace my_lib { using namespace tg::literals; }    // in my_lib/fwd.hh — 60_deg_f works library-wide
```

`using namespace tg::literals;` at file scope is for code that isn't inside a namespace — a test, an
example, a `main.cc`.

`make_from_degree` stays right when the value is a runtime expression that reads worse as `x * 1_deg_f`.
`make_*` is the *definition-side* naming rule — not an instruction to spell out the factory where sugar exists.

**Implementation note:** a static data member cannot be `constexpr` of its own, still-incomplete class type.
So they are declared `static T const zero;` in the class and defined `inline` out of line in the same header (`template <...> inline T<...> const T<...>::zero = ...;`).
They are therefore runtime constants, usable everywhere except constant expressions.
Build `identity` from `make_unit` via the `tg::impl::make_identity` helper rather than hand-writing the diagonal.

## Multi-argument `operator[]` and preprocessor macros

`mat` uses the C++23 multi-argument subscript `m[col, row]`.
Inside a function-like macro (`CHECK`, `CC_ASSERT`, …) the preprocessor splits on that comma and sees two arguments.
Wrap the subscript in parentheses at any macro call site:

```cpp
CHECK((m[0, 0]) == 1);          // not CHECK(m[0, 0] == 1) — "too many macro arguments"
CHECK_ASSERTS((m[3, 0]));
```

## Constructors

- All constructors are `explicit` (per the global rule), including the scalar splat `explicit vec(T)`.
- The per-dimension constructors are **fixed-arity** and `requires`-gated (`vec(T,T) requires(D == 2)`, …), never variadic.
  A variadic constructor would over-match and silently swallow unrelated call sites.
- The variadic entry point is the named static `make_from_values(...) requires(sizeof...(Ts) == D)`, kept off the constructor set for the same reason.
- Default construction **zero-initializes** (`T data[D] = {}`).
  The types stay trivially copyable regardless — only the default constructor is non-trivial — and a `static_assert(std::is_trivially_copyable_v<...>)` in tests pins that.

## Don't assert on common degenerate inputs; return a sensible value

The repo-wide guidance is to `CC_ASSERT` preconditions liberally.
`vec::normalized()` and `quat::normalized()` return `zero` for a degenerate length rather than asserting.
**Why:** zero-length normalization shows up constantly — accumulated directions, user data, edge cases in loops.
Asserting there produces far more spurious failures than caught bugs.
The degeneracy test is `tg::traits::is_zero(...)`, never a literal `== 0`, so each scalar type decides what "zero" means.
For `f32`/`f64` that is an exact comparison; an exotic scalar may make it a tolerance.
Use `tg::traits::is_zero(...)` for the degeneracy test, never `== 0`, so exotic scalars decide what "zero" means.

This is a judgement call per operation, not a blanket waiver.
Keep asserting on genuine programmer errors: an out-of-range `operator[]`, a wrong-size initializer list.

## Semantic typing

The point of tg is that the type system encodes geometry, so keep the distinctions meaningful.
`vec` is a free vector, `pos` is a point, `comp` is the semantics-free component bag.
The affine algebra follows from that: `pos - pos -> vec`, `pos + vec -> pos`, `vec + vec -> vec`, and the deliberate `pos + pos -> pos`.
Do not add an operator that blurs them — `pos * scalar` is not meaningful, and `comp` must never grow geometric meaning.
[modules/linalg.md](modules/linalg.md) derives all of it, including why `pos + pos` translates and why `bivec` is not a `vec`.

## Members vs free functions (tg flavor)

Same spirit as the global rule: intrinsic, local, discoverable operations are members (`v.length()`, `v.normalized()`, `ray.at(t)`).
Symmetric, cross-type, heavy or extensible operations are free functions (`dot(a, b)`, `distance(a, b)`, `intersection(a, b)`).
Free functions for a type live in `<type>_ops.hh`, separate from the type header.
