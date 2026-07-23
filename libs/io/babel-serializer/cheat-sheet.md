# babel-serializer cheat sheet

Serialization / deserialization of various formats. Namespace `babel`; headers included by full path from `src/`.
> Reading takes a `cc::read_stream` and parses against its buffered window — no whole-input buffering.
> Each format parses into an unopinionated, read-once structure; string_view / span overloads wrap a `span_read_stream_adapter`.

```cpp
#include <babel-serializer/all.hh>   // umbrella (json + obj)
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

## Gotchas

- **Read-once, not mutable.** `json::document` is great to traverse, has no insertion API by design.
- **`ref` borrows the document.** It holds a `document*`; keep the `document` alive while traversing.
  `as_string()` / `key()` return views into the document's arena — same lifetime.
- **Kind-tolerant, never throws on shape.** `r["missing"]`, `r[99]`, `r.as_double()` on a string all return a fallback / invalid ref.
  Check `is_valid()` when absence matters.
- **OBJ indices are resolved.** 1-based and negative/relative OBJ indices are both converted to 0-based here; a missing corner attribute is `-1`.
- **OBJ is faithful, not a mesh.** No triangulation, no dedup — polygons stay polygons. `usemtl` / `o` / `g` / `s` are recorded (or skipped), not applied.
- **Errors carry an offset / line.** JSON errors report the byte offset; OBJ errors report the line number. Both come back as a `cc::result` error.

## Umbrellas

```cpp
#include <babel-serializer/data/json.hh>     // just JSON
#include <babel-serializer/geometry/obj.hh>  // just OBJ
#include <babel-serializer/all.hh>           // everything
```
