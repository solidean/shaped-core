# babel-serializer

Serialization and deserialization of various formats.
Namespace `babel`. Depends on **clean-core** (streams, containers, `result`) and **typed-geometry** (`vec` / `pos` for the geometry formats).

```cpp
#include <babel-serializer/data/json.hh>

auto const doc = babel::json::read(R"({"name": "shaped", "tags": [1, 2, 3]})").value();
auto const name = doc.root()["name"].as_string();  // "shaped"
auto const tag0 = doc.root()["tags"][0].as_double(); // 1
```

Headers are included by their full path from `src/`, e.g. `#include <babel-serializer/geometry/obj.hh>`.
Each format lives in its own sub-namespace (`babel::json`, `babel::obj`); `<babel-serializer/all.hh>` is the umbrella.

This library is at an **early stage** — a JSON reader, an OBJ reader, and a live SQLite handle exist so far.
See [docs/structure.md](docs/structure.md) for what is `[done]` vs `[planned]`.

## Design at a glance

- **Native structure first.** Each format parses into a data structure that resembles the format itself — unopinionated.
  Opinionated aggregators ("load an image", "load a mesh" across formats) sit *on top* of these, later.
- **Read once, then query.** A parsed document is optimized for traversal and queries, deliberately not for insertion.
  JSON is a flat node array, not a tree of allocating nodes; OBJ is parallel attribute arrays, not a built mesh.
  Writing, when it lands, gets a separate API.
- **Reading takes a `cc::read_stream`.** The readers parse straight against the stream's buffered window
  (`ready_bytes` / `consume` / `flush`) — the buffering is inlined in the caller, so they never slurp the whole input first.
  Convenience overloads accept a `cc::string_view` or a byte span (they wrap a `span_read_stream_adapter`).

## File organization

Source lives in `src/babel-serializer/`, grouped by topic:

| Folder      | What's in it |
|-------------|--------------|
| (root)      | `fwd.hh` (forward decls + vocabulary aliases), `all.hh` (umbrella) |
| `data/`     | `json` — the JSON reader (`document` / `node` / `ref`, `read`); `sqlite` — a live SQLite handle (`database` / `statement` / `row`) |
| `geometry/` | `obj` — the Wavefront OBJ reader (`data` / `corner` / `face` / `group`, `read`) |

`sqlite` deviates from the "reader over a `cc::read_stream`" shape on purpose: SQLite is a live database *engine*, so it is a
thin RAII wrapper over an open connection (open a file / `:memory:` / a byte image, `exec` / `query`, iterate rows), full read/write.
Its engine backend is fetched on demand and may be absent — the API stays always-callable, reporting absence at runtime via `is_available()`.
See [docs/coding-guidelines.md](docs/coding-guidelines.md) for that always-available-API convention.

## Building & testing

Build and test through the repo driver — never run the `babel-serializer-test` binary directly:

```bash
uv run dev.py test            # build + run the full suite
uv run dev.py test "json -"   # just the JSON tests while iterating
uv run dev.py test "obj -"    # just the OBJ tests
```

See [building-and-testing](../../../docs/guides/building-and-testing.md) for the full workflow.

## More

- [cheat-sheet.md](cheat-sheet.md) — the public API at a glance.
- [docs/_index.md](docs/_index.md) — babel-serializer's documentation hub.
- [docs/structure.md](docs/structure.md) — the format roadmap (`[done]` / `[planned]`).
- [coding-guidelines](../../../docs/coding-guidelines.md) — conventions all shaped-core code follows.
