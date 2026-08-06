# Module: transform

> Module docs answer **"what belongs here?"** and **"why is it this way?"** — the load-bearing decisions, not the full API (that's the [cheat-sheet](../../cheat-sheet.md)) and not the roadmap (that's [structure.md](../structure.md)). They capture the rationale behind choices that would otherwise trip up a reader.

## What this module is

`transform/` holds the semantic transformation types — the things you *apply* to geometry, as opposed to `mat`, which is linear-algebra data.
It depends only on `scalar/` and `linalg/`.

There is one transform type that does maths:

```cpp
template <int DSource, int DTarget, class T, transform_flags Flags>
struct homogeneous_transform;
```

plus `composed_transform<Outer, Inner>`, which does none — it holds two arbitrary transforms and applies them in order (see the composition section).

`Flags` names a **capability class**: what the transform is allowed to contain.
Everything else is an alias over it, and every alias is **square** (`DSource == DTarget`), which is what almost all code wants.

```cpp
tg::rigid_transform3f       // rotation | translation
tg::similarity_transform3f  // + a positive uniform scale
tg::affine_transform3f      // a general linear part | translation
tg::projective_transform3f  // + a non-trivial homogeneous row
```

## What belongs here

- The transform type, its factories and accessors, `composed` / `inverse` / `to_mat`.
- `apply_pos` / `apply_linear` / `apply_bivec`, the pieces only the transform can compute because they depend on its storage.
- The explicit widening constructor a geometric object branches on to decide what it becomes.

## What does NOT belong here

- **Any geometric type.** Each object writes its own `transformed` member in `geometry/`, which sits above this module.
  Nothing here names a geometric type, so the dependency runs one way only.
- Camera / projection conventions.
  `make_from_mat` takes a homogeneous matrix, but tg does not pick a handedness or a depth range — that is a rendering convention, not a geometric one.

## Key decisions

### A transform names both a source and a target dimension

A transform is a map between two spaces, and the type says which two.
That is what lifting a 2D object into 3D, or projecting a 3D one onto a plane, needs from the type system: the shapes follow from the pair.

- `apply_pos` takes a `pos<DSource, T>` and returns a `pos<DTarget, T>`; likewise `apply_linear` and `apply_bivec`.
- the linear part is a `mat<DSource, DTarget, T>` — one column per source axis, one row per target axis, matching tg's `mat<C, R>` convention.
- the translation lives in the **target** space, so `to_mat()` is a `mat<DSource + 1, DTarget + 1, T>`.
- `tg::inverse` swaps them: the inverse of a `<DSource, DTarget>` map is a `<DTarget, DSource>` one.
- `composed` chains them: `a.composed(b)` needs `b` to land in `a`'s source space, and runs from `b`'s source to `a`'s target.

**Only the square case is implemented**, and the type `static_assert`s `DSource == DTarget`.
The parameter and every signature above are in place; the lifting and projecting maths is not, and neither is the storage — `transform_storage` still carries a single dimension.

The cost of the parameter is one more template argument on a type almost nobody spells directly.
Every named alias is square, so `tg::rigid_transform3f` and `tg::affine_transform<D, T>` are unchanged, and so is every object's `transformed` chain, which asks about those aliases and nothing else.

### One type over a lattice, not a family of types

`rigid_transform` and `affine_transform` as separate hand-written types would mean writing `composed`, `inverse`, `to_mat` and every conversion once per pair.
Making the capability set a template parameter collapses that to one implementation with an `if constexpr` over the storage kinds, and it makes the *result class of a composition* computable rather than hand-maintained.

The price is that the flags need real algebra behind them, which is the next three sections.

### Flags must be canonical, and canonicalization is not obvious

Several bit patterns denote the same set of transforms.
If both spellings were allowed as template arguments, the same capability would have two distinct types.
So `tg::canonical` reduces each to one representative, and the transform `static_assert`s that its flags are canonical.
There are exactly **19** classes.

Three rules carry the weight:

**Rotation plus non-uniform scaling is a general linear map.** `R1 S1 R2 S2` is not of the form `R S`, so a class holding "a rotation and a per-axis scaling" would not be closed under composition.
This is exact rather than conservative: by the SVD every invertible `A` is `U Σ Vᵀ` — a rotation, a signed diagonal, a rotation — so the closure of `SO(D)` and the diagonals really is all of `GL(D)`.

**Scale factors are positive unless `negative_scaling` says otherwise.** That is what keeps the narrow classes cheap — see the section on it below.
In 3D `signed_similarity` is then the *full* conformal group, since a negative uniform scale composed with a half-turn is a plane reflection; it is precisely the class under which a sphere still maps to a sphere.

### `negative_scaling` is a flag, so the narrow classes stay cheap

A scale factor being allowed to go negative is a real capability, and it is the only thing that makes several operations non-trivial.
Making it a flag lets the common case skip them entirely:

- an `aabb` under a **positive** scaling keeps its corners in order, so the image is just the two transformed corners.
  Only `signed_scaling_translation` pays for re-sorting min and max per axis.
- a `sphere` under a **positive** similarity just multiplies its radius.
  Only `signed_similarity` takes the magnitude.

The factories enforce the promise: `similarity_transform3f::make_uniform_scaling(-2)` asserts, and you reach for `signed_similarity_transform3f` instead.

`linear`, `affine` and `projective` carry the flag by nature — "a general invertible linear map" has no sign restriction, and tg does not model `GL+(D)` separately.
Canonicalization also clears the flag when there is no scaling at all, since there would be nothing for it to apply to.

The flag does **not** change storage: `linear_part()` strips it alongside translation and projection, so a signed similarity stores exactly what a similarity stores.
That is why the storage table and the `if constexpr` chains key on `tg::impl::linear_kind::*` rather than on `transform_class::*` — the latter would silently mismatch for `linear`.

### Containment is `is_subclass`, never `has_all`

This is the module's easiest mistake, so it is worth stating plainly.

Canonicalization **clears** bits — `affine` drops `uniform_scaling`, because `non_uniform_scaling` subsumes it — which means:

```cpp
tg::has_all(transform_class::affine, transform_class::similarity)   // FALSE
tg::is_subclass(transform_class::similarity, transform_class::affine) // true
```

even though every similarity *is* an affine map.
`canonical(a | b)` is the join in the class lattice, so containment is "the join is already the wider class":

```cpp
constexpr bool is_subclass(transform_flags sub, transform_flags super)
{ return tg::canonical(sub | super) == super; }
```

A corollary worth remembering: `canonical` is **not monotone** under bit-subset.
Every ordering question has to go through `is_subclass`.

The lattice laws — idempotence, commutativity, and that the join is the *least* upper bound — are proven exhaustively by a `consteval` sweep in `tests/transform/transform-flags-test.cc`. That test is the foundation the widening constructor and every dispatch branch rest on.

### Widening is explicit, and that is load-bearing

Converting a `rigid_transform` to an `affine_transform` is lossless, so an implicit conversion looks harmless.
It is not.

With implicit widening, every class would convert to every wider one silently, so `requires { tg::similarity_transform<D, T>(t); }` would be true for a projective transform too and an object's chain could no longer tell the classes apart.
Explicit widening makes the question exact.

Narrowing is not a constructor at all: it is lossy, and letting it into overload resolution would reintroduce the same problem.

### Storage is chosen from the flags and is never exposed

| class | storage |
|---|---|
| translation | `vec` |
| rotation | a unit complex number (2D) or a `quat` (3D) |
| similarity | `{rotation, T}` + `vec` |
| affine | `mat<D,D>` + `vec` |
| projective | `mat<D+1,D+1>` |

The transform holds exactly **one** member, a storage type selected by a small trait.
A storage base class would have been the obvious alternative, but it loses standard-layout the moment both halves carry members — which is every common case.
`[[no_unique_address]]` was likewise rejected: it is a no-op under the MSVC ABI, i.e. under both Windows compilers this repo targets.
A pure translation gets its own layout instead of an identity linear part next to a vector, since an empty member still occupies a byte that alignment then rounds up to a whole scalar.

Every storage member carries an explicit default initializer, because `vec`, `mat` and `quat` all default to **zero** — without them a default-constructed transform would be singular rather than the identity.

### Uniform scale is NOT folded into the quaternion

An unnormalized quaternion applied through the sandwich `q v q̄` scales by `|q|²`, which is tempting: rotation and uniform scale in four floats instead of five.
It is the wrong trade.

- `|q|²` is a sum of squares, so it is never negative.
  A **signed** scale is outside the image of the map entirely — and per the section above, the signed scale is exactly what makes `similarity` the full sphere-preserving group in 3D.
- Folding forfeits **renormalization**. With `{quat, T}` the invariant `|q| ≡ 1` can be restored after a long chain of compositions; folded, drift in `|q|` is indistinguishable from a legitimate change of scale, so there is nothing left to restore.
- At `s → 0` the folded quaternion loses the rotation irrecoverably.
- `tg::quat`'s own `operator*(quat, vec)` is written in the unit-only form, correct only when `w² + |u|² = 1`. A folded value would be a different thing wearing `quat`'s clothes.

The saving would have been four bytes and roughly no arithmetic.
Because the table lives in `tg::impl`, the decision is reversible per dimension without an API change.

### The object handshake is an `if constexpr` chain the object writes itself

An object's `transformed` member asks the transform, in the object's own order of preference, which **view** it can produce.
The first one that compiles decides both the maths and the result type:

```cpp
template <class TransformT>
[[nodiscard]] constexpr auto transformed(TransformT const& t) const
{
    if constexpr (requires { t.custom_transform(*this); })   // the transform special-cases this object
        return t.custom_transform(*this);

    else if constexpr (requires { tg::similarity_transform<D, T>(t); })   // angles preserved -> still a sphere
    { ... return sphere(...); }

    else if constexpr (requires { tg::affine_transform<D, T>(t); } && D == 3)   // ... otherwise an ellipsoid
    { ... return ellipsoid<T>(...); }

    else
        static_assert(false, "tg: a sphere only survives a similarity or, in 3D, an affine map");
}
```

The priority order is literally the order of the branches, in the object's own header, next to the maths it selects.
There is no registry, no ranking and nothing for a reader to hold in their head — which is why this replaced an earlier design that ranked registrations against a precomputed ladder of every class.

There is no library-side machinery at all beyond the widening constructor the type already needs.
It is gated on `is_subclass`, so `tg::similarity_transform<D, T>(t)` compiles exactly when `t` really is a similarity — which is what makes it a meaningful question to ask in a `requires`. The narrower-or-equal case is covered too, since a same-class conversion goes through the copy constructor.

One branch therefore covers many inputs: `aabb` asks only for `scaling_translation_transform<D, T>`, and thereby handles the identity, a pure translation and both scalings, because each of those *is* a scaling-plus-translation.

### Applying and composing are the same operation, spelled three ways

These are one value, and the library is arranged so that they cannot drift apart:

```cpp
a(b(obj))  ==  a.composed(b).transform(obj)  ==  obj.transformed(b).transformed(a)
```

The first two identities are free.
`operator()` and `transform` both route straight back to `obj.transformed(*this)`, so the object decides the maths and the return type exactly once.
The third is the one with content: `composed` must produce a transform whose action is "b, then a", which is what `compose_same` computes on the join class.

The equivalence extends to the **return type**, not just the value.
Composing a similarity with a per-axis scaling gives an affine transform, under which a sphere is an ellipsoid — and the chained spelling reaches an ellipsoid too, because the sphere becomes one at the scaling step and an ellipsoid stays an ellipsoid under the similarity.
Where a pair is unsupported, both spellings fail: a rotation composed with a translation is rigid, which an `aabb` rejects, and so does the chain at its rotation step.

### Composition is opt-in, and `composed` is the opt-in

Nothing in the library requires a transform type to be composable.
A type declares `composed` for exactly the transforms it can absorb, and `homogeneous_transform` absorbs every class of the same dimension and scalar.
Unlike `transformed`, this is genuinely probeable: nothing here ends in a `static_assert`, the member simply is or is not there.

```cpp
requires { a.composed(b); }   // "can these two be fused into one transform?"
```

A pair with no `composed` is not a dead end, because `tg::compose` has a fallback — the next section.

### `tg::compose` is total, because it can fall back to holding both

`composed` fuses, and fusing is strictly better where it exists: one transform, one application, and a result class the type system knows.
But it is opt-in, so it is not always there — and a caller who just wants "b then a" should not have to care.

`tg::compose(a, b)` decides at compile time:

```cpp
if constexpr (requires { a.composed(b); })
    return a.composed(b);                            // one fused transform of the join class
else
    return composed_transform<TransformA, TransformB>(a, b);   // the two, held side by side
```

`composed_transform` stores the two and applies `inner` first, then `outer`.
It answers for every object through a **public** `custom_transform` — the first branch of every object's chain — which simply does `obj.transformed(inner).transformed(outer)`.
Public is right here, and not a violation of the private-plus-friend rule below: that rule exists so a transform cannot quietly override the answer for an object it was not written for, and `composed_transform` gives exactly the answer its parts give, for anything they can answer for.

The return type is the whole point: it says at compile time whether you got a fused transform or a pair.
The cost of the pair is real — an object is transformed twice and passes through whatever intermediate type the inner step produced — so `compose` reaches for the fused form first, always.

**There is deliberately no `operator*`.** A transform is applied, not multiplied, and once `*` means composition a reader expects `t * p` to mean application — which tg does not have, on purpose.
`composed` also reads in the order it acts.

### Both spellings work, and there is still one implementation

`obj.transformed(t)`, `t.transform(obj)` and `t(obj)` are all public, and the last two route straight back to the first.
Only the object ever decides anything, so they can never disagree — including on the return type.
`t(u)` for a transform `u` is not composition and does not compile: application and composition stay distinct spellings.

A transform that wants to special-case a particular object does **not** override `transform` or `operator()`, which would break that.
It declares a **private** `custom_transform(ObjT const&)` and befriends that object:

```cpp
struct my_transform
{
private:
    template <int D, class T>
    friend struct tg::sphere;

    tg::sphere3f custom_transform(tg::sphere3f const& s) const { ... }
};
```

The object's chain asks for `requires { t.custom_transform(*this); }` first, and access checking is part of that question — an object that was not befriended does not see the branch at all and falls through to the normal chain.
So the special case stays out of the public API, and the transform states exactly which objects it is allowed to answer for.

### `pos`, `vec` and `bivec` branch on the class too

They are in `linalg`, below this module, but they use the same chain — because the class is what decides the answer, not which members the transform happens to have:

- a **translation** moves a `pos` by one addition, and leaves a `vec` or a `bivec` completely alone.
  Neither the linear part nor a matrix is touched.
- everything wider falls through to the transform's own `apply_pos` / `apply_linear` / `apply_bivec`,
  which is where the linear part, the translation and any perspective divide belong.
- `vec` and `bivec` stop at the affine branch, so a projective transform never reaches them at all.

Naming the classes means the linalg headers include `transform/fwd.hh`. That is forward declarations only — it pulls in nothing from linalg, and the complete transform type is only needed at the call site, so the module dependency still runs one way.

What the linalg types do *not* do is re-derive the maths.
`apply_pos`, `apply_linear` and `apply_bivec` stay on the transform, because only it knows whether the linear part is a quaternion, a scalar or a matrix; going through `linear_mat()` would build a 3x3 for what may be a single quat.
`bivec` has a second reason: the cofactor it needs lives in `cross.hh`, which includes `bivec.hh`.

### An unsupported pair is a compile error on purpose

A rotated `aabb` is not an `aabb`. Returning the enclosing box instead would be a silent, lossy answer to a question the caller did not ask, so the chain falls through to its `static_assert` and says so.
The same holds for a projected `ray` (its point at infinity maps to a finite point, so the image is a bounded segment) and a projected `sphere` (a general quadric).

Each of those gaps names a type tg does not have yet — `obb`, a clipped segment, `quadric`.

The cost of that design is that "can X be transformed by Y" is not separately probeable: the member's return type is `auto`, so asking would instantiate the body and trip the `static_assert`. Probe the branch condition instead — `requires { tg::scaling_translation_transform<D, T>(t); }` is exactly why an `aabb` accepts or rejects a given transform.

### A normal is a `bivec`, not a `vec`

A bivector does not transform by the linear part.
It transforms by that map's **second exterior power**, so in 3D its dual — a normal — picks up the cofactor matrix `det(A)·A⁻ᵀ`. Under a rigid or similarity transform this collapses back to the rotation up to a positive factor, which is why the bug hides until someone applies a non-uniform scaling.

`transform/` is where linalg's `bivec` finally pays for itself: the type system makes the distinction unavoidable instead of a comment nobody reads.
