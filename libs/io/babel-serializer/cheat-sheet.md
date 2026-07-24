# babel-serializer cheat sheet

Serialization / deserialization of various formats. Namespace `babel`; headers included by full path from `src/`.
> Reading takes a `cc::read_stream` and parses against its buffered window — no whole-input buffering.
> Each format parses into an unopinionated, read-once structure; string_view / span overloads wrap a `span_read_stream_adapter`.

```cpp
#include <babel-serializer/all.hh>   // umbrella (json + obj + sqlite + image)
```

---

## JSON (`babel::json`)

```cpp
#include <babel-serializer/data/json.hh>

cc::result<babel::json::document> read(cc::read_stream& in);      // parse from a stream
cc::result<babel::json::document> read(cc::string_view text);     // parse from an in-memory buffer
cc::result<babel::json::document> read(cc::span<cc::byte const>); // parse from raw bytes

babel::json::document doc = read(text).value(); // owns a flat node array + a string arena
babel::json::ref root = doc.root();             // non-owning {document*, index} handle
isize n = doc.node_count();                     // total parsed values
```

`babel::json::ref` — cheap, copyable, kind-tolerant (mismatched kind returns the fallback / an invalid ref):

```cpp
bool ok  = r.is_valid();          // false past-the-end / on a missing key
r.is_object() / is_array() / is_string() / is_number() / is_bool() / is_null();
babel::json::node_kind k = r.kind();

double d = r.as_double(0);        // fallback when not a number
bool   b = r.as_bool(false);      // fallback when not a bool
cc::string_view s = r.as_string(); // view into the document arena; fallback when not a string

isize count = r.size();           // children of an array/object, else 0
babel::json::ref e = r[2];        // i-th child (array or object); invalid ref if out of range
babel::json::ref v = r["key"];    // object member by key (first match wins); invalid ref if absent
bool has = r.has("key");          // object has this member?
cc::string_view key = e.key();    // this node's key within its parent object ("" for array elements)
```

## OBJ (`babel::obj`)

```cpp
#include <babel-serializer/geometry/obj.hh>

cc::result<babel::obj::data> read(cc::read_stream& in);          // + string_view / span overloads

babel::obj::data m = read(src).value();
m.positions;  // cc::vector<tg::pos3f>   — v  (optional w dropped)
m.texcoords;  // cc::vector<tg::vec2f>   — vt (optional third coord dropped)
m.normals;    // cc::vector<tg::vec3f>   — vn
m.corners;    // cc::vector<babel::obj::corner>  — every face corner, flattened
m.faces;      // cc::vector<babel::obj::face>    — each a run of corners (polygons preserved)
m.objects;    // cc::vector<babel::obj::group>   — 'o' spans over faces
m.groups;     // cc::vector<babel::obj::group>   — 'g' spans over faces
m.materials;  // cc::vector<babel::obj::group>   — 'usemtl' spans over faces
m.material_libraries; // cc::vector<cc::string>  — 'mtllib' names
```

```cpp
struct corner { i32 position; i32 texcoord; i32 normal; }; // 0-based indices; -1 = attribute absent
struct face   { i32 first_corner; i32 corner_count; };     // corners[first_corner .. +corner_count)
struct group  { cc::string name; i32 first_face; i32 face_count; }; // faces[first_face .. +face_count)

// iterate a face's corners:
for (auto ci = f.first_corner; ci < f.first_corner + f.corner_count; ++ci)
    auto const p = m.positions[m.corners[ci].position];
```

## SQLite (`babel::sqlite`)

A live database engine, not a stream parser — a thin RAII wrapper over an open connection. Full read/write.

```cpp
#include <babel-serializer/data/sqlite.hh>

bool ok = babel::sqlite::is_available();  // false if the backend wasn't compiled in (fetch-on-demand)

// open: file (create if missing) / existing file read-only / :memory: / a serialized byte image
cc::result<babel::sqlite::database> babel::sqlite::database::open(cc::string_view path);
cc::result<babel::sqlite::database> babel::sqlite::database::open_readonly(cc::string_view path);
cc::result<babel::sqlite::database> babel::sqlite::database::open_memory();
cc::result<babel::sqlite::database> babel::sqlite::database::open_blob(cc::span<cc::byte const> bytes);

auto db = babel::sqlite::database::open_memory().value(); // move-only; closes the handle on destruction
```

```cpp
db.exec("CREATE TABLE t(id INTEGER, name TEXT)");        // result-less SQL (DDL / INSERT / PRAGMA / transactions)
db.exec("INSERT INTO t VALUES (1, 'shaped')");
i64 rows = db.changes();                                  // rows touched by the last statement
i64 rowid = db.last_insert_rowid();
cc::vector<cc::byte> image = db.serialize();              // dump the main db to a byte image (round-trips via open_blob)
```

```cpp
babel::sqlite::statement stmt = db.query("SELECT id, name FROM t WHERE id = ?1").value(); // == prepare, reads as intent
stmt.bind(1, i64(1));            // parameters are 1-based (SQLite convention); overloads: i64 / double / string_view / span<byte const>
stmt.bind_null(2);

for (auto row : stmt)            // single-pass range-for over result rows
{
    i64 id            = row.as_i64(0);      // columns are 0-based; accessors coerce per SQLite rules
    cc::string_view s = row.as_string(1);   // bytes owned by SQLite — valid only until the next step
    cc::span<cc::byte const> b = row.as_blob(2);
    bool null = row.is_null(0);
    babel::sqlite::column_kind k = row.column_type(0); // null / integer / real / text / blob
}
if (!stmt.is_ok())               // a step error ends the loop silently; read it afterwards
    use(stmt.error());

stmt.reset();                    // re-execute (keeps bound parameters); clear_bindings() resets them to NULL
```

## Images (`babel::image` + `babel::png` / `babel::jpg`)

Two layers: low-level per-format codecs that expose the format's own metadata, and an aggregator for "just the pixels".
Backend is the vendored, always-linked **stb** (`extern/stb`), kept behind `image/impl/stb_backend` — no stb header reaches a babel header.

```cpp
#include <babel-serializer/image/image.hh>  // aggregator — the "I just want pixels" layer

cc::result<babel::image::image> read(cc::span<cc::byte const> bytes); // auto-detects PNG / JPG
cc::result<babel::image::image> read(cc::read_stream& in);            // slurps, then decodes
cc::result<babel::image::format> detect_format(cc::span<cc::byte const> bytes); // png / jpg from magic bytes

struct image {                        // row-major, top-left origin, tightly packed
    int width; int height; int channels;      // 1 grey / 2 GA / 3 rgb / 4 rgba
    babel::image::component comp;             // u8 (decoded today) | u16 | f32 (API-ready)
    cc::vector<cc::byte> pixels;
    bool is_empty(); int bytes_per_component(); isize row_stride();
};

// writing — babel's first writer. encode -> bytes, or write to a stream. jpg_quality ignored for PNG.
cc::result<cc::vector<cc::byte>> encode(image const&, babel::image::format fmt, {.jpg_quality = 90});
cc::result<cc::unit> write(cc::write_stream& out, image const&, babel::image::format fmt, {...});
```

```cpp
#include <babel-serializer/image/png.hh>   // low-level PNG: pixels + native metadata
#include <babel-serializer/image/jpg.hh>   // low-level JPG: pixels + native metadata

babel::png::data p = babel::png::read(bytes).value();
p.width; p.height; p.channels; p.pixels;       // populated from the decoder
p.bit_depth; p.color; p.interlace;             // native IHDR fields (parsed natively)
p.gamma; p.icc_profile; p.texts; p.physical;   // [todo] designed, not yet populated (stb exposes no metadata)

babel::jpg::data j = babel::jpg::read(bytes).value();
j.bit_depth; j.progressive; j.chroma; j.jfif_density; // native SOF/JFIF fields
j.icc_profile; j.exif; j.comments;                    // [todo] designed, not yet populated

babel::png::encode(p);  babel::jpg::encode(j, {.quality = 90});  // + write(stream, ...)
```

## Gotchas

- **Read-once, not mutable.** `json::document` is great to traverse, has no insertion API by design.
- **`ref` borrows the document.** It holds a `document*`; keep the `document` alive while traversing.
  `as_string()` / `key()` return views into the document's arena — same lifetime.
- **Kind-tolerant, never throws on shape.** `r["missing"]`, `r[99]`, `r.as_double()` on a string all return a fallback / invalid ref.
  Check `is_valid()` when absence matters.
- **OBJ indices are resolved.** 1-based and negative/relative OBJ indices are both converted to 0-based here; a missing corner attribute is `-1`.
- **OBJ is faithful, not a mesh.** No triangulation, no dedup — polygons stay polygons. `usemtl` / `o` / `g` / `s` are recorded (or skipped), not applied.
- **Errors carry an offset / line.** JSON errors report the byte offset; OBJ errors report the line number. Both come back as a `cc::result` error.
- **SQLite backend may be absent.** The API is *always* declared and callable; when the fetch-on-demand backend wasn't compiled in, `is_available()` is false and every `open_*` returns a `cc::result` error — never a missing symbol. Branch on `is_available()`, never on a macro.
- **SQLite handles are live and non-owning downstream.** `database` / `statement` are move-only and own their handle. A `row` and any `as_string()` / `as_blob()` it hands back are only valid until the next step or the statement dies — copy out (`cc::string::create_copy_of`) to keep them.
- **SQLite param vs. column indexing differs.** Bind parameters are **1-based** (`stmt.bind(1, …)`); result columns are **0-based** (`row.as_i64(0)`). A row-step error is sticky, not per-row: the range-for just ends — check `is_ok()` / `error()` after the loop.
- **Images are top-left origin, tightly packed.** `row_stride() == width * channels * bytes_per_component()`; no padding. Pixels are 8-bit today regardless of the file's native `bit_depth` (a u16 / f32 path is API-ready but not yet decoded).
- **JPG is lossy; PNG is lossless.** Round-trip PNG for exact pixels; expect small per-channel deltas through JPG. `channels` is the *decoded* count (palette PNGs are de-palettized, Adam7 is de-interlaced) — the native `color` / `interlace` fields still tell you the original encoding.
- **Image metadata is mostly `[todo]`.** stb decodes pixels only. The structural header fields (`bit_depth` / `color` / `interlace`; `progressive` / `chroma` / `jfif_density`) are parsed natively, but the rich metadata fields (ICC, gamma, text chunks, EXIF, comments) are *designed but not yet populated* — a future native chunk/marker walker fills them without an API change.
- **Aggregator vs. low-level.** `babel::image` is for "just the pixels" and dispatches by format; reach for `babel::png` / `babel::jpg` when you need a format's own metadata. The aggregator never touches stb — it delegates to the codecs.

## Umbrellas

```cpp
#include <babel-serializer/data/json.hh>     // just JSON
#include <babel-serializer/data/sqlite.hh>   // just SQLite
#include <babel-serializer/geometry/obj.hh>  // just OBJ
#include <babel-serializer/image/png.hh>     // just PNG (low-level)
#include <babel-serializer/image/jpg.hh>     // just JPG (low-level)
#include <babel-serializer/image/image.hh>   // the image aggregator
#include <babel-serializer/all.hh>           // everything
```
