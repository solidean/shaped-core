# typed-geometry cheat sheet

Strongly-typed C++23 math & geometry, namespace `tg`, depending on clean-core.
Headers are included by full path from `src/`: `#include <typed-geometry/<module>/<name>.hh>`.

> **Scope note:** one sheet still covers the whole surface that exists today — `scalar`, `linalg`, `transform` and the `geometry` primitives.
> As the library grows this will likely split into per-module sheets, since the eventual API is far too large for one file.
> For the *why* behind a decision, read the header `///` docs and [docs/_index.md](docs/_index.md).

How to read this: each block leads with the include, then one symbol per line with a trailing comment giving the return type or intuition.
Format conventions live in [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

---

**Recording domain:** `tg`.
Every `CC_LOG_*` and `CC_RECORD_*` site in this library is attributed to it; see [logging](../../base/clean-core/docs/logging.md).

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
tg::f16                                                // tg::half_float; no linalg aliases, write tg::vec<3, tg::f16>
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
v.normalized();                            // vec — requires has_sqrt<T>; returns zero when traits::is_zero(length())
v.transformed(t);                          // vec — the LINEAR part only; no projective transform

a + b   a - b   -a   a * s   s * a   a / s     // vec arithmetic (s is a scalar T)
a += b  a -= b  a *= s  a /= s
a == b  a != b                             // component-wise
```

```cpp
#include <typed-geometry/linalg/vec_ops.hh>
tg::dot(a, b);                             // T   — dot product
tg::normalize(v);                          // vec — free form of v.normalized() (requires has_sqrt<T>)
tg::any_orthogonal(v);                     // vec — perpendicular to v, any length (2D: rotated, 3D: smallest component zeroed)
tg::orthonormal_basis(n);                  // pair<vec3, vec3> {u, w}: (u, w, n) right-handed orthonormal; n MUST be unit
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
#include <typed-geometry/geometry/query/distance.hh>   // a pos is a geometric object; see "geometric queries"
p.distance_sqr_to(q);                      // T  — squared distance (any scalar)
p.distance_to(q);                          // T  — requires has_sqrt<T>
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
a + b   a - b   -a   a * s   s * a   a / s        // 1D vector space; NO wrap-around
a += b; a -= b; a *= s; a /= s;                   // compound forms of the same
a == b   a < b   a <=> b                          // ordered by the value: 370_deg_f > 10_deg_f, never equal to it
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

m.transposed();                                  // mat<R,C> — works for rectangular m too
m.determinant();                                 // T   — square only
m.inverse();                                     // mat — zero matrix if m is singular
m.adjugate();                                    // mat — m.adjugate() * m == m.determinant() * identity
m.cofactor();                                    // mat — det(m) * m.inverse().transposed(), division-free
// determinant/adjugate/cofactor/inverse are written out to 4x4 only; larger N is not out of scope,
// the expansions are simply not there yet and a bigger matrix static_asserts.
// cofactor is what a NORMAL transforms by; adjugate stays defined for singular matrices.
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
q.to_rotation_matrix();                           // mat3 — q must be UNIT; a non-unit one folds |q|^2 in
```

## transform — one type over a capability lattice

```cpp
// The flag machinery lives in tg::impl and is NOT how you name a class — use the aliases below.
// tg::impl::transform_flag         enum class: translation, uniform_scaling, non_uniform_scaling,
//                                  negative_scaling, rotation, general_linear, projection
//                                  Plain and INDEXED — no `none` / `all` value; the empty set is {},
//                                  the full one is tg::impl::transform_flag_all.
// tg::impl::transform_class::rigid ... identity, translation, uniform_scaling, scaling, rotation,
//                                  scaled_rotation, similarity, linear, affine, scaling_translation,
//                                  uniform_scaling_translation, projective, and signed_ variants
// tg::impl::transform_canonical(f) / transform_is_canonical(f) / transform_is_subclass(sub, super)
// tg::impl::transform_flags       = cc::flags<transform_flag>, the SET type: f.has_any / f.has_all / f.without.
// transform_class::* and the Flags template argument are that type, NOT the bare enum.
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
auto a = tg::affine_transform3f(rigid);    // widening: lossless but EXPLICIT, and only compiles if
                                           //   the source class really IS a member of the target one.
                                           //   That is also the dispatch mechanism — see obj.transformed
tg::impl::transform_representation_of(t);  // the members themselves — the back door for an object that
                                           //   can beat the accessors. Layout follows the class, so branch
                                           //   on tg::impl::linear_part(Flags) exactly as the transform does.
```

```cpp
t.transform(v);  t(v);                     // vec<DSource> -> vec<DTarget> — the linear part only
t.transform(b);  t(b);                     // bivec — the 2nd exterior power of the linear part
t.transform(p);  t(p);                     // pos<DSource> -> pos<DTarget> — linear part, translation, any divide
p.transformed(t);                          // pos   — delegates to t.transform(p); asserts w != 0
v.transformed(t);                          // vec   — NOT available for a projective transform
b.transformed(t);                          // bivec — by the 2nd exterior power, i.e. cofactor in 3D
// vec, pos and bivec are applied BY the transform: only it knows whether its linear part is a
// quaternion, a scalar or a matrix. Their transformed() is a bare delegation — no custom_transform
// probe, since the transform already owns the answer. A non-tg transform answers them the same way.
```

```cpp
a.composed(b);                             // applies b FIRST, then a — NO operator*
                                           // result class = the join of FA and FB, so it can widen
                                           // opt-in per type; probe with requires { a.composed(b); }
#include <typed-geometry/transform/compose.hh>
tg::compose(a, b);                         // a.composed(b) if that exists, else tg::composed_transform<A,B>
                                           // — a compile-time choice, so the return type says which
tg::composed_transform<A, B>(outer, inner);  // stores both; applies `inner` first, then `outer`
                                           // composes ANY two transforms, at the cost of not fusing
t.inverse();                               // same class — every canonical class is closed under it
                                           // dimensions swap: <DSource,DTarget> inverts to <DTarget,DSource>
```

```cpp
obj.transformed(t);   // the return type depends on the object AND the transform class
t.transform(obj);     // the mirror spelling; routes back to obj.transformed(t) for everything but vec/pos/bivec
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
#include <typed-geometry/geometry/primitives/aabb.hh>      // and box/triangle/segment/ray/line/plane/halfspace.hh
tg::aabb<D,T>     {pos min, max}              // solid box {x : min <= x <= max}              — finite
tg::aabb_boundary<D,T>                        // its faces; aabb3f_surface
tg::box<D,DA,T>   {pos center; mat<D,DA> half_extents}  // {center + H*c : c in [-1,1]^D}; columns = half-axes,
                                              //   NOT necessarily orthogonal (any affine image of a box is a box)
tg::box_boundary<D,DA,T>                      // its faces; box3f_surface; box2in3f is a rectangle in 3D
tg::halfspace<D,T> {vec normal; T dist}       // {x : dot(normal,x) <= dist}; h.boundary() is the plane
tg::capsule<D,T>  {segment axis; T radius}    // within radius of the axis (2D: stadium); capsule_boundary
tg::cylinder<3,T> {segment axis; T radius}    // flat caps; cylinder_boundary (all of it), cylinder_mantle = tube3
                                              //   (open tube, NOT a boundary: .solid() but no round trip); .caps()
tg::cone<3,T>     {pos apex; vec axis; T radius}  // axis: apex -> base center (|axis| = height); cone_boundary, cone_mantle
tg::hemisphere<3,T> {pos center; T radius; vec normal}  // the half ball the normal points into; _boundary, _mantle (dome)
tg::tetrahedron<3,T> {pos pos0..pos3}         // solid hull; tetrahedron_boundary; .faces()[i] opposite vertex i
tg::quad<D,T>     {pos00, pos10, pos11, pos01}  // BILINEAR patch (need not be planar): at(comp2), bounds, edges,
                                              //   ray crossings; no area / support / sampling (not convex, not flat)
tg::inf_cylinder<D,T> {line axis; T radius}  // unbounded tube (2D: a slab); _boundary. cylinder.unbounded() gives one
tg::inf_cone<D,T> {pos apex; vec dir; angle opening_angle}  // single nappe, dir unit, opening < 180deg; _boundary
tg::frustum<3,T>  {plane planes[6]}           // left right bottom top near far, normals OUTWARD (inside: <= dist);
                                              //   .vertices() 8 corners (bit0 right, bit1 top, bit2 far); _boundary
tg::frustum3d::make_from_view_projection(vp); // REVERSE-Z [0,1] (near -> 1); infinite far -> absent far plane
f.has_far_plane();                            // false: planes[5] is {0, 0}; plane-only queries still work,
                                              //   vertices / volume / bounds / sampling / GJK need a far plane
tg::triangle<D,T> {pos pos0, pos1, pos2}      // filled triangle (hull of 3 verts), 2D patch  — finite
tg::segment<D,T>  {pos pos0, pos1}            // {(1-t)*pos0 + t*pos1 : t in [0,1]}, 1D        — finite
tg::ray<D,T>      {pos origin; vec dir}       // {origin + t*dir : t >= 0}, 1D                 — infinite
tg::line<D,T>     {pos origin; vec dir}       // {origin + t*dir : t in R}, 1D                 — infinite
tg::plane<D,T>    {vec normal; T dist}        // hyperplane {x : dot(normal,x) == dist}        — infinite
tg::sphere<D,DA,T>    {pos center; T radius}          // SOLID ball {x : distance(x,center) <= radius}  — finite
tg::ellipsoid<D,DA,T> {pos center; vec semi_axes[D]}  // SOLID {center + sum_i u_i*semi_axes[i] : |u| <= 1} — finite
tg::sphere_boundary<D,DA,T>, tg::ellipsoid_boundary<D,DA,T>  // the SURFACE: same storage, == instead of <=
// an object type is MAXIMAL: the plain name is the solid. Its boundary is a separate type, never implicit:
//   s.boundary() -> sphere3f_surface;  b.solid() -> sphere3f.   3D also spells it _surface (sphere3f_surface).
//   a boundary transforms as its solid does: b.transformed(t) == b.solid().transformed(t).boundary()
// sphere/ellipsoid take TWO dims: D = the flat the object curves in, DA = the space that flat sits in.
//   equal for the everyday case (sphere3f == sphere<3,3,f32>); apart when EMBEDDED: disk3f (= sphere2in3f) is a
//   disk in 3D, circle3f (= sphere2in3f_boundary) its rim.
//   ellipsoid ctor takes D axis vectors (or a vec[D] array): tg::ellipsoid3f(center, axis0, axis1, axis2).
//     the axes need not be orthogonal, and they span the flat — so the embedded case stores nothing extra.
//   sphere's {center,radius} does NOT pin down the plane, so what it stores depends on the pair: the PRIMARY
//     template is undefined and each pair is a specialization — sphere<D,D,T> is {center,radius},
//     sphere<2,3,T> adds the plane's normal: tg::disk3f(center, radius, normal).
//     A pair with no specialization (a disk in 4D) is an incomplete type, not a silently wrong encoding.
// members are public + named (pos0/min/normal/…), not data[]; default-ctor zero-inits; explicit ctors;
//   defaulted operator==. Queries are MEMBERS (a.intersects(b), p.distance_to(seg)) — being built,
//   see docs/plans/geometry-query-matrix.md.
// dimensional aliases: aabb2/3, triangle2/3, …   concrete: aabb3f triangle3f segment2i ray3f plane3d
//   (aabb/triangle/segment get f/d/i; ray/line/plane/sphere/ellipsoid get f/d — they carry real values)
//   the embedded pair spells both dims: sphere2in3/ellipsoid2in3 (+ …2in3f / …2in3d), and disk3/circle3
//   boundaries: sphere3f_boundary == sphere3f_surface, sphere2f_boundary, ellipsoid3d_surface, circle3f, …

obj.transformed(t);   // every primitive; which transforms it accepts is a geometric statement:
//   sphere              similarity -> sphere      |  affine -> ELLIPSOID (embedded too: disk3 -> ellipsoid2in3)
//   *_boundary          whatever its solid becomes, then .boundary()
//   ellipsoid           affine     -> ellipsoid   (embedded or not — the map is one of the ambient space)
//   aabb                scaling + translation -> aabb  |  affine -> BOX (never a silently enlarged aabb)
//   box                 affine
//   halfspace           whatever its plane accepts
//   triangle, segment   affine, projective
//   plane               affine, projective        (normal picks up the cofactor, not the linear part)
//   ray, line           affine ONLY               (a projected ray is a bounded segment)
```

## unary members (per type, inline)

```cpp
seg.length();  tri.area();  tri.perimeter();  box.volume();   // by intrinsic dim: 1 length, 2 area + perimeter, 3 volume
aabb3.area();  sphere3.area();          // a 3D SOLID's area() is its surface; a boundary answers only its own measure
                                        //   (sphere3f_surface has area(), no volume(); aabb2_boundary has length())
o.centroid();  o.bounds();              // pos; aabb<D,T>
o.vertices();  o.edges();               // cc::fixed_array — segment, triangle, aabb (2^D / D*2^(D-1)), box
tri.normal();  tri.plane();             // 3D, unit, counter-clockwise; also box2in3.normal(), disk3.plane()
o.any_point();                          // a point of the set (for a surface ON it, not the center)
seg.unbounded();  ray.unbounded();      // the line through it
seg.at(t);  ray.at(t);  line.at(t);     // t: [0,1] / >= 0 / any
tri.at(comp3 bary);  aabb.at(comp [0,1]^D);  box.at(comp [-1,1]^D)
o.parameter_of(p);                      // inverse of at; segment/ray/line: of p's projection (clamped),
                                        //   triangle/aabb/box: unclamped (barycentrics go negative)
```

## sampling

```cpp
o.sample_uniform(rng);   // cc::random&; uniform over the object's point set — the TYPE says which set:
                         //   sphere3f the ball, sphere3f_surface the surface, aabb3f_surface the faces
// segment, triangle, aabb (+boundary), box (+boundary, 2D/3D), sphere (+boundary, flat), ellipsoid (solid)
// direct methods with a fixed draw count, except the ball/ellipsoid: rejection measured 2x faster
// also disk3 / circle3 (through an orthonormal basis of their plane); not yet ellipsoid_boundary (not a linear image)
```

## geometric queries (members; definitions per verb)

```cpp
#include <typed-geometry/geometry/query/query.hh>     // or one verb: query/distance.hh, query/project.hh, …
p.project_to(obj);          // obj's nearest point to p; for a solid, p itself when inside
a.closest_points_to(b);     // cc::pair{point of a, point of b}
a.closest_point_to(b);      // the point of a nearest b;  obj.closest_point_to(p) == p.project_to(obj)
a.distance_sqr_to(b);  a.distance_to(b);   // distance_to needs has_sqrt
p.signed_distance_to(obj);  // negative inside (plane: on the normal's far side)
a.contains(b);              // every point of b is in a — not symmetric
a.intersects(b);            // they share a point
l.intersection_parameter_with(b);          // l a line/ray/segment: tg::hits<N,T> against a SURFACE (sorted crossings,
                                           //   .has_any() .first() .last()), cc::optional<tg::hit_interval<T>> {start,
                                           //   end} against a SOLID (from inside, start is the ray's own 0)
l.closest_intersection_parameter_with(b);  // cc::optional<T>: the first crossing, or where l enters the solid
a.intersection_with(b);     // cc::optional<X>, X the generic-case shape: aabb∩aabb aabb, plane∩plane line,
                            //   triangle∩plane segment, ball∩plane disk3, sphere surfaces circle3; a linear object
                            //   gives its crossing pos (hits of pos) or the segment inside a BOUNDED solid
a.separation_from(b);       // cc::optional<tg::separation<D,T>> {normal, depth}: move b by normal*depth to stop
                            //   overlapping; empty when apart. Bounded convex SOLIDS only (EPA), 2D and 3D
a.may_intersect(b);         // culling: false only when certainly apart; frustum vs anything with a support is
                            //   plane by plane (cheap), every other pair falls back to the exact intersects
a.intersects(b, eps);  p_obj.contains(p, eps);  // bool: true if they meet, false beyond eps, either between;
                                                // default is the exact distance test (up to rounding)
// closed forms: aabb–aabb, ball–ball, ball–aabb, segment–segment, box–box (SAT); GJK measured 11–250x slower
// bounded convex objects with a support (pos, segment, triangle, aabb, sphere) get distance / closest points /
//   intersects against each other for free through GJK; closed forms take over where they exist
// a member used without its verb's header: "function with deduced return type cannot be used before it is defined"
// an unsupported pair: a static_assert naming the probe — tg::has_distance_sqr_to<A, B>, tg::has_intersects<A, B>, …
// kernels: tg::impl::<verb>_op<A, B> specializations; each verb also tries (B, A), then derives
//   (distance from closest points, closest points from a projection, contains/intersects for a pos from a projection)
// special cases are assumed away: NaN/inf propagate, nothing asserts. SC_CHECK_GEOMETRY_SPECIAL_CASES logs each one.
// exact scalars (tg::traits::is_exact: ints, bool, fixed_int) get only exact kernels: no projection onto a segment.
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
tg::traits::has_abs<T>;  tg::traits::has_exponential<T>;  tg::traits::has_rounding<T>;
tg::traits::has_pow2<T>;                  // the exact base-two family; f32/f64 only
tg::traits::is_zero(x);  tg::traits::is_one(x);   // bool — routed through the trait (symbolic-friendly)
tg::one<T>();                             // T   — multiplicative identity (always)
tg::sqrt(x);                              // T   — requires has_sqrt<T>
tg::abs(x);                               // T   — constexpr; requires has_abs<T> (integers too, not bool)
tg::sin(a); tg::cos(a); tg::tan(a);       // angle<T> -> T          — requires has_trigonometry<T>
tg::sec(a); tg::csc(a); tg::cot(a);       // angle<T> -> T          — reciprocals (free == member a.sin()…)
tg::sin_cos(a);                           // angle<T> -> cc::pair<T,T> {sin, cos}
tg::asin(x); tg::acos(x); tg::atan(x);    // T -> angle<T>          — inverse trig
tg::atan2(y, x);                          // (T, T) -> angle<T>     — requires has_trigonometry<T>
tg::pow(base, exp);                       // (T, T) -> T            — same T both sides; requires has_exponential<T>
tg::exp(x); tg::log(x);                   // T -> T                 — log needs x > 0
tg::round(x); tg::floor(x); tg::ceil(x);  // T -> T                 — requires has_rounding<T>; float-only
tg::pow2_by_int<T>(n);                    // int -> T               — 2^n exactly; requires has_pow2<T>
tg::scale_by_pow2(x, n);                  // (T, int) -> T          — x * 2^n exactly (C's ldexp); n must be integral
tg::exponent_of(x);                       // T -> int               — floor(log2(|x|)); x finite and non-zero
tg::split_pow2(x);                        // T -> tg::pow2_split<T> {significand, exponent}
tg::pi<T>;                                // inline constexpr T  (scalar/constants.hh)
// scalars: f32/f64 have the lot (via std:: — see docs/TODO.md); all integer types except plain `char`
// get one/is_zero/is_one + abs (signed/unsigned char count; `char` does not); bool is special.
// round/floor/ceil return T, not an int — narrow explicitly: int(tg::round(x)).
// tg::abs on the most negative integer is UB — that value has no representable magnitude.
// split_pow2's significand is in [1, 2), NOT C frexp's [0.5, 1): x == significand * 2^exponent, so
// exponent is floor(log2(|x|)). Porting frexp-shaped code means adjusting the exponent by one.
```

## fixed_int (wide two's-complement integers)

```cpp
#include <typed-geometry/scalar/fixed_int/fixed_int.hh>    // the types and their operators
#include <typed-geometry/scalar/fixed_int/fixed_arith.hh>  // + tg::add/sub/mul<R>, checked_*, the division family
tg::fi32; tg::fi64; tg::fi128; tg::fi192; tg::fi256;       // tg::fixed_int<Bits>: Bits is 32 or a multiple of 64
tg::fu32 … tg::fu256;                                      // tg::fixed_uint<Bits>
x.limbs[i];                                                // u64, least significant first (fi32/fu32: one u32)
a + b; a * b; a << n; a >> n; a / b; a <=> b;              // same type both sides; wrap modulo 2^Bits
fi128 x = 5; x + 1;                                        // a builtin converts implicitly when every value fits
fi192(x); x.widened<fi192>();                              // widen: explicit, lossless
x.truncated_to<fi64>();                                    // narrow: the low bits
x.shifted_left<fi192>(n);                                  // widen, then shift
fu128(x);                                                  // same width, other signedness: reinterpret
tg::mul<fi192>(a, b);                                      // fi128 x fi128 -> fi192: the exact result into R
tg::add<fi192>(a, b); tg::sub<R>(a, b);                    //   R is a claim: SC_CHECK_WIDE_ARITH checks it
tg::checked_mul<R>(a, b);                                  // -> cc::optional<R>; none when it does not fit
tg::div_trunc / mod_trunc / div_floor / mod_floor / div_ceil(a, b);  // same width; operators are trunc
tg::div_mod_trunc(a, b); tg::div_mod_floor(a, b);          // -> {quotient, remainder} from one division
tg::div_floor<fi32>(x, w); tg::div_ceil<fi32>(x, w);       // quotient known to fit fi32: udiv128 estimate + exact fix
tg::div_floor_ceil<fi32>(x, w);                            // -> {floor, ceil}
x.to_f64(); x.to_f32(); fi128(2.5);                        // correctly rounded out; truncating in
x.sign(); x.is_negative();                                 // int -1/0/+1 without a branch; bool
u.count_leading_zeroes(); u.popcount(); u.bit_width();     // fixed_uint only; s.magnitude_bit_width() for signed
x.to_string(); cc::format("{:'x}", x);                     // decimal; the full integer format spec
// fi192 r = a * b over fi128 does not compile: the product would wrap at 128 bits. Write tg::mul<fi192>(a, b).
// A shift amount must be in [0, Bits); min() / -1 wraps to min(); division by zero asserts.
// vec<3, fi64> exists, and its dot product is computed (and wraps) in fi64.
```

## half_float (binary16)

```cpp
#include <typed-geometry/scalar/half_float.hh>
tg::f16 h = tg::f16(0.1f);                        // tg::half_float; explicit, rounds to nearest even -> 0.0999755859375
tg::f16(0.1); tg::f16(3); 0.5_f16;                // from f64 and integers in one rounding; literal in tg::literals
f32(h); h.to_f32(); h.to_f64();                    // explicit, exact
tg::f16::make_from_bits(0x3c00); h.bits();         // the raw u16
h + h; h * h; h /= h; -h;                          // binary16 arithmetic: computed in f32, rounded once (IEEE-exact)
h == h; h < h; h <=> h;                            // on the bits; -0 == +0, NaN unordered -> std::partial_ordering
h.is_nan(); h.is_inf(); h.is_finite(); h.is_subnormal(); h.sign_bit();
tg::floor(h); tg::sqrt(h); tg::sin(a);             // every scalar_traits family; vec<3, tg::f16> works
tg::f16::max; lowest; min_normal; denorm_min; epsilon; infinity; quiet_nan;   // constexpr, unlike other tg constants
cc::format("{}", h);                               // shortest digits for f16: "0.1"; `{:.5f}` prints the exact value
// h + 1.0f does not compile: widen explicitly, f32(h) + 1.0f, or stay in f16, h + tg::f16(1).
// Each operation rounds, so a long chain in f16 drifts: widen once for heavy math.
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

- **No `.x/.y/.z`** — by design; use `data[i]` or `operator[]`.
- **Constructors are `explicit`.** `tg::vec3f v = {1,2,3};` does not compile; use `tg::vec3f(1,2,3)` or `tg::vec3f({1,2,3})`.
- **`length()`/`normalized()`/`distance()`/`tg::sqrt` need `has_sqrt<T>`** — they don't exist for `vec3i` etc.
  Use `length_sqr()` / `distance_sqr()` for integers.
- **`normalized()` does NOT assert on zero** — it returns the zero vector or quaternion.
  Check `tg::traits::is_zero(v.length())` yourself if you need to tell the cases apart.
- **Out-of-range `operator[]` and wrong-size initializer lists `CC_ASSERT`** (active in debug/relwithdebinfo, stripped in release).
- Types are **trivially copyable**; default construction **zero-initializes** the components.
- **Factories are `make_*`** (`make_from_values`, `make_unit`, `make_rotation_z`, …). Distinguished values are static constants (`vec::zero`, `mat::identity`, …) — runtime consts, not `constexpr`.
- **`mat`'s multi-arg `m[c, r]` needs parentheses inside macros**: `CHECK((m[0,0]) == 1)`, else the comma is read as a macro-argument separator.
- **`mat` default is the ZERO matrix, not identity** — use `tg::matNf::identity`. A **transform**, by contrast, defaults to the **identity**: a zero-filled transform would be singular.
- **Transform containment is `tg::impl::transform_is_subclass`, NEVER `has_all`.**
  `transform_canonical()` clears bits — `affine` drops `uniform_scaling` because `non_uniform_scaling` subsumes it.
  So `has_all(affine, similarity)` is `false` even though every similarity is affine.
  Reaching for either means you are in the flag machinery; day to day you name a class through its alias and let the widening constructor answer the containment question.
- **Widening a transform is explicit**: `tg::affine_transform3f(rigid)`, not an implicit conversion.
  An implicit one would make two registrations at different classes an ambiguous overload set.
  Narrowing is not a constructor at all.
- **A normal is a `bivec`, not a `vec`.** It transforms by the cofactor matrix, not the linear part — the difference only shows up under a non-uniform scaling, which is what makes it a silent bug.
- **`obj.transformed(t)` on an unsupported pair is a compile error on purpose** (a projected `ray` is not a `ray`); a rotated `aabb` is a `box`.
  It is not probeable — the return type is `auto`, so asking trips the `static_assert`.
  Test the branch condition (`requires { tg::affine_transform<D, T>(t); }`) instead.
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
