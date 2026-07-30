# babel-serializer structure (babel::)

The living roadmap for babel-serializer. Section headers carry a status tag:

- **[done]** — implemented and tested
- **[in progress]** — partially implemented
- **[planned]** — not started

Update the tags as formats land. This document is design intent, not a guarantee of final API.

## The shape of the library

Two layers, and everything here is about keeping them separate.

- **Format layer (native structures).** Each format parses into a data structure that resembles the format itself.
  It is unopinionated relative to the format: JSON stays a tree of values, OBJ stays parallel attribute arrays + face corners.
  No cross-format vocabulary is imposed at this layer.
- **Aggregator layer (opinionated).** On top of the format readers sit high-level entry points that dispatch across formats —
  "load an image", "load a mesh" — returning one convenient result regardless of the source format.
  None have landed yet.

Two more rules bind the whole library:

- **Read once, then query.** Reading is optimized for the read-once-into-a-basically-immutable-structure case.
  The parsed structure is cheap to traverse and query, deliberately awkward to mutate — there is no insertion API.
  Writing gets a *separate* API when it lands (a JSON writer will not reuse the reader's `document`).
- **Reading takes a `cc::read_stream`.** Readers parse against the stream's buffered window (`ready_bytes` / `consume` / `flush`);
  the buffering is inlined in the caller, so a byte-at-a-time parse is fast and nothing slurps the whole input first.
  A seekable stream is only requested if a format genuinely needs random access. string_view / span overloads wrap a `span_read_stream_adapter`.
  **The documented deviation is a format whose parsed result must hand back zero-copy views of the input** — `gltf` takes a
  `cc::pinned_data<byte const>` so every embedded buffer is a subview sharing the input's owner.
  Such a reader still offers stream / span overloads; they simply own a copy, and say so.

## Top-level structure

```text
src/babel-serializer/
  data/        [in progress]   text / structured data formats
    base64     [done]          codec (both alphabets; optional padding)
    json       [done]          reader
    markdown   [done]          block-level reader (no inline parsing)
    sqlite     [done]          live database engine (read/write; fetch-on-demand backend)
  geometry/    [in progress]   mesh / geometry formats
    gltf       [done]          glTF 2.0 + GLB reader over pinned bytes (zero-copy buffers)
    obj        [done]          reader
  image/       [in progress]   image formats (read + write; committed stb backend)
    png        [done]          low-level reader + writer (native IHDR fields; rich metadata [todo])
    jpg        [done]          low-level reader + writer (native SOF/JFIF fields; rich metadata [todo])
    image      [done]          aggregator: pixel buffer + format-dispatching read/encode/write
```

## data/ [in progress]

### base64 [done]

A plain codec, not a format reader — it has no `data` structure and no `read`, just four functions.

`decode` accepts **both** RFC 4648 alphabets (standard `+` / `/` and URL-safe `-` / `_`, even mixed in one input),
treats the `=` padding as optional, and skips ASCII whitespace between characters.
It fails on a character outside both alphabets, on a data character after padding, and on a final quantum of a single
character (which encodes no byte at all).
`decoded_size` is the same validation as an `optional<isize>`, `decode_into` writes into caller storage (a short buffer is an
error, not an assert), and `encode` always emits the standard alphabet with padding.

There is no streaming interface on purpose: base64 payloads in practice are data URIs and blobs embedded in a text format,
which the caller already holds whole in memory.
glTF's `data:` buffers are the first consumer.

### json [done]

Reader only. A parsed `document` is a **flat** structure, not a tree of allocating nodes:

- one `cc::vector<node>` in document order (root at index 0, preorder),
- one `cc::vector<i32>` of child indices (so a container's children are a contiguous range → O(1) random child access),
- one `cc::string` arena holding every string + key, unescaped once at parse time.

Traverse via the non-owning `ref` handle (`{document*, index}`); accessors are kind-tolerant (fallback / invalid ref on a mismatch).
`\uXXXX` escapes and surrogate pairs decode to UTF-8; an unpaired surrogate is an error.

Planned refinements:

- `[planned]` **writer** — a separate `babel::json::write` API with its own builder type (not the reader's `document`).
- `[planned]` **richer errors** — a `parse_error` with line/column (today: a `cc::result` message carrying the byte offset).
- `[planned]` **lossless numbers** — recover exact integers (today numbers are `double`); costs an arena copy of the raw slice.
- `[planned]` **compact node** — union the string / container payload fields (today they are separate for clarity).

### markdown [done]

Reader only, and **block level only** — the deliberate cut that makes a first version tractable.

The parsed `document` has exactly json's flat shape (preorder `node` array, contiguous child-index runs, one text arena)
and the same non-owning kind-tolerant `ref` handle, so knowing one reader is knowing the other.
Blocks covered: ATX headings, fenced code (with its info string), paragraphs with lazy continuation,
bullet + ordered lists (nested), block quotes, and thematic breaks.
Parsing is line-oriented over `read_stream::read_line`; every block records the 1-based source line it starts on,
which is what makes a markdown file usable as a test corpus that can point at a failing case.

**Inline spans are not parsed.** A paragraph's or heading's `text()` is the raw source, so `**bold**` comes back with its
asterisks. That is the single biggest cut, and it is why there is no `emphasis` / `link` / `code_span` node kind.

Markdown has no invalid input — every byte sequence is a valid document.
The `cc::result` is there for stream I/O failure and for consistency with the other readers, never for a content error.

Planned refinements:

- `[planned]` **inline parsing** — emphasis, links, code spans and images as child nodes under a paragraph.
- `[planned]` **setext headings** (`===` / `---` underlines), which need one line of lookahead against the thematic-break reading.
- `[planned]` **indented code blocks** — cut for now because a naive 4-space rule collides with list-item content indentation.
- `[planned]` **tables, HTML blocks, link reference definitions**, and exact tab-stop handling (tabs count as 4 columns today).

### sqlite [done]

The first format that breaks the two shapes above, and deliberately so.

- **Not a one-shot stream parser.** SQLite is a live database *engine*, not a byte format, so `babel::sqlite` is a thin
  RAII wrapper over an open connection you keep talking to — no `read(cc::read_stream&)`. Open a file (`open`),
  an existing file read-only (`open_readonly`), a transient `:memory:` db (`open_memory`), or a serialized byte image
  (`open_blob`, via `sqlite3_deserialize`); `serialize()` round-trips back to bytes.
- **Full read/write.** `exec` runs result-less SQL (DDL, INSERT/UPDATE/DELETE, PRAGMA, transactions);
  `prepare` / `query` return a move-only `statement` you bind parameters on and iterate as result `row`s with a range-for.
- **First third-party dependency, fetched on demand.** The engine backend is the vendored SQLite amalgamation under
  `extern/sqlite`, fetched (not committed, ~9.5 MB) the same way as Zydis / SDL3. `SC_SKIP_SQLITE` opts out.
- **Always-available API.** Because the backend may be absent, the whole `babel::sqlite` API is *always* declared and
  callable: when it was not compiled in, `is_available()` is false and every `open_*` returns a `cc::result` error.
  The compile switch lives only inside `sqlite.cc`, never in a public header or user code — see [coding-guidelines.md](coding-guidelines.md).

Planned refinement:

- `[planned]` **typed query layer** — a compile-time-validated, typed query front end (in the spirit of `cc::format`)
  on top of the prepared-statement API.

### Other data formats [planned]

`[planned]` a hex codec, and further structured formats as needed.

## geometry/ [in progress]

### obj [done]

Reader only. A faithful, flat mirror of the Wavefront `.obj`:

- `positions` / `texcoords` / `normals` as parallel `tg::pos3f` / `tg::vec2f` / `tg::vec3f` arrays (optional trailing coords dropped),
- faces as flattened `corners` + per-face spans, so polygons of any arity are preserved (no triangulation, no dedup),
- `o` / `g` / `usemtl` recorded as named spans over faces; `mtllib` names collected; `s` and unknown directives skipped.

OBJ's 1-based and negative/relative indices are both resolved to 0-based here; a missing corner attribute is `-1`.

### gltf [done]

Reader for both glTF 2.0 containers: the JSON `.gltf` and the binary `.glb`.
`detect_container` picks between them and never fails — anything that does not open with the GLB magic is read as JSON, so a
malformed file reports a JSON parse error with a byte offset instead of a useless "unrecognized container".

**This is the format that takes bytes instead of a stream, and zero copy is the whole reason.**
A `.glb` carries its vertex / index / texture payload inline, so `read(cc::pinned_data<byte const>)` returns each `buffers` entry
as a `subdata` of the input that shares its owner: nothing bulk is copied, and the views stay valid after the caller drops its
own handle. A bufferView-backed image gets the same treatment, which is what makes `img.data` go straight into `babel::image::read`.
The JSON side costs nothing extra — the JSON chunk is a subspan handed to `babel::json::read`, whose span stream is unbuffered,
so the parse runs directly against the input bytes.
The stream and span overloads exist for convenience and are documented as owning a copy.

The structure is a flat mirror: one `cc::vector` per glTF array, cross-references as **one strong enum per index role**
(`accessor_index`, `material_index`, …, each with `invalid = -1`), and every list-of-lists flattened into one array plus per-owner
runs, exactly as OBJ does with face corners. `data::find` resolves an index (nullptr only for `invalid`, because `read` validates
every index it stores), and `primitives_of` / `attributes_of` / `children_of` / `nodes_of` resolve the runs.

`accessor_view` is the data path: `view_of` resolves a bufferView's stride (0 in the file means tightly packed, so the element size
becomes the stride) and stacks the accessor's own `byteOffset` on top of the view's.
`element_size` includes the spec's per-column 4-byte matrix padding — a `MAT3` of `u8` is 12 bytes, not 9.
From a view, `is_typed_as<T>` / `as_strided<T>` read in place when size and alignment allow, `read_elements<T>` always works
(one memcpy per element, de-interleaving as it goes), and `data::read_indices` widens a u8 / u16 / u32 index accessor to `u32`.

**External URIs are resolved through a callback, never by babel.**
`read_options::resolve_uri` is a `cc::function_ref` the caller supplies; unset, an external URI stays recorded with
`resolved == false` and empty `data`. Percent-decoding and joining against a base path are the resolver's job, because babel owns
no filesystem policy. `data:` URIs resolve during read via `babel::base64` and must be base64 — glTF 2.0 allows no other form.

Validation follows one rule: **error when the value decides how bytes are interpreted, default or ignore when it only affects
appearance or is purely additive.** So a sparse accessor, an unknown `componentType` / `type` / `mode`, an out-of-range index, a
range that escapes its bufferView or buffer, and a **non-empty `extensionsRequired`** all fail the read — the last one is
spec-mandated, and it is what correctly rejects Draco / meshopt / basisu with a message naming the extension.
Unknown members, `extras`, morph targets, and the unmodelled `skins` / `animations` / `cameras` arrays are skipped,
and a wrong JSON type on an *optional* scalar falls back to its default.

**Skipped is not silent.** `data::issues` lists everything the reader did not implement (`unsupported`), could not follow
(`unresolved`), or tolerated (`malformed`) — each message naming the element by index, with `has_issues()` / `has_issue_of(kind)`
/ `issue_report()` on top. A `cc::result` alone cannot express "you got a usable structure, but not everything the file
described", which is the normal outcome for a real-world asset; the issue list is that channel, and an issue never substitutes
for an error (see [coding-guidelines.md](coding-guidelines.md)). A file this reader fully understands comes back with an empty list.

Planned refinements:

- `[planned]` **skins / animations / cameras** — recorded structurally today only as "skipped"; animations need channels + samplers
  + interpolation, which is a subsystem rather than a field.
- `[planned]` **morph targets** — tolerated (ignored) today, so adding them breaks nothing.
- `[planned]` **sparse accessors** — a hard error today, deliberately: silently ignoring the sparse override hands back the wrong geometry.
- `[planned]` **typed decode** — normalized-integer dequantization, cross-component conversion (`u16` accessor read as `f32`), and
  unpacking a padded matrix column layout into a tight `tg::mat3f`. The bytes and the metadata for all three are already exposed.
- `[planned]` **extensions / extras raw access** — a `read_options{ bool keep_json_document; }` plus an optional `json::document`
  member and a per-object `i32 json_node`. Deliberately not in v1: retaining the whole JSON document alongside the parsed
  structure doubles memory against "read once into a native structure", and a stored `json::ref` would dangle across a `cc::move(data)`.
- `[planned]` **a node's composed local transform** — blocked on typed-geometry, which has no `make_translation` / `make_scaling` /
  quaternion-to-matrix / TRS composition yet, so the reader stores `matrix` or TRS in whichever form the file used and composes nothing.
  That gap and the rest of what glTF wants from the lower libraries are written up in [lower-library-gaps.md](lower-library-gaps.md).
- `[planned]` **writer**.

### Other geometry formats [planned]

`[planned]` `.mtl` material libraries (referenced by OBJ), `.ply`, `.stl`.

## image/ [in progress]

Images bend the library's two shapes in two ways, both deliberate.

- **The aggregator ships alongside the format layer, not after it.**
  Every image format decodes to the *same* packed pixel buffer, so the "load an image" aggregator (`babel::image`) is useful immediately — there is no format-specific pixel shape to wait on.
  It dispatches by format and delegates to the low-level codecs; it never touches the backend itself.
- **The format layer is a real reader/writer pair, and the first writer in babel.**
  `babel::png` and `babel::jpg` are the format-shaped layer: decoded pixels **plus** the format's own metadata.
  Reading slurps then decodes (stb needs the whole buffer); writing establishes babel's `encode -> bytes` + `write(write_stream&)` convention (see [coding-guidelines.md](coding-guidelines.md)).

The backend is the vendored **stb** single-file libraries — babel's first **committed** third-party dependency (contrast sqlite's fetched-on-demand engine).
Because it is always in-tree, it is always linked; there is no availability probe.
It stays behind `image/impl/stb_backend` and links `PRIVATE`, so no stb header reaches a babel public header.

### png [done]

Low-level PNG reader + writer.
`read` returns a `data` with the decoded pixels (8-bit, expanded / de-palettized / de-interlaced by the backend) and the native IHDR fields (`bit_depth`, `color`, `interlace`) parsed directly.
The richer metadata fields (gamma, ICC, text chunks, physical dimensions, ...) are **designed but `[todo]`** — stb exposes none of it, so populating them needs a native chunk walker.
The fields exist now so that walker lands without an API change.
`encode` / `write` emit PNG via stb (lossless).

### jpg [done]

Low-level JPEG reader + writer, same shape.
Native SOF/JFIF fields (`bit_depth`, `progressive`, `chroma` subsampling, `jfif_density`) are parsed by walking the marker segments up to the first scan.
The variable-length metadata (`icc_profile` reassembled across APP2 markers, `exif`, `comments`) is `[todo]`.
`encode` / `write` emit baseline JPEG via stb at a `quality` (lossy).

### image [done]

The aggregator: a plain `{ width, height, channels, component, pixels }` buffer, `detect_format` from the magic bytes, `read` that auto-detects and delegates, and `encode` / `write` that build the low-level struct and hand it to the matching codec.
`component` is `u8` today with `u16` / `f32` reserved so the API already spans the reasonable pixel formats (16-bit PNG, HDR) without a future break.

### Other image formats [planned]

`[planned]` further stb-supported containers (bmp / tga / gif / hdr), 16-bit and float decode paths, and the native metadata walkers that fill the `[todo]` fields.

## Aggregators

`[done]` **`load_image`** is here — it is `babel::image` in the `image/` group above, dispatching across image formats and returning a plain pixel buffer + format enum (never an `sg` texture, so babel stays below the graphics stack).
The name lives in the group as `babel::image::read` rather than a free `load_image`, but it *is* the planned image aggregator.
`[planned]` `load_mesh` — dispatches across mesh formats and returns a triangle mesh.
Wants a `tg::mesh` (typed-geometry roadmap, not built yet); until then the format readers hand back their native structures.
With OBJ and glTF both landed there are now two native shapes for it to reconcile, which is what will make the aggregator's
vocabulary an actual decision rather than a rename of one reader's output.

## Dependency note

Among shaped-core libraries babel-serializer depends only on clean-core and typed-geometry, and sits above typed-geometry and below the graphics stack.
The image aggregator returning a plain pixel buffer (not an `sg::texture`) is what keeps that layering intact.

The sqlite format added the library's first **third-party** dependency (the vendored SQLite amalgamation), fetched on demand and linked `PRIVATE`.
The image formats add the first **committed** third-party dependency (the vendored stb single-file libraries), also linked `PRIVATE`.
Neither changes the layering: the backends stay out of every public header, so no shaped-core layer is affected and the public API never grows a third-party include.
