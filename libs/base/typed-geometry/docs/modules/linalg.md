# Module: linalg

> Module docs answer **"what belongs here?"** and **"why is it this way?"** — the load-bearing
> decisions, not the full API (that's the [cheat-sheet](../../cheat-sheet.md)) and not the roadmap
> (that's [structure.md](../structure.md)). They capture the rationale behind choices that would
> otherwise trip up a reader.

## What this module is

`linalg/` holds the algebraic building blocks: `vec`, `pos`, `comp`, `bivec`, `mat`, `quat`, and
their operations. These are the vocabulary types nearly everything above (transforms, geometry,
curves, mesh) is phrased in. It depends only on `scalar/`.

## What belongs here

- Algebraic *data* types and their **intrinsic** members (cheap, local, discoverable):
  `v.length()`, `m.col(i)`, `q.conjugate()`.
- **Symmetric / cross-type** operations as free functions: `dot`, `cross`, `distance`, matrix
  products, …, split into `<type>_ops.hh` where they aren't operators.
- Pure linear algebra and the small set of "escape hatch" conversions (`dual`/`undual`).

## What does NOT belong here

- **Transforms.** A `mat` is linear-algebra data, not a semantic transformation; `rigid_transform`,
  `affine_transform`, and the `transform(t, x)` machinery live in the (planned) `transform/`
  module. There is intentionally no `mat * pos`.
- Geometry primitives (aabb, ray, triangle, …), acceleration structures, and shape algorithms —
  those are higher modules.

## Key decisions

### Storage is a raw `T data[D]`; access is index-only

Each type stores a single public C array (`T data[D]`, or `vec<R> cols[C]` for `mat`) and exposes
it through `data` and `operator[]` — there are **no `.x/.y/.z`** members, not even as functions.
This keeps one generic implementation over the dimension `D` (operations are loops), works
uniformly for every scalar type, and avoids the pull toward per-dimension specializations. Named
accessors and swizzles can be reconsidered later; we are first seeing how far the minimal form goes.

### Semantic typing: `vec` vs `pos` vs `comp`

The whole point of tg is that the type system encodes geometry, so these are distinct types with
distinct algebra rather than interchangeable tuples:

- **`vec`** is a free vector — a direction or displacement. `vec + vec -> vec`, scaling, `dot`.
- **`pos`** is a point. The affine rules are deliberate:
  `pos - pos -> vec`, `pos + vec -> pos`, `pos - vec -> pos`.
- **`comp`** is the semantics-free component bag (see below).

#### Why `pos + pos -> pos` (and `+ vec` / `+ pos` "translate")

This is the decision most likely to surprise people, and it is intentional. We think of a geometric
object as a **set of points**, and `pos` is the singleton set `{p}`. Adding something to a geometric
object **translates** it. Translating `{p}` by a vector gives `{p + v}` — hence `pos + vec -> pos`.
Translating `{p}` by another point `q` (treating `q`'s coordinates as the offset) gives
`{p + q}` — hence the otherwise-unusual `pos + pos -> pos`. So `+ vec` and `+ pos` both read as
"translate this object", and `pos` is special precisely because it is simultaneously a linear-algebra
value and a (singleton) geometric object. `pos - pos -> vec` is the displacement between two points,
the one affine operation that yields a free vector.

### `comp` is the semantics-free building block

`comp<D, T>` carries no geometric meaning — it is just "D components". That is exactly why it is the
intended future home of **all raw component-wise arithmetic** (element-wise `* / min max …`): those
operations are meaningful on plain components but *not* on a `pos` or a direction `vec`. Today `comp`
only stores and indexes; the arithmetic is planned. `comp` must never grow geometric semantics.

### `bivec` is its own type, not an alias for `vec`

In 3D a bivector has three components, the same count as `vec3`, so it is tempting to reuse `vec3`
for cross products and call it a day. tg deliberately does not:

- A bivector is an **oriented area element**, not a vector. The "cross product is a vector" shortcut
  is a 3D-only coincidence; a bivector has `C(D, 2)` components (1 in 2D, **3** in 3D, 6 in 4D), so
  the vector identification does not even typecheck in other dimensions.
- A bivector and a vector transform differently (a bivector is a pseudovector: it flips under
  reflection where a vector does not). Conflating them silently produces wrong results under
  mirroring/handedness changes.

So `cross(vec3, vec3) -> bivec3`, and converting to the familiar "cross product vector" is the
**explicit** `dual()` (with `undual()` as its inverse) — you have to ask for the
pseudovector→vector reinterpretation, it never happens implicitly. `bivec3` stores its components in
the order `{yz, zx, xy}`, chosen so that `dual`/`undual` are identity component-casts and
`dual(cross(a, b))` is exactly the classic `a × b`.

### `mat` is column-major, and its default is the zero matrix

`mat<C, R, T>` stores `C` column vectors (`vec<R> cols[C]`), so `col(i)` is a real reference and
matrix/vector products fall out as column combinations. Elements use the C++23 multi-argument
subscript `m[col, row]`. The **only constructor is the default one, and it yields the zero matrix**
— there is no "default is identity" surprise; identity is the explicit `mat::identity` constant.
`mat` is linear-algebra data only (no `mat * pos`); rotation factories live here because they
*produce* matrices, but applying a matrix as a transform is a `transform/` concern.

### Special values are static constants; factories are `make_*`

Canonical values are static data members (`vec::zero`, `mat::identity`, `quat::identity`, …) rather
than functions, so they read as constants. They are runtime constants (a static member cannot be
`constexpr` of its own incomplete type). Every factory is named `make_*` (`make_unit`,
`make_from_values`, `make_rotation_z`, …) so it is unmistakable at the call site that it constructs
a fresh value — see the [coding-guidelines](../coding-guidelines.md).

### `normalized()` returns zero instead of asserting

`vec`/`quat` `normalized()` return the zero value for a (near-)zero length rather than asserting,
because zero-length normalization is common in real code and a hard assert there caused too many
spurious failures previously. The degeneracy test goes through `tg::traits::is_zero`. Rationale is
detailed in the [coding-guidelines](../coding-guidelines.md).

## See also

- [coding-guidelines](../coding-guidelines.md) — the cross-cutting rules (storage, qualification,
  factories, statics, no-assert-normalize) these types follow.
- [cheat-sheet](../../cheat-sheet.md) — the linalg API at a glance.
- source: [vec.hh](../../src/typed-geometry/linalg/vec.hh),
  [pos.hh](../../src/typed-geometry/linalg/pos.hh),
  [bivec.hh](../../src/typed-geometry/linalg/bivec.hh),
  [cross.hh](../../src/typed-geometry/linalg/cross.hh),
  [mat.hh](../../src/typed-geometry/linalg/mat.hh),
  [quat.hh](../../src/typed-geometry/linalg/quat.hh).
