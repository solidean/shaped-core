# What babel-serializer wants from the lower libraries

The repo rule is that a task best served by **extending a lower library** should say so rather than work around it silently
(see [CLAUDE.md](../../../../CLAUDE.md), *The libraries are living — surface missing pieces*).
This file is where babel-serializer keeps those findings: capabilities that belong in **clean-core** or **typed-geometry**,
which babel currently hand-rolls, approximates, or simply does not offer because the piece is missing.

It is a wish list with receipts, not a roadmap — none of these blocks a format from landing.
Each entry names what is wanted, why babel wants it, and what the code does today instead.
**Retire an entry in the same change that lands the capability**, and delete the workaround it describes.

An entry graduates onto this list when a workaround is *general* — a second format would hand-roll the same thing.
A one-off quirk of one format belongs in a comment next to the code, not here.

---

## typed-geometry

### Transform builders: quaternion → matrix, translation, scaling, TRS composition

**Wanted:** `tg::mat<3, 3, T>::make_from_quat(tg::quat<T>)`, `tg::mat<4, 4, T>::make_translation(tg::vec<3, T>)`,
`make_scaling(tg::vec<3, T>)`, and a `make_from_trs(translation, rotation, scale)` that composes the three in the
conventional order.

**Why:** glTF is the direct case.
A node's local transform is *either* a 4×4 `matrix` *or* a translation / rotation / scale triple, mutually exclusive per the spec.
So a consumer walking the node hierarchy has to convert the TRS form to a matrix before it can multiply anything.
That conversion is pure typed-geometry work: a quaternion, a translation and a scale to one matrix, with the composition order fixed once instead of per caller.
`.gltf` files in the wild use both forms freely, so this is not an edge case.

**Today:** [linalg/mat.hh](../../../base/typed-geometry/src/typed-geometry/linalg/mat.hh) offers only `make_from_cols`,
`make_rotation_x/y/z`, `make_rotation_axis_angle` (3×3 only), `zero` and `identity` — nothing that builds a translation, a
scale, or a matrix from a quaternion.
So `babel::gltf::node` stores whichever form the file used (`has_matrix` says which) and the reader composes **nothing**.
That is a defensible native-structure decision on its own — the reader is faithful to the file — but the *absence of a
`local_transform(node)` helper* is the gap, not the design: writing one would mean hand-rolling quaternion→matrix math inside
babel, which is exactly the silent workaround the repo rule forbids.

### An affine transform type, and `mat4 * pos3` / `mat4 * vec3`

**Wanted:** the transform type typed-geometry's roadmap already anticipates, plus the affine products that distinguish a
**point** (translated) from a **direction** (not translated).

**Why:** it is the other half of the same job.
Once a glTF node's local transform exists, composing parent × child down the hierarchy and applying the result to positions and normals is the whole of placing a mesh in a scene.
`mat4f * vec4f` alone pushes every caller into homogeneous-coordinate bookkeeping that the type system should be doing.

**Today:** `tg::mat` multiplies `mat<C,R> * vec<C>` only, so there is no affine application at all.
babel does not attempt it — the node hierarchy is handed back as parent/child indices and left un-flattened.

### `tg::aabb` factories, so stated bounds can come back typed

**Wanted:** an `aabb` factory from a min / max pair (and eventually from a point range), per
[typed-geometry's cheat sheet](../../../base/typed-geometry/cheat-sheet.md), which notes the primitives have "no
queries/measures/factories yet".

**Why:** a glTF accessor may state `min` / `max`, and for `POSITION` that is a bounding box the caller gets **without touching
a single buffer byte** — the cheapest useful thing in the format (fit a camera, cull, size a scene). It wants to be a
`tg::aabb3f`.

**Today:** `data::min_of` / `max_of` hand back `cc::span<f32 const>` of `component_count()` floats.
Honest and dimension-agnostic, since bounds exist for `VEC2` and `MAT4` accessors too — but untyped, so the caller assembles the box.

### `tg::mesh`, for the `load_mesh` aggregator

**Wanted:** the mesh type on typed-geometry's roadmap.

**Why:** it is the one thing standing between babel and its second aggregator.

**Today:** `[planned]` in [structure.md](structure.md), which carries the rest; the format readers hand back their native structures.

---

## clean-core

### A memory-mapped file behind a `cc::pinned_data`

**Wanted:** an mmap-backed pin — a `cc::pinned_data<byte const>` over a mapped file, with the unmap living in the pin's deleter.

**Why:** this is the piece that would make babel's zero-copy story end-to-end.
`babel::gltf::read` already returns every embedded buffer as a subview of its input pin, so a 200 MB `.glb` never copies its vertex data *inside* babel.
Getting those bytes in still costs one full read into memory, because a file is only reachable as a stream today.
With a mapped pin, "load a glTF" would touch the vertex bytes only when the caller reads them.

**Today:** nothing in the repo maps a file (no `mmap` / `MapViewOfFile` wrapper anywhere), so the composition is
`file_read_stream_adapter::open` → `read_all()` → `cc::make_pinned_data(cc::move(bytes))` — one precise allocation and one copy.
The seam is already the right shape: `pinned_data::create_from_pin(span, std::shared_ptr<void const>)` accepts any deleter, so
a mapped pin drops in without touching a single babel signature.

### A fallible `pinned_data::subdata`

**Wanted:** `try_subdata(cc::offset_size) -> cc::optional<pinned_data>` (and the `start_end` / `isize` twins).

**Why:** slicing a pin from *file-supplied* offsets is the normal case for a binary format, and a bad offset is input to be
rejected, not a bug to assert on.
[pinned_data.hh](../../../base/clean-core/src/clean-core/container/pinned_data.hh) gives two choices and neither is a validation
channel: `subdata` **asserts** (and the assert is compiled out in `release-*`, which is the worst possible outcome for
attacker-supplied bytes), while `subdata_clamped` silently truncates.

**Today:** every slice site in [gltf.cc](../src/babel-serializer/geometry/gltf.cc) is preceded by a hand-written bounds check
that produces a `cc::error` — the GLB chunk walk, the buffer trim, the bufferView-in-buffer and accessor-in-bufferView
validation pass, and `view_of` (which goes through `span::is_subspan` first, purely to get a fallible answer).
A `try_subdata` would delete that whole class of boilerplate, here and in every future binary format.

### `trim` and a whitespace `split` on `cc::string_view`

**Wanted:** `cc::string_view::trimmed()` (leading and trailing `cc::is_space` removed) and a whitespace `split`
that yields the tokens of a view — either as a range or into a caller's `cc::vector<cc::string_view>`.

**Why:** every ASCII-headered format walks the same two operations.
Radiance HDR splits its `-Y h +X w` resolution line and trims both halves of each `KEY=value` line; PFM walks its
five-token header one token at a time; a future PPM/PGM, OBJ material library or `.mtl` reader wants exactly this.
None of it is format knowledge — it is string handling that clean-core does not offer yet.

**Today:** `hdr.cc` hand-rolls `trimmed` and `split_tokens`, and `pfm.cc` hand-rolls `next_token`, each marked with
a one-line comment saying `cc` has no equivalent.
`cc::is_space` from [char_predicates.hh](../../../base/clean-core/src/clean-core/string/char_predicates.hh) already
does the classification, so only the iteration is missing.
A line iterator on `cc::string_view` would retire `hdr.cc`'s `next_line` too, though that one also strips a `\r`,
which is arguably the caller's business.

### A pinned strided view

**Wanted:** `pinned_data::as_strided<T>(stride)`, or a `cc::pinned_strided_data<T>` pairing a `strided_span` with a pin.

**Why:** `cc::strided_span` is exactly the right view over an interleaved vertex buffer, but it is non-owning — handing one out
drops the pin, and with it the guarantee that made the zero-copy read worth doing.

**Today:** `babel::gltf::accessor_view` hand-rolls `pinned_data + stride + count + element_size` and exposes
`as_strided<T>()` as a non-owning view tied to its own lifetime.
**Low priority**: the hand-rolled struct also carries the accessor's `component` / `type` / `normalized`, so it is more
expressive than a generic pinned strided span would be, and it is not obvious a second caller wants the generic form.

---

## Not gaps

Recorded so they do not get re-raised:

- **base64** is a serialization codec, so it belongs in `babel::base64` (`data/base64.hh`), not clean-core.
  [coding-guidelines.md](coding-guidelines.md) has the rule, and `gltf::read_options::resolve_uri` is the seam.
- **The power-of-two scalar pair** was a gap and is now `tg::pow2_by_int` / `tg::scale_by_pow2` / `tg::exponent_of` / `tg::split_pow2`.
  `babel::hdr` consumes them; the entry is retired rather than kept as history.
