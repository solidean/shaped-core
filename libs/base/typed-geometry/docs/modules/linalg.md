# Module: linalg

> Module docs answer **"what belongs here?"** and **"why is it this way?"** — [docs/_index.md](../_index.md#module-docs) states the contract they follow.

## What this module is

`linalg/` holds the algebraic building blocks — `vec`, `pos`, `comp`, `bivec`, `mat`, `quat` — and their operations.
These are the vocabulary types nearly everything above (transforms, geometry, curves, mesh) is phrased in.
It depends only on `scalar/`.

## What belongs here

- Algebraic *data* types and their **intrinsic** members (cheap, local, discoverable): `v.length()`, `m.col(i)`, `q.conjugate()`.
- **Symmetric / cross-type** operations as free functions: `dot`, `cross`, `distance`, matrix products, …, split into `<type>_ops.hh` where they aren't operators.
- Pure linear algebra and the small set of "escape hatch" conversions (`dual`/`undual`).

## What does NOT belong here

- **Transforms.** A `mat` is linear-algebra data, not a semantic transformation.
  `rigid_transform`, `affine_transform` and the `obj.transformed(t)` machinery live in the [`transform/`](transform.md) module.
  There is intentionally no `mat * pos`, and no `t * p` either.
- Geometry primitives (aabb, ray, triangle, …), acceleration structures, and shape algorithms —
  those are higher modules.

## Key decisions

### Storage is a raw `T data[D]`; access is index-only

Each type stores a single public C array — `T data[D]`, or `vec<R> cols[C]` for `mat` — reached through `data` and `operator[]`, with **no `.x/.y/.z`** members.
The [coding-guidelines](../coding-guidelines.md) own the rule and the reasoning behind it.

### Semantic typing: `vec` vs `pos` vs `comp`

The whole point of tg is that the type system encodes geometry, so these are distinct types with distinct algebra rather than interchangeable tuples.

- **`vec`** is a free vector — a direction or displacement, with `vec + vec -> vec`, scaling and `dot`.
- **`pos`** is a point, and its affine rules are deliberate: `pos - pos -> vec`, `pos + vec -> pos`, `pos - vec -> pos`.
- **`comp`** is the semantics-free component bag (see below).

#### Why `pos + pos -> pos` (and `+ vec` / `+ pos` "translate")

This is the decision most likely to surprise people, and it is intentional.
A geometric object **is** the set of points it represents, and `pos` is the singleton set `{p}`.
Adding something to a geometric object **translates** it.
Translating `{p}` by a vector gives `{p + v}`, hence `pos + vec -> pos`.
Translating `{p}` by another point `q`, whose coordinates are read as the offset, gives `{p + q}` — hence the otherwise-unusual `pos + pos -> pos`.
So `+ vec` and `+ pos` both read as "translate this object".
`pos` is special precisely because it is simultaneously a linear-algebra value and a singleton geometric object.
`pos - pos -> vec` is the displacement between two points, the one affine operation that yields a free vector.

### `comp` is the semantics-free building block

`comp<D, T>` carries no geometric meaning — it is just "D components".
That is exactly why it is the home of **all raw component-wise arithmetic**.
Every operator is element-wise including the Hadamard `*` and `/`, a scalar operand broadcasts to all components, and `comp_ops.hh` adds component-wise `min`/`max`.
Those operations are meaningful on plain components but not on a `pos` or a direction `vec`, which is why `vec`/`pos` deliberately omit Hadamard products and scalar broadcast.
`comp` must never grow geometric semantics in the other direction either.

### `bivec` is its own type, not an alias for `vec`

In 3D a bivector has three components, the same count as `vec3`, so it is tempting to reuse `vec3` for cross products and call it a day.
tg deliberately does not:

- A bivector is an **oriented area element**, not a vector.
  The "cross product is a vector" shortcut is a 3D-only coincidence.
  A bivector has `C(D, 2)` components — 1 in 2D, **3** in 3D, 6 in 4D — so the identification does not even typecheck in another dimension.
- A bivector and a vector transform differently, because a bivector is a pseudovector and flips under reflection where a vector does not.
  Conflating them silently produces wrong results under mirroring or a handedness change.

So `cross(vec3, vec3) -> bivec3`, and converting to the familiar "cross product vector" is the **explicit** `dual()`, with `undual()` as its inverse.
The pseudovector→vector reinterpretation never happens implicitly; you have to ask for it.
`bivec3` stores its components in the order `{yz, zx, xy}`, chosen so `dual`/`undual` are identity component-casts and `dual(cross(a, b))` is exactly the classic `a × b`.

### `mat` is column-major, and its default is the zero matrix

`mat<C, R, T>` stores `C` column vectors (`vec<R> cols[C]`), so `col(i)` is a real reference and matrix/vector products fall out as column combinations.
Elements use the C++23 multi-argument subscript `m[col, row]`.
The **only constructor is the default one, and it yields the zero matrix** — there is no "default is identity" surprise, and identity is the explicit `mat::identity` constant.
`mat` is linear-algebra data only, with no `mat * pos`.
Rotation factories live here because they *produce* matrices; applying a matrix as a transform is a `transform/` concern.

### Special values are static constants; factories are `make_*`

Canonical values are static data members — `vec::zero`, `mat::identity`, `quat::identity` — rather than functions, so they read as constants.
Every factory is named `make_*` (`make_unit`, `make_from_values`, `make_rotation_z`), so a call site cannot mistake it for an operation on an instance.
The [coding-guidelines](../coding-guidelines.md) carry both rules, including why such a static member is a runtime constant rather than `constexpr`.

### `normalized()` returns zero instead of asserting

`vec`/`quat` `normalized()` return the zero value for a (near-)zero length rather than asserting, with the degeneracy test going through `tg::traits::is_zero`.
The [coding-guidelines](../coding-guidelines.md) carry the rationale.

## See also

- [coding-guidelines](../coding-guidelines.md) — the cross-cutting rules (storage, qualification, factories, statics, no-assert-normalize) these types follow.
- [cheat-sheet](../../cheat-sheet.md) — the linalg API at a glance.
- source: [vec.hh](../../src/typed-geometry/linalg/vec.hh), [pos.hh](../../src/typed-geometry/linalg/pos.hh), [bivec.hh](../../src/typed-geometry/linalg/bivec.hh), [cross.hh](../../src/typed-geometry/linalg/cross.hh), [mat.hh](../../src/typed-geometry/linalg/mat.hh), [quat.hh](../../src/typed-geometry/linalg/quat.hh).
