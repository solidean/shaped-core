# babel-serializer cheat sheet

Serialization / deserialization of various formats.
Namespace `babel`; headers included by full path from `src/`.
> Each format parses into an unopinionated, read-once structure, and reading takes a `cc::read_stream`.
> The deviation: `gltf` takes a `cc::pinned_data<byte const>` so its buffers are zero-copy subviews of the input.
> [docs/coding-guidelines.md](docs/coding-guidelines.md) owns both rules.

```cpp
#include <babel-serializer/all.hh>   // umbrella (base64 + json + markdown + sqlite + obj + gltf + png + jpg + hdr + pfm + image)
```

---

**Recording domain:** `babel` — each format shadows it: `babel.json`, `babel.obj`, `babel.gltf`, `babel.png`, `babel.jpg`, `babel.hdr`, `babel.pfm`, `babel.image`, `babel.markdown`, `babel.sqlite`.
Every `CC_LOG_*` and `CC_RECORD_*` site in this library is attributed to it; see [logging](../../base/clean-core/docs/logging.md).

## base64 (`babel::base64`)

Tolerant on input, canonical on output.
No streaming interface — data URIs and embedded blobs are already in memory.

```cpp
#include <babel-serializer/data/base64.hh>

cc::optional<isize> decoded_size(cc::string_view text);              // nullopt when text is not valid base64
cc::result<cc::vector<cc::byte>> decode(cc::string_view text);       // fresh buffer
cc::result<isize> decode_into(cc::string_view text, cc::span<cc::byte> out); // -> bytes written; short out is an error
cc::string encode(cc::span<cc::byte const> bytes);                   // standard alphabet, '=' padded

babel::base64::decode("Zm9vYmFy").value();  // "foobar"
babel::base64::decode("-_-_").value();      // URL-safe alphabet decodes too
```

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

**Writing is a separate API** — imperative, streaming, and sharing no type with the reader (there is no writable document):

```cpp
auto w = babel::json::writer(out, {.indent = 2});  // out: a cc::write_stream
auto sw = babel::json::string_writer({});          // owns a cc::string; finish() -> result<cc::string>
auto& j = sw.underlying();                         // the writer under the convenience wrapper

// the imperative layer, which is the whole API
j.begin_object();  j.begin_array("k");  j.end_array();  j.end_object();  // keyed forms in an object, bare in an array
j.write("key", value);                      // null / bool / any int width / float / double / string_view
j.write(value);                             // an array element, or a bare scalar as the whole document
j.write("x", 1.5, cc::float_notation::fixed, 3);   // this one value, its own notation
j.write_ascii("k", utf8);                   // escape every non-ASCII byte as \uXXXX
j.write_raw("k", R"([1,2])");               // verbatim JSON: never parsed, escaped or checked

// the RAII layer, sugar over exactly those calls, and the two mix freely on one writer
auto o = w.object();  auto a = w.array();   // the root scope; closes when the handle dies
o.write("key", value);                      // the same set; an array scope's overloads take no key
auto inner = o.write_object("k");  auto arr = o.write_array("k");
auto rec = arr.write_object(babel::json::layout::compact);          // this scope (and all inside it) on ONE line

cc::result<cc::unit> r = w.finish();        // THE place errors surface; a scope left open is one of them
babel::json::write_report rep = w.report(); // what CHANGED on the way out; survives finish()
rep.is_clean();                             // non_finite + large_integers + undecodable_bytes all 0
```

`write_options`: `indent` (0 = compact; indenting also puts a space after ':'), `floats` + `float_precision`,
`non_finite` (`error` (default) / `null` / `string`), `large_integers` (`number` (default) / `string` / `error`, past 2^53),
`escape_non_ascii`, `escape_html` (`<` as a `\u003c`, for a `<script>` payload), `newline_delimited` (json-nd: several roots, one per line).

- **Errors are sticky**: the first failed write is recorded, later writes are no-ops, `finish()` reports it.
  Structural misuse (a scope written to while its child is open, a key into an array, an `end_array()` closing an object) lands there too, and asserts as well where assertions are on.
  Treat that assert as today's contract rather than a promise: it may later become a reported error, since widening costs callers nothing while the reverse kills programs relying on it.
  The one structural failure that is **not** an assert is `finish()` with a scope still open — the brackets go out anyway, so the bytes stay well-formed, and `finish()` says the document stops short.
  A writer that is never finished logs its error via `CC_LOG_ERROR` rather than losing it.
- **A valid document can still be lossy**: a NaN became null, an id past 2^53 will round in a double-based reader.
  That is what `report()` counts — it is never an error, and the counts are flat because a streaming writer knows the value, not the path to it.
- **Duplicate keys are not detected, and UTF-8 is never validated** — both on purpose, both documented in the header.
- **`layout::compact` is one record per line** in an otherwise indented document — what a trace or a log wants, where a field per line is unreadable.
- `dev.py example babel-serializer/json-write` runs one example.
  The others are `json-read`, `json-imperative`, `json-write-stream`, `json-write-numbers`, `json-newline-delimited` and `json-round-trip`.

## Markdown (`babel::markdown`)

Block level only — the same flat `document` / `ref` shape as JSON.
Inline spans (emphasis, links, code spans) are **not** parsed.

```cpp
#include <babel-serializer/data/markdown.hh>

cc::result<babel::markdown::document> read(cc::read_stream& in); // + string_view / span overloads

babel::markdown::document doc = read(text).value();
babel::markdown::ref root = doc.root();     // the document node; children are the top-level blocks
babel::markdown::ref n = doc.node_at(3);    // preorder index; 0 .. node_count() visits every block
isize count = doc.node_count();
```

`babel::markdown::ref` — cheap, copyable, kind-tolerant, exactly like `json::ref`:

```cpp
r.is_document() / is_heading() / is_paragraph() / is_code_block();
r.is_list() / is_list_item() / is_block_quote() / is_thematic_break();
babel::markdown::node_kind k = r.kind();

i32 lvl = r.level();              // heading 1..6, else 0
bool ord = r.is_ordered();        // ordered list?
i32 line = r.line();              // 1-based source line the block starts on
cc::string_view t = r.text();     // heading / paragraph / code text; raw, inline spans unparsed
cc::string_view i = r.info();     // a code block's fence info string ("cpp", "cpp [rule] fix=…", …)

isize n = r.size();               // child block count
babel::markdown::ref c = r[0];    // i-th child block; invalid ref if out of range
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

## glTF 2.0 / GLB (`babel::gltf`)

Takes **bytes, not a stream**: every embedded buffer comes back as a `cc::pinned_data` subview sharing the input's owner.

```cpp
#include <babel-serializer/geometry/gltf.hh>

babel::gltf::container detect_container(cc::span<cc::byte const> bytes); // gltf | glb; never fails

struct read_options {   // by value; the default touches nothing outside the input
    cc::function_ref<cc::result<cc::pinned_data<cc::byte const>>(cc::string_view uri)> resolve_uri;
};

cc::result<babel::gltf::data> read(cc::pinned_data<cc::byte const> bytes, read_options = {}); // ZERO-COPY
cc::result<babel::gltf::data> read(cc::pinned_data<cc::byte> const& bytes, read_options = {});
cc::result<babel::gltf::data> read(cc::read_stream& in, read_options = {});       // slurps, then pins (moves)
cc::result<babel::gltf::data> read(cc::span<cc::byte const> bytes, read_options = {}); // COPIES into a pin
cc::result<babel::gltf::data> read(cc::string_view text, read_options = {});           // COPIES into a pin

// getting a pin from bytes you already own — cc::make_pinned_data moves, it does not copy
auto pinned = cc::pinned_data<cc::byte const>(cc::make_pinned_data(cc::move(bytes)));
```

```cpp
auto const doc = babel::gltf::read(pinned).value();
doc.source;            // container::gltf | container::glb
doc.asset;             // asset_info { version, min_version, generator, copyright }

// what the read did NOT give you — a successful read with issues is normal for a real asset
doc.issues;                 // cc::vector<issue> { issue_kind kind; cc::string message; }, in notice order
doc.has_issues();           // bool
doc.has_issue_of(babel::gltf::issue_kind::unsupported); // unsupported | unresolved | malformed
doc.issue_report();         // cc::string, one "kind: message" line per issue; "" when clean

doc.extensions_used;   // cc::vector<cc::string>; extensions_required is always empty (a non-empty one fails the read)

doc.buffers;      // cc::vector<buffer>      — { uri, byte_length, pinned_data<byte const> data, resolved }
doc.buffer_views; // cc::vector<buffer_view> — { buffer, byte_offset, byte_length, byte_stride (0 = packed), target }
doc.accessors;    // cc::vector<accessor>    — { buffer_view, byte_offset, component, type, count, normalized, ... }
doc.attributes;   // cc::vector<attribute>   — every primitive attribute, flattened { semantic, accessor }
doc.primitives;   // cc::vector<primitive>   — flattened { first_attribute, attribute_count, indices, material, mode }
doc.meshes; doc.nodes; doc.scenes; doc.materials; doc.textures; doc.images; doc.samplers;
doc.default_scene;     // scene_index; the document's `scene`
```

Index roles are strong enums (`buffer_index`, `buffer_view_index`, `accessor_index`, `mesh_index`, `node_index`,
`scene_index`, `material_index`, `texture_index`, `image_index`, `sampler_index`), each with `invalid = -1`:

```cpp
buffer const* b = doc.find(buffer_index(0));    // one overload per role; nullptr ONLY for `invalid`
cc::span<primitive const> ps = doc.primitives_of(doc.meshes[0]);
cc::span<attribute const> as = doc.attributes_of(ps[0]);
cc::span<node_index const> cs = doc.children_of(doc.nodes[0]);
cc::span<node_index const> rs = doc.nodes_of(doc.scenes[0]);
accessor_index pos = doc.find_attribute(ps[0], "POSITION"); // invalid when absent
cc::span<f32 const> lo = doc.min_of(doc.accessors[1]);      // empty when the file stated no bounds; also max_of
```

```cpp
// accessor data: a strided view that CARRIES the buffer's pin, so it may outlive `doc`
cc::result<babel::gltf::accessor_view> v = doc.view_of(pos);  // also view_of(accessor const&)
v.value().bytes;        // pinned_data<byte const>, starting at element 0
v.value().stride;       // NEVER 0 — a packed accessor gets element_size
v.value().count; v.value().element_size; v.value().component; v.value().type; v.value().normalized;
v.value().element(i);   // cc::span<byte const>, non-owning; asserts 0 <= i < count

auto const view = v.value();
view.is_typed_as<tg::vec3f>();     // size + alignment allow reading in place?
view.as_strided<tg::vec3f>();      // cc::strided_span<tg::vec3f const>; precondition: is_typed_as
view.read_elements<tg::vec3f>();   // result<cc::vector<T>>; always safe (memcpy per element, de-interleaves)

cc::result<cc::vector<u32>> idx = doc.read_indices(ps[0]);  // widens a u8 / u16 / u32 SCALAR accessor

// accessor arithmetic, if you do it yourself:
a.component_count(); // 1/2/3/4/4/9/16   a.component_size(); // 1/1/2/2/4/4
a.element_size();    // packed element bytes INCLUDING per-column 4-byte matrix padding (MAT3 of u8 == 12, of u16 == 24)
                     // MAT4 / MAT3 of f32 match sizeof(tg::mat4f) / sizeof(tg::mat3f)
```

```cpp
// a node keeps the transform form the file used — `matrix` and TRS are mutually exclusive in the spec
node const& n = doc.nodes[0];
n.has_matrix ? use(n.matrix) : use(n.translation, n.rotation, n.scale); // tg::mat4f / vec3f + quat_f + vec3f
```

## SQLite (`babel::sqlite`)

A live database engine, not a stream parser — a thin RAII wrapper over an open connection.
Full read/write.

```cpp
#include <babel-serializer/data/sqlite.hh>

bool ok = babel::sqlite::is_available();  // false if the backend wasn't compiled in (fetch-on-demand)

// open: file (create if missing) / existing file read-only / :memory: / a serialized byte image
cc::result<babel::sqlite::database, babel::sqlite::error> babel::sqlite::database::open(cc::string_view path);
cc::result<babel::sqlite::database, babel::sqlite::error> babel::sqlite::database::open_readonly(cc::string_view path);
cc::result<babel::sqlite::database, babel::sqlite::error> babel::sqlite::database::open_memory();
cc::result<babel::sqlite::database, babel::sqlite::error> babel::sqlite::database::open_blob(cc::span<cc::byte const> bytes);

auto db = babel::sqlite::database::open_memory().value(); // move-only; closes the handle on destruction
```

```cpp
// every call reports babel::sqlite::error — a code, SQLite's own result code, and the message
struct babel::sqlite::error { error_code code; i32 native_code; cc::string message; };
// unknown / backend_missing / not_a_database / corrupt / busy / cannot_open / read_only / io_error / constraint / full / misuse
e.code == babel::sqlite::error_code::busy;  // branch on the code; never match on message text
// copyable, unlike cc::any_error — a caller can latch the first failure and still read it later
// converts implicitly into cc::result<T, cc::any_error>, so a caller that ignores the code loses nothing
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
    use(stmt.last_error());      // sqlite::error const& — named last_error so it doesn't hide the type

stmt.reset();                    // re-execute (keeps bound parameters); clear_bindings() resets them to NULL
```

```cpp
// connection configuration — each returns a cc::result
db.set_journal_mode(babel::sqlite::journal_mode::wal);   // delete_journal / truncate / persist / memory / wal / off
db.get_journal_mode();                                    // -> result<journal_mode>: what it ACTUALLY is, not what was asked
db.set_busy_timeout(250);                                 // ms a blocked write waits; 0 = do not wait
db.set_foreign_keys(true);                                // SQLite defaults this OFF — ON DELETE CASCADE needs it
db.get_foreign_keys();                                    // -> result<bool>

// the two file-header fields the application owns; both survive a reopen and live outside any table
db.set_application_id(0x56444F43);  // whose file this is, to anything inspecting it; SQLite never reads it
db.get_application_id();            // -> result<i32>; 0 on a database nobody has stamped
db.set_user_version(1);             // the format version
db.get_user_version();              // -> result<i32>; reading it is the FIRST page read, so not_a_database surfaces here
```

```cpp
// one transaction: commit() publishes it, anything else rolls it back
auto tx = db.begin_transaction().value();  // move-only
db.exec("INSERT INTO t VALUES (2, 'atomic')");
tx.commit();                               // the reporting path; the destructor cannot report
tx.is_open();                              // false once committed or moved from
```

```cpp
// incremental blob I/O: read one BLOB cell in pieces, without materializing the row
auto h = db.open_blob_handle({.table = "chunks", .column = "data", .rowid = 1}).value(); // NOT open_blob (that's a whole db)
isize n = h.size();                        // the whole value's size in bytes
h.read_at(offset, out_span);               // the range must lie inside the blob; past the end is an error
h.reopen(other_rowid);                     // same handle, next row — cheaper than opening another
// read-only for now; the row's table must be a rowid table (a WITHOUT ROWID one cannot be reached this way)
```

## Images (`babel::image` + `babel::png` / `babel::jpg` / `babel::hdr` / `babel::pfm`)

Two layers: low-level per-format codecs that expose the format's own metadata, and an aggregator for "just the pixels".
`png` decodes through the vendored **libspng** and `jpg` through the vendored **stb** — always linked, never visible from a babel header, and never reached by the aggregator.
`hdr` / `pfm` are fully native and reach no backend at all.

```cpp
#include <babel-serializer/image/image.hh>  // aggregator — the "I just want pixels" layer

cc::result<babel::image::image> read(cc::span<cc::byte const> bytes); // auto-detects PNG / JPG / HDR / PFM
cc::result<babel::image::image> read(cc::read_stream& in);            // slurps, then decodes
cc::result<babel::image::format> detect_format(cc::span<cc::byte const> bytes); // png / jpg / hdr / pfm from magic bytes

struct image {                        // row-major, top-left origin, tightly packed
    int width; int height; int channels;      // 1 grey / 2 GA / 3 rgb / 4 rgba
    babel::image::component comp;             // u8 (png/jpg) | f32 (hdr/pfm) | u16 (API-ready)
    cc::vector<cc::byte> pixels;
    bool is_empty(); int bytes_per_component(); isize row_stride();
    cc::span<float const> samples_f32();      // the pixels as floats; empty unless comp == f32
};

// writing — babel's first writer. encode -> bytes, or write to a stream. jpg_quality ignored by every other format.
cc::result<cc::vector<cc::byte>> encode(image const&, babel::image::format fmt, {.jpg_quality = 90});
cc::result<cc::unit> write(cc::write_stream& out, image const&, babel::image::format fmt, {...});
```

```cpp
#include <babel-serializer/image/png.hh>   // low-level PNG: pixels + native metadata
#include <babel-serializer/image/jpg.hh>   // low-level JPG: pixels + native metadata

babel::png::data p = babel::png::read(bytes).value();
p.width; p.height; p.channels; p.pixels;       // channels follow the file: 1 grey / 2 GA / 3 rgb / 4 rgba, +1 for a tRNS chunk
p.bit_depth; p.color; p.interlace;             // native IHDR fields; pixels are always 8-bit whatever bit_depth says
p.gamma; p.srgb_intent; p.icc_profile; p.texts; p.physical;  // gAMA / sRGB / iCCP / tEXt+zTXt+iTXt / pHYs, read AND written

babel::jpg::data j = babel::jpg::read(bytes).value();
j.bit_depth; j.progressive; j.chroma; j.jfif_density; // native SOF/JFIF fields
j.icc_profile; j.exif; j.comments;                    // [todo] designed, not yet populated

babel::png::encode(p, {.compression_level = 9});  // -1 (zlib default) .. 9; always 8-bit, non-interlaced
babel::jpg::encode(j, {.quality = 90});           // + write(stream, ...) for both
```

```cpp
#include <babel-serializer/image/hdr.hh>   // low-level Radiance HDR: f32 radiance + the header's variables
#include <babel-serializer/image/pfm.hh>   // low-level PFM: f32 samples + scale and byte order

babel::hdr::data h = babel::hdr::read(bytes).value();
h.samples_f32();                               // width * height * 3 linear values, top-left origin
h.format;                                      // pixel_format::rgbe | ::xyze, from the FORMAT= line
h.run_length_encoded; h.stored_bottom_up;      // what the file did; `pixels` are normalized either way
h.exposure; h.pixel_aspect; h.software;        // parsed header variables (cc::optional / cc::string)
h.variables;                                   // every KEY=value line, in file order

babel::hdr::encode(h, {.run_length_encode = true});  // false writes flat RGBE; a width outside [8, 0x7fff] is flat regardless

babel::pfm::data f = babel::pfm::read(bytes).value();
f.channels;                                    // 3 for the "PF" magic, 1 for "Pf"
f.scale;                                       // the header's factor — metadata, NOT applied to the samples
f.byte_order;                                  // what the file used; `pixels` are always host order

babel::pfm::encode(f, {.byte_order = babel::pfm::endianness::little});  // + write(stream, ...)
```

## Chrome Trace (`trace/chrome_trace.hh`)

Writes a `cc::rec::recording` as Chrome Trace Event JSON — the first thing that lets you LOOK at a recording rather than assert on one.
Write-only: we write traces for other tools to read, and read our own recordings through `cc::rec`.

```cpp
#include <babel-serializer/trace/chrome_trace.hh>
babel::chrome_trace::encode(recording);              // -> cc::result<cc::vector<byte>>
babel::chrome_trace::write(out_stream, recording);   // -> cc::result<cc::unit>; encode + write

babel::chrome_trace::write_options{
    .process_id = 1, .process_name = "shaped-core",
    .include_system_events = false,  // gaps / chunk acquisition / dropped spans: noise until you ask about the recorder
    .include_logs = true, .include_values = true, .include_stats = true, .include_scopes = true,
    .pretty = true,                  // one event per line, so a diff or a grep works
};
```

The mapping:

| cc::rec | Chrome Trace | note |
|---|---|---|
| `scope_begin` / `scope_end` | `"B"` / `"E"` | a scope that never closed still renders instead of being dropped |
| `marker`, `value`, `log` | `"i"` instant | the payload's declared fields land in `args` |
| `stat_snapshot` | `"C"` counter | the reading |
| `stat_accumulate` | `"C"` counter | the **running total** — a counter track shows a level, not a delta |
| a domain | `cat` | so a viewer filters per library |

Timestamps are microseconds **relative to the earliest event**, not since the epoch: absolute time would spend most of a double's precision on a number no viewer shows.

Run `uv run dev.py example babel-serializer/chrome-trace` for a synthetic workload, its query output and a trace file.

## Gotchas

Only what the signatures above cannot tell you.

- **`ref` borrows the document.** It holds a `document*`, so keep the `document` alive while traversing.
  `as_string()` / `key()` return views into the document's arena, with the same lifetime.
- **Kind-tolerant accessors never fail on shape**, so check `is_valid()` when absence is what you care about.
  `r["missing"]`, `r[99]` and `r.as_double()` on a string each return a fallback or an invalid ref instead.
- **Errors carry an offset or a line.** JSON reports the byte offset, OBJ the line number, both as a `cc::result` error.
- **OBJ records, never applies.** No triangulation and no dedup, so polygons stay polygons; `usemtl` / `o` / `g` become named face spans and `s` is skipped outright.
- **base64 rejects a lone trailing character.** `"Zm9vY"` is a final quantum encoding no byte at all, so it is an error rather than a truncation.
- **Markdown never fails on content.** Every input is a valid document, so its `cc::result` reports stream I/O failure only.
  An unterminated fence simply runs to end of input.
- **Markdown `line()` is what makes a corpus debuggable**: a failing case points at a file and line rather than at a string literal.
- **glTF buffer views outlive the document.** `buffer::data` and `accessor_view::bytes` carry the pin, so they survive both the `data` and the caller's own handle.
  `accessor_view::element(i)` does *not* — it is a plain span into the view.
- **glTF `resolved` implies the exact length.** A resolved buffer has `data.size() == byte_length`, and a GLB BIN chunk is trimmed past its padding.
  `resolved == false` with empty `data` means an external URI nobody fetched, so supply `read_options::resolve_uri`.
  The URI arrives exactly as the file spelled it; percent-decoding and base-path policy are yours.
- **glTF is strict about bytes, lenient about everything else.**
  Hard errors: a sparse accessor, an unknown `componentType` / `type` / `mode`, and any range escaping its bufferView or buffer.
  A non-empty `extensionsRequired` is one too, which is what refuses Draco / meshopt / basisu by name.
  Skipped: unknown members, `extras`, morph targets, and the `skins` / `animations` / `cameras` arrays.
  A wrong JSON type on an *optional* scalar falls back to the default.
- **glTF `.value()` is not the whole answer — check `issues`**, which names every element the reader skipped, could not resolve, or tolerated.
  A usable-but-incomplete import is the normal case for a real asset, and a file the reader fully understood leaves `has_issues()` false.
- **glTF hands back encoded image bytes, never pixels.** `image::data` is the PNG/JPEG payload — feed it to `babel::image::read`.
  `babel::gltf` does not depend on `babel::image`.
- **glTF node transforms are not composed.** The reader never multiplies TRS into a matrix and never flattens the hierarchy; `has_matrix` says which fields are authoritative.
- **Cross into a glTF index enum explicitly**, with `int(x)` — the strong enums allow no implicit conversion.
- **SQLite: branch on `is_available()`, never on a macro.** The fetch-on-demand backend may be absent, and the API is declared and callable either way.
- **SQLite rows are valid only until the next step.** A `row`, and any `as_string()` / `as_blob()` it hands back, dies with it.
  Copy out with `cc::string::create_copy_of` to keep it.
  `database` and `statement` are move-only and own their handle.
- **A SQLite step error is sticky, not per-row.** The range-for just ends, so read `is_ok()` / `error()` after the loop.
- **A SQLite blob handle is invalidated by a write to its row**, including one through another connection.
  `read_at` then errors rather than returning stale bytes; `reopen` is how the handle comes back.
- **A SQLite transaction's rollback cannot report.** The destructor rolls back silently, so a caller who needs to know the write landed calls `commit()` and reads its result.
  Transactions do not nest — a nested `begin_transaction` is an ordinary error.
- **`get_journal_mode` is not a readback of `set_journal_mode`.** A mode can be refused — an in-memory database is always `memory`, whatever WAL was asked for.
  That is exactly why it is read rather than assumed.
- **JPG is lossy, PNG lossless.** Round-trip PNG for exact pixels, and expect small per-channel deltas through JPG.
- **`channels` is the *decoded* count**, since palette PNGs are de-palettized and Adam7 is de-interlaced.
  The native `color` / `interlace` fields still report the original encoding.
- **The sample type follows the format.** PNG / JPEG decode to `u8` whatever the file's native `bit_depth` says (`u16` is API-ready but not decoded); HDR / PFM decode to `f32`.
  `encode` errors on a mismatch rather than reinterpreting the buffer, so check `comp` after a `read` that picked its own format.
- **HDR is lossy and PFM is not.** RGBE quantizes a pixel's three mantissas against one shared exponent (~0.2% of the pixel's largest channel); PFM stores the bits, so its round-trip is exact.
- **HDR and PFM both store rows the other way up**, and both are flipped to a top-left origin on read — `stored_bottom_up` reports what the HDR file did.
- **Image rows carry no padding**: `row_stride() == width * channels * bytes_per_component()`.

## Umbrellas

```cpp
#include <babel-serializer/data/base64.hh>   // just base64
#include <babel-serializer/data/json.hh>     // just JSON
#include <babel-serializer/data/markdown.hh> // just markdown
#include <babel-serializer/data/sqlite.hh>   // just SQLite
#include <babel-serializer/geometry/obj.hh>  // just OBJ
#include <babel-serializer/geometry/gltf.hh> // just glTF / GLB
#include <babel-serializer/image/png.hh>     // just PNG (low-level)
#include <babel-serializer/image/jpg.hh>     // just JPG (low-level)
#include <babel-serializer/image/hdr.hh>     // just Radiance HDR (low-level)
#include <babel-serializer/image/pfm.hh>     // just PFM (low-level)
#include <babel-serializer/image/image.hh>   // the image aggregator
#include <babel-serializer/all.hh>           // everything
```
