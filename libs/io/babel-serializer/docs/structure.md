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

## Top-level structure

```text
src/babel-serializer/
  data/        [in progress]   text / structured data formats
    json       [done]          reader
    sqlite     [done]          live database engine (read/write; fetch-on-demand backend)
  geometry/    [in progress]   mesh / geometry formats
    obj        [done]          reader
```

## data/ [in progress]

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

`[planned]` base64 / hex codecs, and further structured formats as needed.

## geometry/ [in progress]

### obj [done]

Reader only. A faithful, flat mirror of the Wavefront `.obj`:

- `positions` / `texcoords` / `normals` as parallel `tg::pos3f` / `tg::vec2f` / `tg::vec3f` arrays (optional trailing coords dropped),
- faces as flattened `corners` + per-face spans, so polygons of any arity are preserved (no triangulation, no dedup),
- `o` / `g` / `usemtl` recorded as named spans over faces; `mtllib` names collected; `s` and unknown directives skipped.

OBJ's 1-based and negative/relative indices are both resolved to 0-based here; a missing corner attribute is `-1`.

### Other geometry formats [planned]

`[planned]` `.mtl` material libraries (referenced by OBJ), `.ply`, `.stl`, `.gltf`.

## Aggregators [planned]

`[planned]` `load_mesh` — dispatches across mesh formats and returns a triangle mesh.
Wants a `tg::mesh` (typed-geometry roadmap, not built yet); until then the format readers hand back their native structures.
`[planned]` `load_image` — dispatches across image formats and returns a plain pixel buffer + format enum (never an `sg` texture, so babel stays below the graphics stack).

## Dependency note

Among shaped-core libraries babel-serializer depends only on clean-core and typed-geometry, and sits above typed-geometry and below the graphics stack.
The image aggregator returning a plain pixel buffer (not an `sg::texture`) is what keeps that layering intact.

The sqlite format adds the library's first **third-party** dependency (the vendored SQLite amalgamation), but it changes none of the above:
the backend is fetched on demand, linked `PRIVATE`, and its absence is a runtime condition — no shaped-core layering is affected, and the public API never grows a third-party include.
