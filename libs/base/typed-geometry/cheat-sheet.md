# typed-geometry cheat sheet

Strongly-typed C++23 math & geometry. Namespace `tg`. Depends on clean-core. Headers are included
by full path from `src/`: `#include <typed-geometry/<module>/<name>.hh>`.

> **Scope note:** this single sheet covers the small surface that exists today (`scalar` + the
> `linalg` core). As the library grows we will likely split it into per-module cheat sheets
> (one per larger module) — the eventual API surface is far too large for one file. For the
> *why*, read the header `///` docs and [docs/structure.md](docs/structure.md).

How to read this: each block leads with the include; one symbol per line with a trailing comment
giving the return type / intuition. Format conventions live in
[docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

---

## Types & typedefs

```cpp
#include <typed-geometry/fwd.hh>          // forward decls + all aliases
tg::vec<D, T>  tg::pos<D, T>  tg::comp<D, T>  tg::bivec<D, T>   // generic over dimension D
tg::mat<C, R, T>   tg::quat<T>   tg::angle<T>                   // matrix / quaternion / angle
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
// ctors: default, splat(T), {init,list}, make_from_values(... == num_components)
```

```cpp
#include <typed-geometry/linalg/cross.hh>
tg::cross(a, b);                          // bivec3 — wedge of two vec3 (components {xy, yz, zx})
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
q1 * q2;                                          // composition (applies q2 then q1)
q * v;                                            // rotate a vec3
q.length();  q.normalized();                      // requires has_sqrt;  q.length_sqr() always
q.axis();                                         // vec3 — unit axis (zero vec if no rotation), requires has_sqrt
q.angle();                                        // angle — requires has_sqrt + has_trigonometry
q.conjugate();                                    // inverse rotation for a unit quat
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
#include <typed-geometry/linalg/linalg.hh>   // curated: vec/pos/comp + ops
#include <typed-geometry/linalg/all.hh>      // everything in linalg
#include <typed-geometry/all.hh>             // everything (scalar + linalg); expensive
```

## Gotchas

- **No `.x/.y/.z`** — by design. Use `data[i]` or `operator[]`.
- **Constructors are `explicit`.** `tg::vec3f v = {1,2,3};` does not compile; use
  `tg::vec3f(1,2,3)` or `tg::vec3f({1,2,3})`.
- **`length()`/`normalized()`/`distance()`/`tg::sqrt` need `has_sqrt<T>`** — they don't exist for
  `vec3i` etc. Use `length_sqr()` / `distance_sqr()` for integers.
- **`normalized()` does NOT assert on zero** — it returns the zero vector/quaternion (a hard assert
  here caused too many spurious failures in practice). Check `tg::traits::is_zero(v.length())`
  yourself if you need to distinguish.
- **Out-of-range `operator[]` and wrong-size initializer lists `CC_ASSERT`** (active in
  debug/relwithdebinfo, stripped in release).
- Types are **trivially copyable**; default construction **zero-initializes** the components.
- **Factories are `make_*`** (`make_from_values`, `make_unit`, `make_rotation_z`, …). Distinguished
  values are static constants (`vec::zero`, `mat::identity`, …) — runtime consts, not `constexpr`.
- **`mat`'s multi-arg `m[c, r]` needs parentheses inside macros**: `CHECK((m[0,0]) == 1)`, else the
  comma is read as a macro-argument separator.
- **`mat` default is the ZERO matrix, not identity** — use `tg::matNf::identity`.
