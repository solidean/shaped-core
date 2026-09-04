# babel-data

The externals-free base of babel: base64, JSON and markdown.
Namespace `babel`. Depends on **clean-core** and nothing else.

```cpp
#include <babel-data/data/json.hh>

auto const doc = babel::json::read(R"({"name": "shaped", "tags": [1, 2, 3]})").value();
auto const name = doc.root()["name"].as_string();  // "shaped"
auto const tag0 = doc.root()["tags"][0].as_double(); // 1
```

Headers are included by their full path from `src/`, e.g. `#include <babel-data/data/markdown.hh>`.
Each format lives in its own sub-namespace (`babel::base64`, `babel::json`, `babel::markdown`); `<babel-data/all.hh>` is the umbrella.

## One namespace, two libraries

babel is one namespace across two libraries, and the split is by **dependency** rather than by topic.

- **`babel-data`** — the formats that are pure parsing and need nothing but clean-core.
  That dependency list is the library's whole contract, so a consumer that wants JSON does not link an image decoder, a database engine or typed-geometry to get it.
  It is why `data/` spans both libraries: `sqlite` lives next door because of what it *needs*, not because of what it *is*.
- **[`babel-serializer`](../babel-serializer/readme.md)** — everything else, and the one to link when in doubt.
  It links babel-data publicly, so linking it still gets `babel::json`.

Spin off a third only when a format arrives carrying a dependency nobody should pay for by accident — a cursed vendor SDK, a heavyweight decoder — and give it the same treatment.

**The namespace never splits with the libraries**: `babel::json` is `babel::json` wherever it is linked from.
`fwd.hh` **layers rather than forks** — this library's declares `namespace babel` itself plus the formats here, and babel-serializer's includes it and adds the rest.
So does the recording domain set: `babel`, `babel.json` and `babel.markdown` are defined in this library's `fwd.cc`, the formats above it in babel-serializer's.

## Design at a glance

babel's conventions are one set across both libraries, and [babel-serializer's docs/coding-guidelines.md](../babel-serializer/docs/coding-guidelines.md) owns them.
The two a caller of this library notices first:

- **Native structure first.** Each format parses into a data structure that resembles the format itself — unopinionated.
- **Read once, then query.** A parsed document is optimized for traversal and queries, deliberately not for insertion.
  JSON is a flat node array, not a tree of allocating nodes; markdown shares that exact shape, so knowing one reader is knowing the other.
  Writing gets a separate API — `babel::json::writer` is the one here.

**Reading takes a `cc::read_stream`**, parsed against its buffered window rather than slurping the input first.
`cc::string_view` and byte-span overloads wrap a `span_read_stream_adapter`.
base64 is the exception, and deliberately: data URIs and embedded blobs are already in memory, so it has no streaming interface at all.

## File organization

Source lives in `src/babel-data/`:

| File | What's in it |
|------|--------------|
| `fwd.hh` / `all.hh` | forward decls + vocabulary aliases; the umbrella |
| `data/base64` | the base64 codec (`decode` / `decode_into` / `decoded_size` / `encode`) |
| `data/json` | the JSON reader (`document` / `node` / `ref`, `read`) and writer (`writer`, `string_writer`) |
| `data/markdown` | the block-level markdown reader (same `document` / `ref` shape) |

## Building & testing

Build and test through the repo driver — never run the `babel-data-test` binary directly:

```bash
uv run dev.py test              # build + run the full suite
uv run dev.py test "json -"     # just the JSON tests while iterating
uv run dev.py test "markdown -"
uv run dev.py test "base64 -"
```

See [building-and-testing](../../../docs/guides/building-and-testing.md) for the full workflow.

## More

- [cheat-sheet.md](cheat-sheet.md) — the public API at a glance.
- [babel-serializer](../babel-serializer/readme.md) — the rest of babel, and where its shared docs live.
- [coding-guidelines](../../../docs/coding-guidelines.md) — conventions all shaped-core code follows.
