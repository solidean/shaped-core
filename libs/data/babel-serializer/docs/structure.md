# babel-serializer structure (babel::)

The living roadmap for babel-serializer.
Section headers carry a status tag:

- **[done]** — implemented and tested
- **[in progress]** — partially implemented
- **[planned]** — not started

Update the tags as formats land.
This document is design intent, not a guarantee of final API.

It is the roadmap and only that.
The rules that bind every format — the two layers, read-once-then-query, and what a reader takes as input — live in [coding-guidelines.md](coding-guidelines.md).
The design of a format that already exists lives in that format's own header, where the reader of it is.

## Top-level structure

```text
src/babel-serializer/
  data/        [in progress]   text / structured data formats
    base64     [done]          codec (both alphabets; optional padding)
    json       [done]          reader
    markdown   [done]          block-level reader (no inline parsing)
    sqlite     [done]          live database engine (read/write, transactions, incremental blob reads; fetch-on-demand backend)
  geometry/    [in progress]   mesh / geometry formats
    gltf       [done]          glTF 2.0 + GLB reader over pinned bytes (zero-copy buffers)
    obj        [done]          reader
  image/       [in progress]   image formats (read + write; committed libspng + stb backends, plus two native codecs)
    png        [done]          low-level reader + writer (native IHDR fields; ancillary chunks read and written)
    jpg        [done]          low-level reader + writer (native SOF/JFIF fields; rich metadata [todo])
    hdr        [done]          low-level reader + writer, fully native — Radiance RGBE, f32 out
    pfm        [done]          low-level reader + writer, fully native — Portable FloatMap, f32 out
    image      [done]          aggregator: pixel buffer + format-dispatching read/encode/write
```

## data/ [in progress]

### base64 [done]

A plain codec, not a format reader — no `data` structure and no `read`, just four functions.
Design and tolerances: [base64.hh](../src/babel-serializer/data/base64.hh).

No refinements planned.

### json [done]

Reader only, into the flat document shape [json.hh](../src/babel-serializer/data/json.hh) describes.

- `[planned]` **writer** — a separate `babel::json::write` API with its own builder type, not the reader's `document`.
- `[planned]` **richer errors** — a `parse_error` with line/column; today a `cc::result` message carrying the byte offset.
- `[planned]` **lossless numbers** — recover exact integers, where today every number is a `double`.
  Costs an arena copy of the raw slice.
- `[planned]` **compact node** — union the string / container payload fields, separate today for clarity.

### markdown [done]

Reader only, and **block level only** — the deliberate cut that makes a first version tractable.
It shares json's flat document shape exactly, so knowing one reader is knowing the other; see [markdown.hh](../src/babel-serializer/data/markdown.hh).

- `[planned]` **inline parsing** — emphasis, links, code spans and images as child nodes under a paragraph.
  This is the single biggest cut, and it is why there is no `emphasis` / `link` / `code_span` node kind.
- `[planned]` **setext headings** (`===` / `---` underlines), which need one line of lookahead against the thematic-break reading.
- `[planned]` **indented code blocks** — cut for now because a naive 4-space rule collides with list-item content indentation.
- `[planned]` **tables, HTML blocks, link reference definitions**, and exact tab-stop handling; tabs count as 4 columns today.

### sqlite [done]

The first format that is not a one-shot stream parser: a live database engine behind a thin RAII wrapper, full read/write.
See [sqlite.hh](../src/babel-serializer/data/sqlite.hh) for the shape, and [coding-guidelines.md](coding-guidelines.md) for the always-available-API rule its fetch-on-demand backend forces.

- `[done]` **incremental blob I/O, transactions and connection configuration** — a read handle over one BLOB cell,
  an RAII transaction that commits on request and rolls back otherwise, and the journal-mode / busy-timeout / `foreign_keys` pragmas.
  Grown for `vdoc::file`, which needs to read a chunk without materializing its row and to publish a whole document in one transaction.
- `[planned]` **writing through a blob handle** — reading is what a content store needs first; writing is a separate capability.
- `[planned]` **typed query layer** — a compile-time-validated, typed query front end (in the spirit of `cc::format`) on top of the prepared-statement API.

### Other data formats [planned]

`[planned]` a hex codec, and further structured formats as needed.

## geometry/ [in progress]

### obj [done]

Reader only: a faithful, flat mirror of the Wavefront `.obj`, no triangulation and no dedup.
See [obj.hh](../src/babel-serializer/geometry/obj.hh).

No refinements planned for the reader itself.
A `.mtl` reader is listed below.

### gltf [done]

Reader for both glTF 2.0 containers, the JSON `.gltf` and the binary `.glb`, over pinned bytes for zero copy.
See [gltf.hh](../src/babel-serializer/geometry/gltf.hh) for the structure and the data path.
It established three of the rules in [coding-guidelines.md](coding-guidelines.md): bytes-instead-of-a-stream, one strong enum per index role, and the import-issue list.

- `[planned]` **skins / animations / cameras** — recorded today only as "skipped".
  Animations need channels + samplers + interpolation, which is a subsystem rather than a field.
- `[planned]` **morph targets** — tolerated (ignored) today, so adding them breaks nothing.
- `[planned]` **sparse accessors** — a hard error today, deliberately: silently ignoring the sparse override hands back the wrong geometry.
- `[planned]` **typed decode** — normalized-integer dequantization, cross-component conversion (a `u16` accessor read as `f32`), and unpacking a padded matrix column layout into a tight `tg::mat3f`.
  The bytes and the metadata for all three are already exposed.
- `[planned]` **extensions / extras raw access** — a `read_options{ bool keep_json_document; }` plus an optional `json::document` member and a per-object `i32 json_node`.
  Deliberately not in v1: retaining the whole JSON document alongside the parsed structure doubles memory against read-once-into-a-native-structure.
  A stored `json::ref` would also dangle across a `cc::move(data)`.
- `[planned]` **a node's composed local transform** — blocked on typed-geometry, which has no TRS composition yet.
  The reader stores `matrix` or TRS in whichever form the file used and composes nothing; [lower-library-gaps.md](lower-library-gaps.md) has that gap and the rest.
- `[planned]` **writer**.

### Other geometry formats [planned]

`[planned]` `.mtl` material libraries (referenced by OBJ), `.ply`, `.stl`.

## image/ [in progress]

Images are where the aggregator layer ships alongside the format layer rather than after it, and where babel's writer convention was set.
Both are rules now, in [coding-guidelines.md](coding-guidelines.md).

### png [done]

Low-level PNG reader + writer; see [png.hh](../src/babel-serializer/image/png.hh).

Runs on the vendored libspng, which is also what reads and writes the ancillary chunks: gAMA, sRGB, iCCP, tEXt / zTXt / iTXt and pHYs.
`channels` follows the file rather than a fixed output shape, and a tRNS chunk becomes an alpha channel.

16-bit files decode to `u16` samples and re-encode at that width, host-endian in memory and big-endian on disk.
Sub-byte depths (1/2/4) unpack to `u8`, so `bit_depth` is the only place they survive.

A PNG is untrusted input, so the decode caps image dimensions and ancillary-chunk memory rather than believing a header.
Both ceilings are `spng_backend.cc` constants, and both are errors rather than a truncated decode.
libspng ships no default worth having, and the chunks are deflate-compressed, so a few crafted kilobytes would otherwise inflate to gigabytes.

Four decode behaviours babel's own encoder cannot produce are pinned by hand-built PNGs under `tests/image/fixtures/`, embedded as bytes by the script that generates them.
Palette, tRNS-becomes-alpha, sub-byte grey and Adam7 — every one of them decode-only, and so unreachable through a round-trip.

- `[planned]` **the remaining chunks** — cHRM, bKGD, sBIT and tIME, each an added field rather than an API change.

### jpg [done]

Low-level JPEG reader + writer, same shape; see [jpg.hh](../src/babel-serializer/image/jpg.hh).

- `[planned]` **variable-length metadata** — the ICC profile reassembled across APP2 markers, the APP1 EXIF block, and the COM comments.
  The structural SOF/JFIF fields are already parsed natively.

### hdr [done]

Low-level Radiance HDR reader + writer, and the first image codec with **no backend under it at all**; see [hdr.hh](../src/babel-serializer/image/hdr.hh).
Both scanline encodings are read: the adaptive per-component RLE, and flat RGBE with the old `1 1 1 n` repeat marker.
Both resolution orders are normalized to a top-left origin, and every `KEY=value` header line survives.
Nothing here is `[todo]`.

- The shared exponent is the format's own lossiness, not a gap: three 8-bit mantissas against one exponent give ~0.2% of a pixel's largest channel.

### pfm [done]

Low-level Portable FloatMap reader + writer, also fully native; see [pfm.hh](../src/babel-serializer/image/pfm.hh).
PFM stores IEEE-754 bits, so the round-trip is **exact** — which is what makes it the format to reach for when values matter more than file size.
Reading normalizes both of the format's conventions away: rows come back top-first, samples in host byte order, with `byte_order` still reporting what the file used.

### image [done]

The aggregator: a plain pixel buffer, `detect_format` from the magic bytes, and format-dispatching `read` / `encode` / `write`.
See [image.hh](../src/babel-serializer/image/image.hh).

- `[done]` **the f32 sample path** — HDR and PFM decode to `component::f32`, and `encode` rejects an image whose `comp` the target format cannot store.
- `[done]` **16-bit samples** — a 16-bit PNG decodes to `component::u16` and re-encodes at that width; PNG is the one format spanning two sample types.

### Other image formats [planned]

`[planned]` further stb-supported containers (bmp / tga / gif), and `.exr`.

## Aggregators

The "load an X across formats" layer.
One has landed.

- `[done]` **`load_image`** — it is `babel::image`, in the `image/` group above.
  It returns a plain pixel buffer and a format enum, never an `sg::texture`, which is what keeps babel below the graphics stack.
- `[planned]` **`load_mesh`** — dispatches across mesh formats and returns a triangle mesh.
  Wants a `tg::mesh`, on the typed-geometry roadmap and not built yet; until then the format readers hand back their native structures.
  With OBJ and glTF both landed there are now two native shapes for it to reconcile.
