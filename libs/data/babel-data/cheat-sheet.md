# babel-data cheat sheet

base64, JSON and markdown — the externals-free base of babel.
Namespace `babel`; headers included by full path from `src/`.
> Each format parses into an unopinionated, read-once structure, and reading takes a `cc::read_stream`.
> base64 is the deviation: no streaming interface, because data URIs and embedded blobs are already in memory.
> [babel-serializer's docs/coding-guidelines.md](../babel-serializer/docs/coding-guidelines.md) owns both rules, for both libraries.

```cpp
#include <babel-data/all.hh>   // umbrella (base64 + json + markdown)
```

---

**Recording domain:** `babel` — each format shadows it: `babel.json`, `babel.markdown`.
Every `CC_LOG_*` and `CC_RECORD_*` site in this library is attributed to it; see [logging](../../base/clean-core/docs/logging.md).

## base64 (`babel::base64`)

Tolerant on input, canonical on output.
No streaming interface — data URIs and embedded blobs are already in memory.

```cpp
#include <babel-data/data/base64.hh>

cc::optional<isize> decoded_size(cc::string_view text);              // nullopt when text is not valid base64
cc::result<cc::vector<cc::byte>> decode(cc::string_view text);       // fresh buffer
cc::result<isize> decode_into(cc::string_view text, cc::span<cc::byte> out); // -> bytes written; short out is an error
cc::string encode(cc::span<cc::byte const> bytes);                   // standard alphabet, '=' padded

babel::base64::decode("Zm9vYmFy").value();  // "foobar"
babel::base64::decode("-_-_").value();      // URL-safe alphabet decodes too
```

## JSON (`babel::json`)

```cpp
#include <babel-data/data/json.hh>

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
- `dev.py example babel-data/json-write` runs one example.
  The others are `json-read`, `json-imperative`, `json-write-stream`, `json-write-numbers`, `json-newline-delimited` and `json-round-trip`.

## Markdown (`babel::markdown`)

Block level only — the same flat `document` / `ref` shape as JSON.
Inline spans (emphasis, links, code spans) are **not** parsed.

```cpp
#include <babel-data/data/markdown.hh>

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

## Gotchas

Only what the signatures above cannot tell you.

- **`ref` borrows the document.** It holds a `document*`, so keep the `document` alive while traversing.
  `as_string()` / `key()` return views into the document's arena, with the same lifetime.
- **Kind-tolerant accessors never fail on shape**, so check `is_valid()` when absence is what you care about.
  `r["missing"]`, `r[99]` and `r.as_double()` on a string each return a fallback or an invalid ref instead.
- **JSON errors carry a byte offset**, as a `cc::result` error.
- **base64 rejects a lone trailing character.** `"Zm9vY"` is a final quantum encoding no byte at all, so it is an error rather than a truncation.
- **Markdown never fails on content.** Every input is a valid document, so its `cc::result` reports stream I/O failure only.
  An unterminated fence simply runs to end of input.
- **Markdown `line()` is what makes a corpus debuggable**: a failing case points at a file and line rather than at a string literal.

## Umbrellas

```cpp
#include <babel-data/data/base64.hh>   // just base64
#include <babel-data/data/json.hh>     // just JSON
#include <babel-data/data/markdown.hh> // just markdown
#include <babel-data/all.hh>           // all three
```

The formats above these — SQLite, OBJ, STL, glTF, the image codecs, Chrome Trace — live in [babel-serializer](../babel-serializer/cheat-sheet.md).
