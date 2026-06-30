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
tg::vec<D, T>   tg::pos<D, T>   tg::comp<D, T>   // single generic type per family; D in {any > 0}
// dimensional alias templates: vec2/vec3/vec4, pos2/.., comp2/..   e.g. tg::vec3<T>
// concrete typedefs (suffix f=f32, d=f64, i=i32):
tg::vec2f tg::vec3f tg::vec4f  tg::vec2d.. tg::vec2i..   // + pos2f.., comp2f..
```

## vec — displacement / direction

```cpp
#include <typed-geometry/linalg/vec.hh>
tg::vec3f v;                              // default: zero-initialized {0,0,0}
auto a = tg::vec3f(2.0f);                 // splat -> {2,2,2}              (explicit)
auto b = tg::vec3f(1, 2, 3);             // per-dim ctor, requires D==2/3/4 (explicit)
auto c = tg::vec3f({1, 2, 3});           // initializer_list, CC_ASSERTs size == D (explicit)
auto d = tg::vec3f::from_values(1,2,3);  // variadic, requires sizeof...(args) == D

v.data;                                   // T[D] — the raw storage (public). NO .x/.y/.z
v[i];                                      // T& / T const& — CC_ASSERTs 0 <= i < D
v.length_sqr();                            // T   — sum of squares (any scalar)
v.length();                                // T   — requires has_sqrt<T>
v.normalized();                            // vec — requires has_sqrt<T>; CC_ASSERTs non-zero

a + b   a - b   -a   a * s   s * a   a / s     // vec arithmetic (s is a scalar T)
a += b  a -= b  a *= s  a /= s
a == b  a != b                             // component-wise
```

```cpp
#include <typed-geometry/linalg/vec_ops.hh>
tg::dot(a, b);                             // T   — dot product
tg::normalize(v);                          // vec — free form of v.normalized() (requires has_sqrt<T>)
// tg::cross(a, b) -> bivec3  is PLANNED (needs the bivec type)
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

## comp — neutral component container

```cpp
#include <typed-geometry/linalg/comp.hh>
tg::comp3f c;                            // zero-init; same ctor set as vec/pos
c.data;   c[i];                           // storage + indexed access
c == c2;                                  // component-wise
// NOTE: comp is the future home of all raw component-wise arithmetic — not implemented yet.
```

## scalar traits (extensibility seam)

```cpp
#include <typed-geometry/scalar/scalar.hh>   // pulls in scalar/traits.hh
tg::scalar_traits<T>;                     // specialize this to teach tg about a new scalar type
tg::scalar_traits<T>::has_sqrt;           // bool capability flag
tg::traits::has_sqrt<T>;                  // inline constexpr bool alias of the above
tg::sqrt(x);                              // T   — requires has_sqrt<T>; dispatches to the trait
// f32/f64 have has_sqrt=true (currently via std::sqrt — see docs/TODO.md); i32 has it false.
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
- **Out-of-range `operator[]` and wrong-size initializer lists `CC_ASSERT`** (active in
  debug/relwithdebinfo, stripped in release).
- Types are **trivially copyable**; default construction **zero-initializes** the components.
