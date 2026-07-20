# typed-geometry coding guidelines

These build on the repo-wide [coding-guidelines](../../../../docs/coding-guidelines.md) — read
that first; everything there still applies. This document only captures the **tg-specific**
rules and the places where generic advice does *not* apply to tg for non-obvious reasons.

It is intentionally thin for now. **Extend it as we go:** whenever you catch yourself making a
"style mistake" by following generic advice that turns out to be wrong for tg (for a reason that
isn't obvious from the code), that's the signal to add the rule here.

---

## Scalars go through `scalar_traits`, never `std::` directly

tg does **not** call `std::sqrt`, `std::is_floating_point_v`, etc. in its type/operation API.
Every scalar capability is routed through `tg::scalar_traits<T>` (see
[scalar/traits.hh](../src/typed-geometry/scalar/traits.hh)) and the thin wrappers over it
(`tg::traits::has_sqrt<T>`, the free `tg::sqrt()`).

**Why** (not obvious): the scalar type is a template parameter, and we want tg to work over more
than the built-in floats — expression trees (`vec<3, expr>`), double-double, `bigint`/`bigrat`,
intervals, autodiff scalars, … `std::` traits and `<cmath>` are not extensible to those. The
trait is the single seam those types specialize to opt in.

Concretely:

- Add a scalar operation by extending `scalar_traits<T>` (a capability flag + the operation),
  then a thin `tg::`-level free function / `tg::traits::` alias over it. Mirror the existing
  `has_sqrt` / `tg::sqrt` pair.
- **Capability-gate** members and free functions with `requires(tg::traits::has_*<T>)` rather
  than hard-coding floating-point. E.g. `length()` / `normalized()` / `distance()` require
  `has_sqrt<T>`; `length_sqr()` / `distance_sqr()` work for every scalar.
- `<cmath>` **is** permitted in tg (unlike clean-core, which forbids it) — but only inside the
  `scalar_traits` specializations, never leaking into the geometric types. Today's `std::sqrt`
  routing is a placeholder; see [TODO.md](TODO.md).

## Qualify every call: `tg::` or `this->`, never unqualified

Inside tg, **no call is unqualified.** Free functions are called as `tg::foo(...)`; member
functions are called as `this->foo(...)`, even from inside the same class. Member *data* access is
exempt — `data[0]` stays bare (it needs no `this->`); this rule is about *calls*.

```cpp
return tg::sqrt(this->length_sqr());   // free fn -> tg::, member fn -> this->
auto const l = this->length();         // not: length()
```

**Why** (not obvious): tg has a large surface of free functions and operators, often
instantiated over user-provided scalar types that drag in their own namespaces. An unqualified
call is an open invitation for ADL to pick up an unintended overload from a scalar's namespace,
and an unqualified member call can be ambiguous with a free function of the same name. Explicit
`tg::` / `this->` makes name resolution deterministic. This extends the repo-wide rules
("explicitly prefix `cc::` even inside the library to prevent unintended ADL capture",
"non-trivial ADL usage must always be explicitly marked") to tg's own members.

## Don't require more of `T` than the operation needs

Avoid gratuitously constraining the scalar type. A common offender is seeding a reduction with
`T{}` (requires default-constructibility) when you could seed from the first element instead —
dimensions are always `D > 0` (`static_assert`ed), so `data[0]` exists:

```cpp
// avoid: requires T to be default-constructible
T s{};
for (int i = 0; i < D; ++i) s += data[i] * data[i];

// prefer: seeds from the first product, no T{} requirement
T s = data[0] * data[0];
for (int i = 1; i < D; ++i) s += data[i] * data[i];
```

**Why** (not obvious): tg is meant to instantiate over exotic scalars (expression trees,
intervals, …) where `T{}` may be meaningless, expensive, or simply unsupported. When a
straightforward formulation avoids a requirement, take it. (Same spirit as the `scalar_traits`
rule: don't bake in assumptions the built-in floats happen to satisfy.)

## Storage: raw `T data[D]`, indexed access only

The linalg types store components in a single public C array member named `data`
(`T data[D]`). Access is via `data` or `operator[]` — there are deliberately **no `.x/.y/.z/.w`**
members, not even as accessor functions.

**Why** (not obvious): we want one generic implementation over `D` (operations are loops over
`data`), uniform handling of all scalar types, and to see how far the minimal representation
carries us before adding sugar. Named accessors would push us toward per-dimension
specializations, which we are explicitly avoiding.

The member is named `data` (not `comp`) because a member named `comp` collides with the
injected-class-name of the `comp` type and makes constructors/return types inside it ill-formed.

## One generic type per family; dimensions via `requires`

`vec` / `pos` / `comp` are each a single `template <int D, class T>` — **no per-dimension
specializations**. Typedefs exist for D = 2/3/4, but the type stays generic. Dimension-specific
behavior (e.g. the 3-arg constructor, or a future `cross` for D == 3) is selected with a
`requires` clause, not a specialization.

## Factory methods are `make_*`; special values are static constants

Every factory (static method that returns an instance) is named `make_*`:
`vec::make_from_values`, `vec::make_unit`, `mat::make_from_cols`, `mat::make_rotation_z`,
`angle::make_from_degree`, … **Why** (not obvious): C++ lets you call a static through an instance
(`v.make_unit(0)`), so a bare verb like `unit()` or `from_values()` reads like a member operating
on `v`. The `make_` prefix makes "this constructs a fresh value" unambiguous at the call site and
groups factories under autocomplete. (This mirrors clean-core's `create_` rule; tg uses `make_`.)

Distinguished constant values are **static data members**, not factories: `vec::zero`,
`pos::zero`, `comp::zero`, `bivec::zero`, `mat::zero`, `mat::identity`, `quat::zero`,
`quat::identity`. Provide these for new types where a canonical value exists.

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

**Implementation note:** a static data member cannot be `constexpr` of its own (incomplete) class
type, so these are declared `static T const zero;` in the class and defined `inline` out of line
in the same header (`template <...> inline T<...> const T<...>::zero = ...;`). They are therefore
runtime constants, usable everywhere except constant expressions. Build `identity` from
`make_unit` via the `tg::impl::make_identity` helper rather than hand-writing the diagonal.

## Multi-argument `operator[]` and preprocessor macros

`mat` uses the C++23 multi-argument subscript `m[col, row]`. The comma is a problem inside
function-like macros (`CHECK`, `CC_ASSERT`, …): the preprocessor splits on it and sees two
arguments. Wrap the subscript in parentheses at any macro call site:

```cpp
CHECK((m[0, 0]) == 1);          // not CHECK(m[0, 0] == 1) — "too many macro arguments"
CHECK_ASSERTS((m[3, 0]));
```

## Constructors

- All constructors are `explicit` (per the global rule), including the scalar splat
  `explicit vec(T)`.
- The per-dimension constructors are **fixed-arity** and `requires`-gated
  (`vec(T,T) requires(D == 2)`, …) — never a variadic constructor. A variadic ctor would
  over-match and silently swallow unrelated call sites.
- The variadic entry point is the **named static** `from_values(...) requires(sizeof...(Ts) == D)`,
  kept off the constructor set for the same reason.
- Default construction **zero-initializes** (`T data[D] = {}`). The types stay trivially
  copyable regardless (only the default ctor is non-trivial); assert this with a
  `static_assert(std::is_trivially_copyable_v<...>)` in tests.

## Don't assert on common degenerate inputs; return a sensible value

The repo-wide guidance is to `CC_ASSERT` preconditions liberally. For math operations whose
degenerate input is *common in real code*, prefer returning a defined value over asserting.
Concretely, `vec::normalized()` / `quat::normalized()` return `zero` for a (near-)zero length
instead of asserting. **Why** (not obvious): zero-length normalization shows up constantly
(accumulated directions, user data, edge cases in loops), and a hard assert there produced far too
many spurious failures in the previous incarnation of this library. Use `tg::traits::is_zero(...)`
for the degeneracy test (not `== 0`) so exotic scalars decide what "zero" means.

This is a judgement call per operation, not a blanket waiver: keep asserting on genuine programmer
errors (out-of-range `operator[]`, wrong-size initializer lists, …).

## Semantic typing

Keep the type distinctions meaningful — the point of tg is that the type system encodes geometry:

- `vec` is a free vector (direction/displacement), `pos` is a point, `comp` is the neutral
  component container.
- Respect the affine algebra: `pos - pos -> vec`, `pos + vec -> pos`, `vec + vec -> vec`, and the
  deliberate `pos + pos -> pos` (translation of the singleton point set). Don't add operators
  that blur these (e.g. `pos * scalar` is not meaningful).
- `comp` is the *semantics-free* type: it is where raw component-wise arithmetic belongs
  (planned), and it should never grow geometric meaning.

## Members vs free functions (tg flavor)

Same spirit as the global rule: intrinsic, local, discoverable operations are members
(`v.length()`, `v.normalized()`, `ray.at(t)`); symmetric / cross-type / heavy / extensible
operations are free functions (`dot(a, b)`, `distance(a, b)`, `intersection(a, b)`). Free
functions for a type live in `<type>_ops.hh`, separate from the type header.
