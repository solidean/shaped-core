# babel-serializer

Serialization and deserialization of various formats.
Namespace `babel`. Depends on **clean-core** (streams, containers, `result`) and **typed-geometry** (`vec` / `pos` / `mat` / `quat` for the geometry formats).

```cpp
#include <babel-serializer/data/json.hh>

auto const doc = babel::json::read(R"({"name": "shaped", "tags": [1, 2, 3]})").value();
auto const name = doc.root()["name"].as_string();  // "shaped"
auto const tag0 = doc.root()["tags"][0].as_double(); // 1
```

Headers are included by their full path from `src/`, e.g. `#include <babel-serializer/geometry/obj.hh>`.
Each format lives in its own sub-namespace (`babel::json`, `babel::obj`); `<babel-serializer/all.hh>` is the umbrella.

## One namespace, several link targets

`babel-serializer` is **the** target — link it when in doubt, and it carries every format below.
It is not the only one, because the namespace and the link graph answer different questions.

- **`babel-data`** is the base: the formats that are pure parsing and need nothing but clean-core (base64, JSON, markdown).
  Its whole contract is that dependency list, so a consumer that wants JSON does not link an image decoder, a database engine or typed-geometry to get it.
  That is why `data/` itself spans two targets — `sqlite` sits above because of what it *needs*, not because of what it *is*.
- **`babel-serializer`** is everything else, and links `babel-data` publicly.

Spin off a third only when a format arrives carrying a dependency nobody should pay for by accident — a cursed vendor SDK, a heavyweight decoder — and give it the same treatment.
**The namespace never splits with the targets**: `babel::json` is `babel::json` wherever it is linked from, and `fwd.hh` stays single.

This library is at an **early stage**.
A base64 codec, a JSON reader and writer, a markdown block reader, a live SQLite handle, OBJ and glTF/GLB readers, PNG/JPG image read+write, and a Chrome Trace writer exist so far.
See [docs/structure.md](docs/structure.md) for what is `[done]` vs `[planned]`.

## Design at a glance

The three rules a caller notices first.
[docs/coding-guidelines.md](docs/coding-guidelines.md) owns them, along with the rest of babel's own conventions.

- **Native structure first.** Each format parses into a data structure that resembles the format itself — unopinionated.
  Opinionated aggregators ("load an image", "load a mesh" across formats) sit *on top* of these; `babel::image` is the first.
- **Read once, then query.** A parsed document is optimized for traversal and queries, deliberately not for insertion.
  JSON is a flat node array, not a tree of allocating nodes; OBJ is parallel attribute arrays, not a built mesh.
  Writing gets a separate API.
- **Reading takes a `cc::read_stream`**, parsed against its buffered window rather than slurping the input first.
  `cc::string_view` and byte-span overloads wrap a `span_read_stream_adapter`.
  The one deviation is a format whose result must hand back views *of* its input: `gltf` takes a `cc::pinned_data<byte const>` instead.

## File organization

Source lives in `src/babel-serializer/`, grouped by topic:

| Folder      | What's in it |
|-------------|--------------|
| (root)      | `fwd.hh` (forward decls + vocabulary aliases), `all.hh` (umbrella) |
| `data/`     | `base64` — the base64 codec (`decode` / `decode_into` / `decoded_size` / `encode`); `json` — the JSON reader (`document` / `node` / `ref`, `read`); `markdown` — the block-level markdown reader (same `document` / `ref` shape); `sqlite` — a live SQLite handle (`database` / `statement` / `row`) |
| `geometry/` | `obj` — the Wavefront OBJ reader (`data` / `corner` / `face` / `group`, `read`); `gltf` — the glTF 2.0 / GLB reader (`data` + `accessor_view`, `read` over pinned bytes) |
| `image/`    | `png` / `jpg` — low-level image codecs (pixels + native metadata, `read` / `encode` / `write`); `image` — the "just the pixels" aggregator (`read` auto-detects, `encode` / `write` by format) |
| `trace/`    | `chrome_trace` — writes a `cc::rec::recording` as Chrome Trace Event JSON, for `chrome://tracing` and `ui.perfetto.dev` (`encode` / `write`) |

`sqlite` deviates on purpose: SQLite is a live database *engine*, so it is a thin RAII wrapper over an open connection rather than a one-shot parser.
Open a file / `:memory:` / a byte image, `exec` / `query`, iterate rows — full read/write.
Its engine backend is fetched on demand and may be absent, so probe `is_available()`; the API is declared and callable either way.

`image` is babel's first **writer** and first **committed** third-party backend.
The per-format `png` / `jpg` codecs read *and* write, exposing each format's own metadata (much of it still `[todo]`); `image` sits on top for the plain-pixels case.
The backend is the vendored **stb** single-file libraries, always in-tree and always linked — so no availability probe, unlike sqlite.

`gltf` deviates on its **input**, for zero copy: every embedded buffer comes back as a subview sharing the input's owner instead of a copy.
Its stream and span overloads exist for convenience and own a copy, which their `///` docs say outright.
External `.bin` / image URIs resolve through a caller-supplied `read_options::resolve_uri` callback, and an unresolved reference is recorded rather than an error.
It is also the first format whose result carries an **import-issue list**, `data::issues` — check it before assuming you got everything the file described.

## Building & testing

Build and test through the repo driver — never run the `babel-serializer-test` binary directly:

```bash
uv run dev.py test              # build + run the full suite
uv run dev.py test "json -"     # just the JSON tests while iterating
uv run dev.py test "markdown -"
uv run dev.py test "base64 -"
uv run dev.py test "sqlite -"
uv run dev.py test "obj -"
uv run dev.py test "gltf -"
uv run dev.py test "image -"    # the aggregator; the codecs are "png -" and "jpg -"
```

See [building-and-testing](../../../docs/guides/building-and-testing.md) for the full workflow.

## More

- [cheat-sheet.md](cheat-sheet.md) — the public API at a glance.
- [docs/_index.md](docs/_index.md) — babel-serializer's documentation hub.
- [docs/structure.md](docs/structure.md) — the format roadmap (`[done]` / `[planned]`).
- [docs/lower-library-gaps.md](docs/lower-library-gaps.md) — what babel wants from clean-core / typed-geometry that isn't there yet.
- [coding-guidelines](../../../docs/coding-guidelines.md) — conventions all shaped-core code follows.
