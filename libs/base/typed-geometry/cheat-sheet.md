# typed-geometry cheat sheet

Strongly-typed C++23 math & geometry.
Namespace `tg`. Depends on clean-core.
Headers are included by full path from `src/`: `#include <typed-geometry/<module>/<name>.hh>`.

> **Scope note:** this single sheet covers the surface that exists today (`scalar`, `linalg`, `transform`, and the `geometry` primitives). As the library grows we will likely split it into per-module cheat sheets — the eventual API surface is far too large for one file.
> For the *why*, read the header `///` docs and [docs/structure.md](docs/structure.md).

How to read this: each block leads with the include; one symbol per line with a trailing comment giving the return type / intuition.
Format conventions live in [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

---

## Types & typedefs

```cpp
#include <typed-geometry/fwd.hh>          // forward decls + all aliases
tg::vec<D, T>  tg::pos<D, T>  tg::comp<D, T>  tg::bivec<D, T>   // generic over dimension D
tg::mat<C, R, T>   tg::quat<T>   tg::angle<T>                   // matrix / quaternion / angle
tg::homogeneous_transform<DSource, DTarget, T, Flags>           // the one transform type (dims are 2 or 3)
// dimensional alias templates: vec2/3/4, pos2/3/4, comp2/3/4, bivec2/3/4, mat2/3/4   e.g. tg::vec3<T>
// concrete typedefs — suffix attaches to a trailing digit, else separated by '_':
tg::vec3f tg::pos3f tg::comp3f tg::bivec3f tg::mat3f   // f=f32, d=f64, i=i32  (e.g. vec2d, mat4i)
tg::quat_f tg::quat_d   tg::angle_f tg::angle_d        // quat/angle end in a letter -> '_f'/'_d'
```

## vec — displacement / direction

```cpp
#include <typed-geometry/linalg/vec.hh>
tg::vec3f v;                              // default: zero-initialized {0,0,0}
auto a = tg::vec3f(2.0f);                 // splat -> {2,2,2}              (explicit)
auto b = tg::vec3f(1, 2, 3);             // per-dim ctor, requires D==2/3/4 (explicit)
auto c = tg::vec3f({1, 2, 3});                 // initializer_list, CC_ASSERTs size == D (explicit)
auto d = tg::vec3f::make_from_values(1,2,3);   // variadic, requires sizeof...(args) == D
auto e = tg::vec3f::make_unit(1);              // {0,1,0}; CC_ASSERTs 0 <= idx < D
tg::vec3f::zero;                                // static constant {0,0,0} (runtime const)

v.data;                                   // T[D] — the raw storage (public). NO .x/.y/.z
v[i];                                      // T& / T const& — CC_ASSERTs 0 <= i < D
v.length_sqr();                            // T   — sum of squares (any scalar)
v.length();                                // T   — requires has_sqrt<T>
v.normalized();                            // vec — requires has_sqrt<T>; returns zero if length is ~0
v.transformed(t);                          // vec — the LINEAR part only; no projective transform

a + b   a - b   -a   a * s   s * a   a / s     // vec arithmetic (s is a scalar T)
a += b  a -= b  a *= s  a /= s
a == b  a != b                             // component-wise
```

```cpp
#include <typed-geometry/linalg/vec_ops.hh>
tg::dot(a, b);                             // T   — dot product
tg::normalize(v);                          // vec — free form of v.normalized() (requires has_sqrt<T>)
```

## pos — point (affine arithmetic)

```cpp
#include <typed-geometry/linalg/pos.hh>
tg::pos3f p;                              // default: origin {0,0,0}; same ctor set as vec
p.data;   p[i];                            // storage + indexed access (as vec)

q - p;                                     // vec  — displacement between points
p + v;    v + p;    p - v;                 // pos  — translate a point
p + q;                                     // pos  — translation of singleton {p} (adds coords)
p += v;   p -= v;                          // pos  — in place
p.transformed(t);                          // pos  — incl. the projective divide (asserts w != 0)
p == q;                                    // component-wise
```

```cpp
#include <typed-geometry/linalg/pos_ops.hh>
tg::distance_sqr(p, q);                    // T  — squared distance (any scalar)
tg::distance(p, q);                        // T  — requires has_sqrt<T>
```

## comp — neutral component container (raw component-wise arithmetic)

```cpp
#include <typed-geometry/linalg/comp.hh>
tg::comp3f c;                            // zero-init; same ctor set as vec/pos; tg::comp3f::zero
c.data;   c[i];   c == c2;                // storage + indexed access + comparison
// fully element-wise; a scalar operand broadcasts. (vec/pos do NOT have these — comp is the home.)
a + b   a - b   a * b   a / b   -a        // comp-comp: + - and Hadamard * /
a + s   s + a   a - s   s - a             // scalar broadcast (both sides)
a * s   s * a   a / s   s / a
a += b  a -= b  a *= b  a /= b            // compound (comp or scalar rhs)
```

```cpp
#include <typed-geometry/linalg/comp_ops.hh>
tg::min(a, b);  tg::max(a, b);            // comp — component-wise
tg::min(a, s);  tg::max(a, s);            // comp — against a broadcast scalar bound
```

## bivec — bivector + the 3D cross/dual

```cpp
#include <typed-geometry/linalg/bivec.hh>
tg::bivec3f b;                                  // C(D,2) components: 1 in 2D, 3 in 3D, 6 in 4D
tg::bivec3f::num_components;                     // static constexpr int
b.data;  b[i];  b == b2;  b + b2;  -b;  s * b;  b / s;   tg::bivec3f::zero
b.transformed(t);   // bivec — by the 2nd exterior power (cofactor in 3D), NOT the linear part
// ctors: default, splat(T), {init,list}, make_from_values(... == num_components)
```

```cpp
#include <typed-geometry/linalg/cross.hh>
tg::cross(a, b);                          // bivec3 — wedge of two vec3 (components {yz, zx, xy})
tg::dual(biv);                            // vec3   — Hodge dual; dual(cross(a,b)) == classic a x b
tg::undual(v);                            // bivec3 — inverse of dual
```

## angle — radian/degree-safe scalar

```cpp
#include <typed-geometry/scalar/angle.hh>
tg::angle_f a;                                  // default 0; tg::angle_f / tg::angle_d
tg::angle_f::make_from_radians(x);  tg::angle_f::make_from_degree(d);   // only ways to build
a.radians();   a.degree();                       // read back as T
a + b   a - b   -a   a * s   s * a   a / s        // 1D vector space; NO wrap-around; a == b
a.sin(); a.cos(); a.tan(); a.sin_cos(); a.sec(); a.csc(); a.cot();   // trig members (has_trigonometry)
using namespace tg::literals;  90_deg_f;  3.14_rad_d;  // _rad_f/_rad_d/_deg_f/_deg_d
// PREFER the literal over make_from_degree for constants: `60_deg_f`, not `angle_f::make_from_degree(60)`
```

## mat — column-major matrix

```cpp
#include <typed-geometry/linalg/mat.hh>
tg::mat3f m;                                    // default = ZERO (not identity)
tg::mat3f::zero;   tg::mat3f::identity;          // static constants
tg::mat3f::make_from_cols(c0, c1, c2);          // from C column vecs
m.col(c);                                        // vec<R,T>& — a real column reference
m[c, r];                                         // T& — multi-arg subscript (col, row). PARENS in macros!
m + n   m - n   m * s   s * m   m == n
m * v;                                           // vec<C> -> vec<R>
a * b;                                           // mat<C,R> * mat<K,C> -> mat<K,R>
// rotations (3x3, requires has_trigonometry<T>):
tg::mat3f::make_rotation_x(a);  ..._y(a);  ..._z(a);  ..._axis_angle(axis_vec3, a);
```

## quat — quaternion rotation

```cpp
#include <typed-geometry/linalg/quat.hh>
tg::quat_f q;                                   // default zero; data is {x,y,z,w} (w = scalar part)
tg::quat_f::zero;   tg::quat_f::identity;        // identity = (0,0,0,1)
tg::quat_f(x, y, z, w);                           // explicit ctor; q[i], q.data
tg::quat_f::make_rotation_x(a); ..._y; ..._z; ..._axis_angle(axis, a);  // requires has_trigonometry
tg::quat_f::make_from_basis(x_axis, y_axis, z_axis);  // rotation sending +x/+y/+z onto the given orthonormal axes; requires has_sqrt
q1 * q2;                                          // composition (applies q2 then q1)
q * v;                                            // rotate a vec3
q.length();  q.normalized();                      // requires has_sqrt;  q.length_sqr() always
q.axis();                                         // vec3 — unit axis (zero vec if no rotation), requires has_sqrt
q.angle();                                        // angle — requires has_sqrt + has_trigonometry
q.conjugate();                                    // inverse rotation for a unit quat
```

## mat_ops — square-matrix operations

```cpp
#include <typed-geometry/linalg/mat_ops.hh>
tg::transpose(m);                          // mat<R,C> — works for rectangular m too
tg::determinant(m);                        // T        — square only, N <= 4
tg::inverse(m);                            // mat      — zero matrix if m is singular
tg::adjugate(m);                           // mat      — adjugate(m) * m == determinant(m) * identity
tg::cofactor(m);                           // mat      — det(m) * transpose(inverse(m)), division-free
// N <= 4 because tg is 2D/3D: the linear part tops out at mat3, the homogeneous matrix at mat4.
// cofactor is what a NORMAL transforms by; adjugate stays defined for singular matrices.
```

## transform — one type over a capability lattice

```cpp
#include <typed-geometry/transform/transform_flags.hh>
tg::transform_flags;                       // enum class: translation, uniform_scaling,
                                           //   non_uniform_scaling, negative_scaling, rotation,
                                           //   general_linear, projection
tg::transform_class::identity;             // ... translation, uniform_scaling, scaling, rotation,
                                           //     rigid, scaled_rotation, similarity, linear, affine,
                                           //     scaling_translation, uniform_scaling_translation, projective
                                           // + signed_ variants that allow a NEGATIVE factor:
                                           //     signed_uniform_scaling, signed_scaling, signed_scaled_rotation,
                                           //     signed_scaling_translation, signed_uniform_scaling_translation,
                                           //     signed_similarity   (linear/affine/projective include it already)
tg::canonical(f);                          // transform_flags — the class representative (19 classes)
tg::is_canonical(f);                       // bool
tg::is_subclass(sub, super);               // bool — USE THIS, not has_all. See the Gotchas.
tg::has_any(f);  tg::has_all(f, part);  tg::without(f, x);  f | g;  f & g;  ~f
```

```cpp
#include <typed-geometry/transform/homogeneous_transform.hh>
tg::rigid_transform3f t;                   // default = IDENTITY (not zero); also 2f/2d/3d
tg::homogeneous_transform<DSource, DTarget, T, Flags>;  // the one type: a map DSource -> DTarget space
                                           // SQUARE ONLY today — it static_asserts DSource == DTarget.
                                           // The pair is what will make lifting/projecting typed.
t.source_dimension;  t.target_dimension;   // int
// aliases: identity_/translation_/rotation_/scaling_/scaling_translation_/linear_/
//          rigid_/similarity_/affine_/projective_transform<D,T>   (+ 2f/3f/2d/3d typedefs)
//          signed_scaling_/signed_scaling_translation_/signed_similarity_transform<D,T> too
//          — ALL square, so the second dimension almost never shows up at a call site
tg::rigid_transform3f::identity;           // static constant
tg::rigid_transform3f::make_translation(v);          // requires the class to contain translation
tg::similarity_transform3f::make_uniform_scaling(s);  // s must be POSITIVE; the factory asserts it
tg::scaling_transform3f::make_scaling(vec);
tg::rigid_transform3f::make_rotation(quat);           // D==3; make_rotation(angle) for D==2
tg::affine_transform3f::make_from_linear_mat(mat3);
tg::projective_transform3f::make_from_mat(mat4);      // projective only

t.translation();  t.rotation();  t.uniform_scale();  t.scale();   // gated on the class
                                           // translation() is in TARGET space; scale() indexes SOURCE axes
t.linear_mat();                            // mat<DSource,DTarget> — not for a projective transform
t.to_mat();                                // mat<DSource+1,DTarget+1> — translation in the last column
t.apply_linear(v);                         // vec<DSource> -> vec<DTarget> — the linear part only
t.apply_bivec(b);                          // bivec — the 2nd exterior power of the linear part
t.apply_pos(p);                            // pos<DSource> -> pos<DTarget> — linear part, translation, any divide
t.homogeneous_w(p);                        // T   — projective only; positive means "in front"
auto a = tg::affine_transform3f(rigid);    // widening: lossless but EXPLICIT, and only compiles if
                                           //   the source class really IS a member of the target one.
                                           //   That is also the dispatch mechanism — see obj.transformed
```

```cpp
p.transformed(t);                          // pos   — incl. the projective divide (asserts w != 0)
v.transformed(t);                          // vec   — NOT available for a projective transform
b.transformed(t);                          // bivec — by the 2nd exterior power, i.e. cofactor in 3D
```

```cpp
a.composed(b);                             // applies b FIRST, then a — NO operator*
                                           // result class = canonical(FA | FB), so it can widen
                                           // opt-in per type; probe with requires { a.composed(b); }
#include <typed-geometry/transform/compose.hh>
tg::compose(a, b);                         // a.composed(b) if that exists, else tg::composed_transform<A,B>
                                           // — a compile-time choice, so the return type says which
tg::composed_transform<A, B>(outer, inner);  // stores both; applies `inner` first, then `outer`
                                           // composes ANY two transforms, at the cost of not fusing
#include <typed-geometry/transform/inverse.hh>
tg::inverse(t);                            // same class — every canonical class is closed under it
                                           // dimensions swap: <DSource,DTarget> inverts to <DTarget,DSource>
```

```cpp
obj.transformed(t);   // the return type depends on the object AND the transform class
t.transform(obj);     // the mirror spelling; routes straight back to obj.transformed(t)
t(obj);               // the call spelling of t.transform(obj) — application, NOT composition
// These three are the same value, by construction:
//   a(b(obj))  ==  a.composed(b).transform(obj)  ==  obj.transformed(b).transformed(a)
// Each object writes its own `if constexpr` chain, in its own header, asking which class it can
// widen the transform to; an unsupported pair is a static_assert. To probe, ask the same question:
//   requires { tg::scaling_translation_transform<D, T>(t); }   // == "an aabb accepts this transform"
// To special-case an object, a transform declares a PRIVATE custom_transform(ObjT const&) and
// befriends that object — the first branch every object checks. Access is part of the requires,
// so an object that was not befriended never sees it.
// `composed` is opt-in per transform type and IS probeable: requires { a.composed(b); }
```

## geometry primitives (each denotes a set of points)

```cpp
#include <typed-geometry/geometry/primitives/aabb.hh>      // and triangle/segment/ray/line/plane.hh
tg::aabb<D,T>     {pos min, max}              // solid box {x : min <= x <= max}              — finite
tg::triangle<D,T> {pos pos0, pos1, pos2}      // filled triangle (hull of 3 verts), 2D patch  — finite
tg::segment<D,T>  {pos pos0, pos1}            // {(1-t)*pos0 + t*pos1 : t in [0,1]}, 1D        — finite
tg::ray<D,T>      {pos origin; vec dir}       // {origin + t*dir : t >= 0}, 1D                 — infinite
tg::line<D,T>     {pos origin; vec dir}       // {origin + t*dir : t in R}, 1D                 — infinite
tg::plane<D,T>    {vec normal; T dist}        // hyperplane {x : dot(normal,x) == dist}        — infinite
tg::sphere<D,DA,T>    {pos center; T radius}          // SURFACE {x : distance(x,center) == radius}    — finite
tg::ellipsoid<D,DA,T> {pos center; vec semi_axes[D]}  // SURFACE {center + sum_i u_i*semi_axes[i] : |u| == 1} — finite
// sphere/ellipsoid take TWO dims: D = the flat the object curves in, DA = the space that flat sits in.
//   equal for the everyday case (sphere3f == sphere<3,3,f32>); apart when EMBEDDED: sphere2in3f is a circle in 3D.
//   ellipsoid ctor takes D axis vectors (or a vec[D] array): tg::ellipsoid3f(center, axis0, axis1, axis2).
//     the axes need not be orthogonal, and they span the flat — so the embedded case stores nothing extra.
//   sphere's {center,radius} does NOT pin down the plane, so what it stores depends on the pair: the PRIMARY
//     template is undefined and each pair is a specialization — sphere<D,D,T> is {center,radius},
//     sphere<2,3,T> adds the plane's normal: tg::sphere2in3f(center, radius, normal).
//     A pair with no specialization (a circle in 4D) is an incomplete type, not a silently wrong encoding.
// members are public + named (pos0/min/normal/…), not data[]; default-ctor zero-inits; explicit ctors;
//   defaulted operator==. No queries/measures/factories yet (representations still settling).
// dimensional aliases: aabb2/3, triangle2/3, …   concrete: aabb3f triangle3f segment2i ray3f plane3d
//   (aabb/triangle/segment get f/d/i; ray/line/plane/sphere/ellipsoid get f/d — they carry real values)
//   the embedded pair spells both dims: sphere2in3/ellipsoid2in3 (+ …2in3f / …2in3d)

obj.transformed(t);   // every primitive; which transforms it accepts is a geometric statement:
//   sphere              similarity -> sphere      |  affine -> ELLIPSOID (unless embedded: needs a basis of the flat)
//   ellipsoid           affine     -> ellipsoid   (embedded or not — the map is one of the ambient space)
//   aabb                scaling + translation ONLY (a rotated aabb needs obb, which does not exist)
//   triangle, segment   affine, projective
//   plane               affine, projective        (normal picks up the cofactor, not the linear part)
//   ray, line           affine ONLY               (a projected ray is a bounded segment)
```

## object_traits (point-set classification seam)

```cpp
#include <typed-geometry/geometry/traits.hh>
tg::object_traits<ObjT>;                   // specialize per object type (in its own header)
tg::traits::intrinsic_dim<ObjT>;           // int  — manifold dim of the set (triangle3f -> 2)
tg::traits::ambient_dim<ObjT>;             // int  — dim of the surrounding space (triangle3f -> 3)
tg::traits::is_finite<ObjT>;               // bool — is the point set bounded? (triangle yes, plane no)
// intrinsic_dim <= ambient_dim; plane is codimension 1. The primary template is undefined on purpose:
//   a type that forgets to specialize it is a compile error, not a silent default.
```

## scalar traits (extensibility seam)

```cpp
#include <typed-geometry/scalar/scalar.hh>   // pulls in scalar/traits.hh + constants.hh
tg::scalar_traits<T>;                     // specialize this to teach tg about a new scalar type
tg::traits::has_sqrt<T>;  tg::traits::has_trigonometry<T>;   // inline constexpr bool flags
tg::traits::is_zero(x);  tg::traits::is_one(x);   // bool — routed through the trait (symbolic-friendly)
tg::one<T>();                             // T   — multiplicative identity (always)
tg::sqrt(x);                              // T   — requires has_sqrt<T>
tg::sin(a); tg::cos(a); tg::tan(a);       // angle<T> -> T          — requires has_trigonometry<T>
tg::sec(a); tg::csc(a); tg::cot(a);       // angle<T> -> T          — reciprocals (free == member a.sin()…)
tg::sin_cos(a);                           // angle<T> -> cc::pair<T,T> {sin, cos}
tg::asin(x); tg::acos(x); tg::atan(x);    // T -> angle<T>          — inverse trig
tg::atan2(y, x);                          // (T, T) -> angle<T>     — requires has_trigonometry<T>
tg::pi<T>;                                // inline constexpr T  (scalar/constants.hh)
// scalars: f32/f64 have sqrt+trigonometry (via std:: — see docs/TODO.md); all integer types except
// plain `char` get one/is_zero/is_one (signed/unsigned char count; `char` does not); bool is special.
```

## Umbrellas

```cpp
#include <typed-geometry/linalg/linalg.hh>       // curated: vec/pos/comp/bivec/mat/quat + ops
#include <typed-geometry/linalg/all.hh>          // everything in linalg
#include <typed-geometry/transform/transform.hh> // curated: the transform type + its operations
#include <typed-geometry/geometry/geometry.hh>   // curated: object_traits + primitives
#include <typed-geometry/geometry/all.hh>        // everything in geometry
#include <typed-geometry/all.hh>                 // everything; expensive
```

## Gotchas

- **No `.x/.y/.z`** — by design.
  Use `data[i]` or `operator[]`.
- **Constructors are `explicit`.** `tg::vec3f v = {1,2,3};` does not compile; use `tg::vec3f(1,2,3)` or `tg::vec3f({1,2,3})`.
- **`length()`/`normalized()`/`distance()`/`tg::sqrt` need `has_sqrt<T>`** — they don't exist for `vec3i` etc. Use `length_sqr()` / `distance_sqr()` for integers.
- **`normalized()` does NOT assert on zero** — it returns the zero vector/quaternion (a hard assert here caused too many spurious failures in practice). Check `tg::traits::is_zero(v.length())` yourself if you need to distinguish.
- **Out-of-range `operator[]` and wrong-size initializer lists `CC_ASSERT`** (active in debug/relwithdebinfo, stripped in release).
- Types are **trivially copyable**; default construction **zero-initializes** the components.
- **Factories are `make_*`** (`make_from_values`, `make_unit`, `make_rotation_z`, …). Distinguished values are static constants (`vec::zero`, `mat::identity`, …) — runtime consts, not `constexpr`.
- **`mat`'s multi-arg `m[c, r]` needs parentheses inside macros**: `CHECK((m[0,0]) == 1)`, else the comma is read as a macro-argument separator.
- **`mat` default is the ZERO matrix, not identity** — use `tg::matNf::identity`. A **transform**, by contrast, defaults to the **identity**: a zero-filled transform would be singular.
- **Transform containment is `tg::is_subclass`, NEVER `tg::has_all`.** `canonical()` clears bits — `affine` drops `uniform_scaling` because `non_uniform_scaling` subsumes it — so `has_all(affine, similarity)` is `false` even though every similarity is affine.
- **Widening a transform is explicit**: `tg::affine_transform3f(rigid)`, not an implicit conversion.
  An implicit one would make two registrations at different classes an ambiguous overload set.
  Narrowing is not a constructor at all.
- **A normal is a `bivec`, not a `vec`.** It transforms by the cofactor matrix, not the linear part — the difference only shows up under a non-uniform scaling, which is what makes it a silent bug.
- **`obj.transformed(t)` on an unsupported pair is a compile error on purpose** (a rotated `aabb` is not an `aabb`). It is not probeable — the return type is `auto`, so asking trips the `static_assert`; test the branch condition (`requires { tg::affine_transform<D, T>(t); }`) instead.
- **Transform scale factors are POSITIVE** unless the class carries `negative_scaling` (`tg::signed_scaling_transform3f`, `signed_similarity_transform3f`, …); the factories assert it.
  `linear`/`affine`/`projective` include it by nature.
- **A transform has TWO dimension parameters** (`homogeneous_transform<DSource, DTarget, T, Flags>`), so lifting and projecting can be typed.
  Only the square case is implemented — it `static_assert`s `DSource == DTarget` — and every alias is square, so you rarely see it.
- **`composed` can widen the class**: a rotation composed with a per-axis scaling is a general linear map, not "a rotation and a scaling".
- **`tg::compose` is total, `composed` is not.** `composed` exists only where two transforms can fuse; `compose` falls back to a `composed_transform` that just holds both.
  Prefer `compose` unless you specifically want the fused transform or nothing.
- **There is no `operator*` on transforms** — composition is `a.composed(b)` (or `tg::compose(a, b)`).
  A transform is applied, not multiplied, and `*` would invite a `t * p` that deliberately does not exist.
- **`t(obj)` applies, it does not compose.** `t(u)` for a transform `u` is a compile error on purpose; write `t.composed(u)`.
