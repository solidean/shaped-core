# Plan: organizing the geometric query matrix (distance / closest / intersects / …)

Status: **proposal** — no code yet.
This is the agreed shape for `geometry/query/`, settled before any binary query gets implemented.
Background: [structure.md](../structure.md) for the `geometry/` roadmap, [modules/geometry.md](../modules/geometry.md) for the set-of-points model and `object_traits`.

## Context — the problem

Binary geometric queries are inherently O(object_types²) in pairs, times the number of verbs.
The verbs: `distance`, `distance_sqr`, `closest_points`, `intersects`, `intersection`, `contains`, `project`.
Written naively that is a large, ever-growing pile of hand-written functions.
It is tedious to author, error-prone for symmetry (`distance(a,b)` vs `distance(b,a)`), and hard to discover — "which pairs actually support `intersection` today?".
The primitives that already landed (`aabb`, `triangle`, `segment`, `ray`, `line`, `plane`) make it concrete and imminent.

The goal: keep the *hand-written* surface close to O(n), let the rest fall out generically, and make "what do we have?" answerable from one place.

## Decision summary

1. **Files are organized by operation**, one header per verb under `geometry/query/`.
2. **Kernels vs. derived**: hand-write a minimal kernel per pair, and derive the other verbs once, generically.
3. **A generic convex kernel via support functions (GJK)** collapses the kernel matrix from O(n²) to ~O(n) support functions.
   It is a permanent floor, not a placeholder.
4. **Symmetry is handled once**, via a canonical argument order (`object_order` in `object_traits`) plus a generic swap.
5. **Discoverability** comes from compile-time capability concepts plus a maintained support matrix in the query module doc.

## 1. Organize by operation

```
geometry/query/
  closest_points.hh   # the kernel most others derive from
  distance.hh         # distance / distance_sqr (derived)
  intersects.hh       # bool (derived)
  intersection.hh     # the actual hit set / parameters (mostly bespoke — see below)
  contains.hh         # for solids (aabb, triangle area, halfspace, …)
  project.hh          # closest_point(p, obj)
  impl/               # generic machinery: support<>, gjk<>, simplex
```

Per-type-pair files (`segment_triangle.hh`) are literally O(n²) files, and they split symmetric operations across two homes.
Per-single-type files (`segment_ops.hh`) have the same split problem.
Per-operation means "where are all the `distance`s?" has exactly one answer.

## 2. Kernels vs. derived — only the kernels are hand-written

The verbs are not independent; the dependency graph collapses:

```
closest_points(a,b)  ──►  distance_sqr  ──►  distance      (sqrt, gated on has_sqrt)
                                        └─►  intersects     (distance_sqr <= 0 for solids)
intersection(a,b)    ──►  intersects    (= bool(intersection))
contains / signed_distance  ──►  intersects for halfspace/plane/aabb
```

So each pair provides exactly one minimal kernel — usually `closest_points`, or `distance_sqr` when
there is a cheaper closed form, or an `intersection_parameter` for ray-vs-X — and the derivations are
written *once*:

```cpp
// distance.hh — derived layer
template <class A, class B>
    requires has_closest_points<A, B>
[[nodiscard]] auto distance_sqr(A const& a, B const& b)
{
    auto const [pa, pb] = closest_points(a, b);
    return (pb - pa).length_sqr();
}

template <class A, class B>
    requires (has_distance_sqr<A, B> && traits::has_sqrt<scalar_of<A>>)
[[nodiscard]] auto distance(A const& a, B const& b) { return tg::sqrt(distance_sqr(a, b)); }
```

A pair gets `distance` / `distance_sqr` / `intersects` for free the moment its kernel exists.
The generic derivations must be *less specialized* than any explicit fast-path overload, so partial ordering picks the fast path where one is present.

## 3. The lever: one generic convex kernel via support functions

Almost everything here is a **convex** point set: point, segment, ray, line, triangle, aabb, plane, halfspace.
For convex sets `closest_points` / `intersects` is *one* algorithm — GJK over the Minkowski difference.
It is parameterized only by a **support function** `support(obj, dir) -> pos`, the farthest point of the object along `dir`:

```cpp
template <class A, class B>
    requires (has_support<A> && has_support<B>)
[[nodiscard]] auto closest_points(A const& a, B const& b) { /* GJK, in impl/ */ }
```

This turns the kernel matrix from O(n²) into **~O(n) support functions**.
A new convex primitive writes one `support()` and immediately gets distance, closest and intersects against every existing convex type.
Hand-written closed forms are then added as more-specialized overloads, only where they pay off.

### How good is GJK out of the box? (the efficiency question)

Short answer: **a genuinely good deal immediately, and a permanent floor** — not throwaway scaffolding we expect to replace everywhere.

- **Support functions are tiny and fully inlinable.**
  `aabb` picks min or max per axis by the sign of the `dir` component, branchless.
  `triangle` and `segment` are an `argmax` of dot over 3 or 2 vertices, and `point` is itself.
  With GJK monomorphized per pair these inline straight into the loop, and `if constexpr` on the dimension unrolls the simplex sizes.
- **What does *not* vanish** is GJK's own control flow: the iterate-until-converge loop, the simplex and sub-distance update, and a convergence tolerance.
  For our low-complexity primitives that is a handful of iterations, often 1–3.
  It is still a loop with branches and an epsilon, where a closed form is straight-line and exact.
- **Rough cost model.**
  Box–box via GJK is a few iterations × (2 support evals + simplex update), so tens to low hundreds of compares and flops.
  A closed-form box–box is about a dozen ops, making GJK roughly 5–10×.
  point–aabb has a ~6-op closed form, so GJK is overkill there at ~20–50×.
  segment–segment and point–triangle closed forms are a few dot products and beat GJK by ~5–20×.
- **So which pairs get replaced over time?**
  The **hot head** of the call distribution, conveniently also the simplest closed forms to write: point, segment, triangle and aabb combinations.
  The **cold tail stays on GJK forever** — correct, allocation-free, and fast *enough* off the hot path.
  We do **not** expect to replace almost every pairing.
- **GJK is also the only long-term answer for genuine convex hulls and polytopes**, a future primitive where a closed form may not even exist.
  So the machinery is not disposable.

Caveats that shape where closed forms are *mandatory* rather than optional:

- **Unbounded objects** (`ray`, `line`, `plane`, `halfspace`) have support functions that run off to infinity in some directions, which vanilla GJK does not handle.
  They want closed forms regardless, and they are exactly the easy ones — ray–plane, point–plane and segment–plane are a few dot products.
  So GJK's clean domain is the **bounded** convex primitives (point, segment, triangle, aabb), and the unbounded ones are bespoke from day one.
- **`intersection`, meaning the constructed overlap geometry, is not what GJK gives you.**
  GJK yields closest points, a boolean, and with EPA a penetration depth — not "the segment where these two overlap".
  So `intersection.hh` is inherently more per-pair than `distance`/`intersects`: a separate, mostly hand-written verb rather than a GJK derivative.
  **It is fine for `intersection` to stay partial by design.**
  It is defined only for pairs whose overlap is itself a representable primitive: segment∩plane → a point, aabb∩aabb → an aabb, ray∩triangle → a point.
  A pair whose overlap has no type we can name — a general triangle∩triangle region, or any non-convex result — simply has no `intersection` overload.
  That is a deliberate boundary rather than a gap to backfill, and `has_intersection<A,B>` makes it explicit instead of a silent omission.
  `intersects` and `distance` stay total across the convex matrix via GJK regardless; only the *constructive* verb is selective.

Bottom line for the rollout: ship GJK as the convex floor, so the *whole* bounded-convex matrix lights up at once and correctly.
Treat it as permanent infrastructure, and add closed forms top-down by profiled call frequency — immediately for the unbounded primitives.
Use GJK results as the oracle to test the closed forms against.

## 4. Symmetry — write one order, auto-swap

Add a total ordering key to `object_traits`, called `object_order`.
`intrinsic_dim` alone is not total: `segment` and `ray` tie.
Implement kernels in canonical order, lower rank first, and one generic swap covers the mirror:

```cpp
template <class A, class B>
    requires (object_order<A> > object_order<B> && has_closest_points<B, A>)
[[nodiscard]] auto closest_points(A const& a, B const& b)
{
    auto const [pb, pa] = closest_points(b, a); // swap the result pair back into (a,b) order
    return cc::pair{pa, pb};
}
```

Symmetric scalar and bool verbs (`distance`, `intersects`, `intersection`) are even simpler.
This halves the matrix and removes the "did I implement both orders?" footgun.

## 5. Discoverability

- **Capability concepts** — `has_closest_points<A,B>`, `has_intersection<A,B>`, `has_support<A>`, … — are the machine-readable registry.
  Call sites, the swap layer and `static_assert`s all query them.
  An unsupported pair then yields a clean `static_assert` ("no `distance()` for `plane`×`plane`") instead of a deep template error.
- **A maintained support matrix** in the query module doc: rows × cols × verbs, each cell marking generic-via-GJK, a hand-written fast path, or unsupported.
  Because most cells are "convex ⇒ all via support function", the table stays short.
  List the support-enabled types once, then the handful of explicit fast paths and the non-convex or unbounded exceptions, kept current like the cheat-sheets.

## Suggested rollout order

1. `object_traits`: add `object_order` (a total order) plus a `point`/`pos` entry, so `pos` participates as a primitive.
2. `query/impl/`: the `support<>` concept and support functions for the bounded primitives (point, segment, triangle, aabb), the GJK kernel, and the canonical-order swap.
3. `closest_points.hh` (generic convex) plus the derived `distance.hh` / `intersects.hh` layer.
   The whole bounded-convex matrix lights up.
4. Unbounded primitives (`ray`/`line`/`plane`/`halfspace`): closed-form kernels.
5. `intersection.hh` / `contains.hh`, per pair, starting with the most-used.
   `intersection` stays partial by design: implement it only where the overlap is a representable primitive, and leave the rest to `has_intersection<A,B>` reporting "unsupported".
6. Profile, then add closed-form fast paths for the hot pairs.
   Write the support matrix doc and the capability concepts as the public discoverability surface.

## Open questions

- Where does `pos`/point sit — a real primitive with `object_traits` and `support`, or special-cased?
  Leaning towards a real primitive, since it makes point–X fall out of the same machinery.
- Penetration depth: EPA for overlapping convex now, or defer until a caller needs it?
- Exact and symbolic scalars through GJK: the convergence tolerance assumes floating point, so closed forms are the exact-scalar path.
  The matrix should mark which verbs are exact-safe.

## See also

- [structure.md](../structure.md) — the `geometry/` roadmap (`query/`, `measure/`, `construct/`).
- [modules/geometry.md](../modules/geometry.md) — set-of-points model, `object_traits`.
- [traits.hh](../../src/typed-geometry/geometry/traits.hh) — where `object_order` would land.
