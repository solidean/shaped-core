# Plan: the geometric query layer (distance / closest / intersects / …)

Status: **agreed, being built** — the infrastructure lands first, then the objects in waves.
This is the shape of `geometry/query/`: the verbs, how a verb finds the code for a pair, the convex floor, and what a verb promises on a special case.
Background: [structure.md](../structure.md) for the `geometry/` roadmap, [modules/geometry.md](../modules/geometry.md) for the set-of-points model and `object_traits`.
[old-tg-carryover.md](old-tg-carryover.md) is the object roster and the per-verb tables.

## The problem

Binary geometric queries are O(object_types²) in pairs, times the number of verbs.
Written naively that is a large, ever-growing pile of hand-written functions: tedious, error-prone for symmetry, and hard to discover.
The old tg had about 8k lines of verbs, 3.7k of them in one `intersection.hh`, every mirrored pair a hand-written forwarder — two of which were misspelled and silently dead.

The goal: the hand-written surface grows with the number of objects rather than its square, and "what do we have?" has one answer.

## Decision summary

1. **Every query is a member**, declared per type and defined per verb (`geometry/query/<verb>.hh`).
2. **One class template per verb is the seam**, specialized per pair; the verb's one generic function walks a fixed ladder of fallbacks.
3. **Symmetry by probing both orders**: a kernel is written once, in either order.
4. **A generic convex kernel via support functions (GJK)** collapses the kernel matrix to ~O(n) support functions, and EPA on top of it gives penetration.
5. **Realtime first**: the verbs assume special cases away, never assert, and let `inf` / `NaN` propagate; an opt-in build flag logs each assumption that is violated.

## 1. The verbs, as members

| member | returns |
|---|---|
| `a.intersects(b)` | `bool` |
| `a.contains(b)` | `bool` — every point of `b` is in `a` |
| `a.intersection_with(b)` | `cc::optional<X>`, X the generic-case shape (see §5) |
| `a.closest_points_to(b)` | `cc::pair<pos, pos>`, the point of `a` first |
| `a.closest_point_to(b)` | the point of `a` nearest `b`; `obj.closest_point_to(p) == p.project_to(obj)` |
| `a.distance_to(b)` / `a.distance_sqr_to(b)` | `T` |
| `a.signed_distance_to(b)` | `T`, negative inside `b`; only for `b` with an inside |
| `a.project_to(b)` | `a` mapped onto `b`: a `pos` for a `pos`, a segment for a segment onto a plane |
| `r.intersection_parameter_with(b)` | `tg::hits<N, T>` for a surface, `cc::optional<tg::hit_interval<T>>` for a solid |
| `r.closest_intersection_parameter_with(b)` | `cc::optional<T>` — the first hit, or the interval's start (0 from inside) |
| `a.may_intersect(b)` | `bool`: false only when certainly apart — a cheap culling test (frustum, plane by plane), exact `intersects` otherwise |
| `a.separation_from(b)` | `cc::optional<tg::separation<D, T>>`, `{normal, depth}`; bounded convex solids only |
| `a.intersects(b, eps)`, `a.contains(b, eps)` | `bool`, the bracket contract of §6; only where a kernel can give it cheaply |

The suffix rule: a preposition where the relation reads from the subject (`_to`, `_with`, `_from`), bare names for predicates.
The unary verbs — measures, bounds, parameters, sampling — are per-type members and live in [old-tg-carryover.md](old-tg-carryover.md).

## 2. Declarations per type, definitions per verb

Each type header declares the verbs it supports, by hand, in one commented block:

```cpp
template <int D, class T>
struct tg::aabb
{
    // …

    // queries — defined in geometry/query/, see docs/plans/geometry-query-matrix.md
public:
    template <class Obj>
    [[nodiscard]] constexpr auto distance_to(Obj const& obj) const;
    // … one line per verb
};
```

Each verb's header defines that member for every type as a forwarding block into one generic function:

```cpp
// geometry/query/distance.hh
template <int D, class T>
template <class Obj>
constexpr auto tg::aabb<D, T>::distance_to(Obj const& obj) const { return tg::impl::distance_to(*this, obj); }
```

Including `distance.hh` makes `distance_to` callable on every type and pulls in nothing else.
Hand-written declarations cost a few lines per type and buy a header that says what the type can be asked.
A shared base declaring every verb once was considered and rejected for that reason.

## 3. The seam, the ladder, symmetry

**A kernel is a specialization** of the verb's op template, whose primary is left undefined:

```cpp
template <class A, class B> struct tg::impl::distance_sqr_op;   // undefined

template <int D, class T>
struct tg::impl::distance_sqr_op<tg::pos<D, T>, tg::aabb<D, T>>
{
    static constexpr T apply(pos<D, T> const& p, aabb<D, T> const& b) { /* clamp per axis */ }
};
```

A specialization declared after the generic function is still found when it is instantiated.
A *qualified* overloaded call (`tg::impl::distance_sqr(a, b)`) would not be: its name lookup happens where the template is written.
ADL would find later overloads, but only in namespace `tg` itself and only for unqualified calls, which the tg guidelines rule out.

**The ladder** in each verb's generic function is an ordered `if constexpr` probe, and its order is the priority:

```cpp
if constexpr (has_op<distance_sqr_op, A, B>)       return distance_sqr_op<A, B>::apply(a, b);
else if constexpr (has_op<distance_sqr_op, B, A>)  return distance_sqr_op<B, A>::apply(b, a);
else if constexpr (/* a is a pos and b projects */) return (a - a.project_to(b)).length_sqr();
else { auto const [pa, pb] = a.closest_points_to(b); return (pb - pa).length_sqr(); }   // GJK at the bottom
```

`has_op` detects a complete specialization through a `requires` on its `apply`.

**Symmetry is the second rung.**
A kernel is written once in whichever order is natural, and a verb returning per-argument data (`closest_points_to`) swaps its result back.
A ranking (`object_order`) was considered and dropped: a synthetic TU with 40 types and all 1,600 ordered pair calls compiled in the same median time either way.
**A pair with a kernel in both orders is an error**, which a test checks, because the first rung would silently shadow the second.

A concept's result is cached per TU, so `has_op<A, B>` evaluated before the kernel's header and again after is ill-formed with no diagnostic required.
Each verb header includes its kernels before it defines anything, which is what keeps a call site from seeing both answers.

**Fast paths beat the floor by partial ordering.**
`distance_sqr_op<pos<D, T>, aabb<D, T>>` is more specialized by type than the constrained `<A, B>` GJK specialization, and constraints only break ties.

## 4. The convex floor: GJK and EPA

Most objects are convex: point, segment, triangle, aabb, box, sphere, capsule, cylinder, cone, ellipsoid, tetrahedron.
For convex sets `closest_points` / `intersects` is one algorithm — GJK over the Minkowski difference — parameterized only by a **support function**, the point of an object farthest along a direction.
Support is `tg::impl::support_op<Obj>`, a kernel like any other, and not a public member until a caller needs one.

- **It is a permanent floor**, not scaffolding: a new convex type writes one support function and gets distance, closest points and intersects against every other.
  Closed forms are added top-down by profiled call frequency, and GJK is their test oracle.
- **The unbounded types** (ray, line, plane, halfspace) have support functions that run off to infinity, so they get closed forms from day one.
- **GJK iterates to a relative tolerance and carries an iteration cap**; hitting the cap returns its current best answer.
- **Measured, it is slower than the estimate this plan started from** (5–10x a closed form for box–box).
  On a Ryzen 9 5900X in a release build, distance through GJK takes 75x the closed form for aabb–aabb and 103x for ball–aabb.
  It takes 13x for segment–segment and 7x for point–triangle (`libs/base/typed-geometry/tests/benchmarks/query-benchmark.cc`).
  The simplex step is a closed form per size — segment, triangle by its Voronoi regions, tetrahedron by its faces — with an exhaustive search over every face as the fallback when that stalls.
  Before it, solving a small system for every face, the same pairs took 124x, 252x, 22x and 11x.
  A 16x looser tolerance buys only another 20–30%, so it stays at 64 machine epsilons; what remains is iteration count on curved supports.
  The hot pairs have closed forms already, so this prices the cold tail rather than a realtime path.
- **EPA** extends a GJK simplex that contains the origin to the penetration depth and normal: `a.separation_from(b)`.
- **`intersection_with` is not a GJK derivative.**
  It is defined only where the overlap is a representable primitive, and a pair whose overlap has no type has no `intersection_with` at all.

**Boundary types are not convex**: a sphere's surface is not a convex set.
They derive `intersects` from their solid: a boundary meets `b` exactly when the solid meets `b` and does not swallow it, `solid.intersects(b) && !solid.contains(b)`.
Distance from inside a solid to its boundary needs one kernel per family.

### What a scalar must provide for GJK

GJK is refused only where `tg::traits::is_exact<T>` is true — the integers, `bool`, `tg::fixed_int` — because an exact type's caller wants an exact answer or a compile error, never a tolerance.
Every other scalar is supported by default, wrappers included (an autodiff or an error-tracking float), as long as it upholds:
- **a total order** through `<`, consistent with subtraction (a NaN may break it, as it does for floats);
- **field operations** `+ - * /` with the usual identities up to rounding;
- **a relative tolerance can be formed** from the scalar's own values (`tol * max(|a|, |b|)`), so no global epsilon is needed.

An interval scalar whose `<` is not a total order cannot uphold the first, and should set `is_exact` or simply not be passed to a GJK pair.

## 5. Results

- **`cc::optional<X>`** for `intersection_with`, X the shape the overlap has when nothing is tangent, coincident or parallel.
  `sphere3_surface ∩ sphere3_surface → optional<circle3>`, `aabb ∩ aabb → optional<aabb>`, `segment ∩ plane → optional<pos>`.
  Parallel or concentric objects that never meet give no X; a coincident pair is the special case, and gives none either.
  A special case lands inside X as whatever the formula gives; exact shapes are a later `_safe` verb's job.
- **`tg::hits<N, T>`** for a ray or line against a surface: at most `N` parameters, sorted along the ray.
- **`cc::optional<tg::hit_interval<T>>`** for a ray or line against a solid: `{start, end}`, the part of the ray inside.
  Under the maximal default this is what `ray.intersection_parameter_with(sphere3)` returns; a ray tracer asks for the surface by type, `s.boundary()`.
- **`cc::optional<tg::separation<D, T>>`** from EPA: `{normal, depth}`, empty when the solids do not overlap.

## 6. Special cases

The verbs are realtime first, and they **assume special cases away**: two 3D lines do not meet, a ray is never coplanar with the triangle it is tested against.
- **No assert** on a data-dependent special case, ever.
- **`inf` and `NaN` propagate.** A verb returns whatever its straight-line formula gives.
  Plain arithmetic produces non-finite values once inputs are large enough (a `det` of a `mat4` with entries near 10^10), so guarding is the caller's job at its own boundaries.
- **An exact-zero denominator on common input is handled, not assumed away.**
  Basis vectors make exact zeros common, and a well-predicted branch costs less than the division it follows.
  So a parallel ray misses a plane, and a point on a cylinder's axis projects perpendicular to it; only a ray lying in the plane stays a special case.
  `NaN` where the answer is well-defined is a defect.
- **No UB and bounded iteration**: no computed index out of range, no integer division by zero, an iteration cap on every iterative kernel.
- **Exact comparisons**, through `tg::traits::is_zero` and plain `<`; GJK's tolerance is internal to it.

**The epsilon overloads** `a.intersects(b, eps)` and `a.contains(p, eps)` (for a point `p`) give this bracket:
- `true` if `a` and `b` share a point;
- `false` if `a.distance_to(b) > eps`;
- either, in between.

It holds up to rounding — a pair within an ulp or two of either edge may land on the wrong side — and `eps` must be `>= 0`.
The default is the exact test, `distance_sqr_to(b) <= eps²`, so the overloads exist wherever a distance does.
A kernel may pad more cheaply in whatever way suits it (a barycentric margin, say), which is why the in-between is left open; none does yet.

**The opt-in check.**
`SC_CHECK_GEOMETRY_SPECIAL_CASES` (default off, on in the `debug-nopch` presets) reaches C++ as `TG_CHECK_SPECIAL_CASES`.
Each assumption a kernel makes is a `TG_SPECIAL_CASE(cond, "what")`, which logs a warning in the `tg` recording domain when the flag is on and compiles to nothing otherwise.
nexus fails a passing test that logs an undeclared warning, so a test that feeds a special case by accident fails, and one that does so on purpose declares it with `nx::expect_warning`.

**`_safe` verbs** that handle every special case exactly are later work, and their shape — a suffix, a policy argument or a namespace — is decided when the first one is written.

## 7. Exact scalars

A verb compiles for an exact scalar only where a kernel is exact on it, and GJK is refused there (§4).
`aabb3i.intersects(aabb3i)` works through its closed form; a pair with no exact kernel has no answer on `int`, and the capability concept says so.

## 8. Discoverability

- **Capability concepts** — `has_distance_sqr<A, B>`, `has_intersection<A, B>` — are the machine-readable registry: true exactly when a kernel or a ladder rung serves the pair.
  Calling a member for an unsupported pair is a `static_assert` at the bottom of the verb's ladder, naming the concept to probe instead.
- **A support matrix** in [old-tg-carryover.md](old-tg-carryover.md): rows × columns × verbs, each cell a closed form, the GJK floor, a derivation, or unsupported.

## Layout

```
geometry/query/
  intersects.hh  contains.hh  intersection.hh  closest_points.hh
  distance.hh    project.hh   parameter.hh     separation.hh
  hits.hh                     # tg::hits, tg::hit_interval, tg::separation
  query.hh  all.hh
  impl/
    ops.hh                    # the op primaries, has_op, capability concepts
    special_case.hh           # TG_SPECIAL_CASE
    support.hh  gjk.hh  epa.hh
    kernels/                  # closed forms, one header per object family they are written for
```

## See also

- [old-tg-carryover.md](old-tg-carryover.md) — the roster, the unary verbs, measures, parameters, sampling, and what was left out.
- [modules/geometry.md](../modules/geometry.md) — the set-of-points model, `object_traits`, maximal objects and boundary types.
- [coding-guidelines.md](../coding-guidelines.md) — members vs free functions, and the special-case rule.
