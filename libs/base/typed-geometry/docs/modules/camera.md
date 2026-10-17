# camera

Projections, views, and the `view_projection` pair that projects into normalized device coordinates and back.
The module sits above `geometry/`, since unprojecting a pixel gives a `ray` and projecting a segment clips it.

## What belongs here

- **Projections**, view space to clip space, for every convention in common use: `tg::perspective`, `tg::orthographic` and their `_off_center` forms.
- **Views**, world space into the camera's frame: `tg::look_at` and `tg::look_to`.
- **`tg::view_projection`**, the two fused with their inverse and their depth convention, carrying `project` / `unproject` for points, segments, rays and lines.
- **Pixels**: `tg::ndc_from_pixel` and `tg::pixel_from_ndc`.

What does not: a camera *object* with a position, an orientation and a field of view.
That is application state, and the viewer's camera owns its own.

## The convention, and why it is the default

The unsuffixed functions are **left-handed, +y up, +z forward, D3D clip space, reverse-Z**.
That is the convention the repository already followed before this module existed:

- sv's camera and every example are left-handed with +z into the scene;
- sg normalizes clip space to D3D's on every backend — x and y in [-1, 1] with +y up, depth in [0, 1] — and absorbs vulkan's y-down with a negative viewport height;
- `tg::frustum::make_from_view_projection` reads reverse-Z: near maps to 1, far to 0.

Reverse-Z is the default because it spends a float's precision where depth needs it.
A float is densest near 0, and reverse-Z puts the far distance there, which roughly cancels the 1/z a perspective divide puts into depth.
It also makes an infinite far plane free, so the default `perspective` without a far distance is the precision sweet spot rather than a compromise.

## The zoo: every convention has an explicit name

`<kind>_<handedness>_<depth range>`, with handedness `lh` or `rh` and depth range `reverse_z`, `zero_to_one` or `minus_one_to_one`.
`perspective_rh_minus_one_to_one` is classic OpenGL, `perspective_lh_zero_to_one` classic D3D.
Each family carries the default's overload set, so nothing is reachable in one convention and missing in another.

The names are explicit rather than glm's `ZO` / `NO`, because the point of a zoo is that the call site says which one it is.
Two variants are left out on purpose: reverse-Z into [-1, 1], which nobody uses, and a y-down clip space, which sg already absorbs.

**A perspective function without `far_distance` puts the far plane at infinity.**
Arity rather than an `_infinite` suffix keeps the zoo at six names per kind, and the infinite case is the limit of the same formula rather than a different projection.

## The return type says what kind of map it is

| Function | Returns | |
|---|---|---|
| any perspective | `projective_transform<3, T>` | divides by w |
| an orthographic one keeping depth's direction | `scaling_translation_transform<3, T>` | an `aabb` stays an `aabb` under it |
| an orthographic one flipping it (reverse-Z left-handed, `[0,1]` and `[-1,1]` right-handed) | `signed_scaling_translation_transform<3, T>` | |
| `look_at` / `look_to` | `rigid_transform<3, T>` | inverts exactly and cheaply |

This is the transform module's lattice doing its job: composing an orthographic projection with a rigid view is affine, and pushing a box through it lands in the narrowest type that holds the answer.

## `view_projection`: the round trip is the contract

`project` takes world space to normalized device coordinates, `unproject` takes them back, and the pair is specified by its round trip:

- `unproject(project(p)) == p` for every point in front of the camera;
- `project(unproject(q)) == q` for every q in the depth range;
- `unproject(xy)` is the pick ray, starting on the near plane, and every point on it projects back onto `xy`;
- `unproject(*project(s))` is the part of a segment inside the view volume.

[view-projection-test.cc](../../tests/camera/view-projection-test.cc) states each as a property over random inputs.
It checks all six conventions, each with a finite, an infinite and an orthographic projection.

**A segment, a ray or a line projects to its visible part**, clipped against the view volume in homogeneous clip space.
That is what makes the answer well-defined: the clip volume is a bounded box, so whatever goes in, what comes out is a bounded segment or nothing.
In homogeneous space a point at infinity is an ordinary point with w = 0, so a ray running off to an infinite far plane ends at its vanishing point with no infinity anywhere in the arithmetic.

**The depth convention is passed, not read off the matrix.**
A matrix does not say which depth range it was built for, and `unproject`, the clip planes and `frustum()` all need to know.
The default needs no argument; a zoo projection's caller restates the convention its function name already spells.
The alternative was a projection type tagged with its convention, which rules out a mismatch.
It also wraps every projection, including the orthographic ones whose type is meant to be informative, so it waits until a mismatch bites.

**A raw projective transform does not clip.**
`segment::transformed` and `triangle::transformed` under a projective map assert that every vertex is in front of the projection.
A segment crossing w = 0 passes through infinity, and the segment between its mapped endpoints is the wrong half.
`view_projection::project` is the spelling that clips first.
