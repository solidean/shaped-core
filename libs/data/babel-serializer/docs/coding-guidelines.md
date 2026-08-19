# babel-serializer coding guidelines

These build on the repo-wide [coding-guidelines](../../../../docs/coding-guidelines.md) — read that first; everything there still applies.
This document owns the **babel-specific** rules: the two library-wide shapes below, plus every place generic advice does not apply to babel for a non-obvious reason.
Other babel docs state a rule from here in one line and link back, never a second copy.
**Extend it as we go:** whenever a babel decision goes against generic advice for a reason that isn't obvious from the code, that's the signal to add the rule here.

---

## The two layers, and what binds both

Two layers, kept separate, and every rule below assumes them.

- **Format layer (native structures).** Each format parses into a data structure that resembles the format itself, unopinionated relative to it.
  A json `document` is a flat preorder node array; an obj `data` is parallel attribute arrays plus face corners.
  The format's own shape, never a cross-format vocabulary imposed on it.
- **Aggregator layer (opinionated).** On top sit entry points that dispatch across formats and return one convenient result — "load an image", "load a mesh".
  An aggregator delegates to the format readers and reaches no backend of its own.
  `babel::image` is the first; images invert the usual format-layer-first order deliberately, for the reason under the backend seam below.

Two rules bind both layers:

- **Read once, then query.** A parsed structure is cheap to traverse and query, deliberately awkward to mutate, and there is no insertion API.
  Writing gets a *separate* API: a json writer will not reuse the reader's `document`.
- **Reading takes a `cc::read_stream`.** Readers parse against the stream's buffered window (`ready_bytes` / `consume` / `flush`) rather than slurping the input first.
  `cc::string_view` and span overloads wrap a `span_read_stream_adapter`, and a seekable stream is requested only by a format that genuinely needs random access.
  The one documented deviation is below: a reader whose result must hand back views *of* its input takes bytes instead.

---

## Conditionally-shipped backend: the API is always present, absence is a runtime error

A format may depend on a third-party backend that is **fetched on demand and not committed** —
the SQLite engine (`extern/sqlite`, ~9.5 MB) is the first, following the Zydis / SDL3 model.
On a raw checkout, or when the user opts out (`SC_SKIP_SQLITE`), that backend is simply absent.

The rule for such a format: **the public API is always declared and always callable.**
Absence never removes a type, a member, or a free function from the header.
It surfaces as an ordinary runtime failure instead:

- a `bool is_available()` probe the caller can read, and
- every factory / entry point returns a `cc::result` **error** when the backend is missing —
  never a missing symbol, never a link error, never a crash.

**Why** (not obvious): IO is error-riddled by nature — a file may not exist, may be truncated, may be the wrong format.
"The engine for this format wasn't compiled into your build" is one more such runtime condition, so it rides the `cc::result` channel the caller already has to handle.
A *compile-time* condition instead would push `#if`s into user code and split the API into "present" and "absent" shapes.

### Where the compile switch is allowed to live

The switch that selects the real backend vs. the stub is a **`PRIVATE` compile definition on the
babel target**, set by `CMakeLists.txt`, and it may appear in **exactly one place**: the format's
`.cc` file (e.g. `BABEL_HAS_SQLITE` inside `data/sqlite.cc`), guarding real-vs-stub implementations
of the same always-declared signatures.

It must **never** appear in:

- a public header (`.hh`) — headers compile identically with or without the backend, and
  consumers never see the macro;
- user code — callers branch on the runtime `is_available()`, never on a macro;
- the umbrella / `fwd.hh` — the type set does not change with the backend.

**A third-party header never reaches a babel public header.** This binds every backend, gated or not.
Forward-declare the backend's opaque handles (`struct sqlite3;`) so its real header stays out of every `.hh`, and link the backend `PRIVATE`.
The `.cc` includes the real header only inside its `#if <backend>` branch.

### The stub path is real code, and it is tested

When the backend is absent the `.cc` still compiles a **complete stub**: every entry point defined, every infallible accessor returning a safe default.
The rest return the availability error, and it links, so the whole API resolves.
Tests for such a format **branch on `is_available()` at runtime — no `#if` in the test**.
One test pins the availability contract directly, since both build modes must satisfy it.
See `tests/data/sqlite-test.cc`.

---

## Committed-in-source backend: always linked, so absence is not a runtime state

Some backends are **committed in-source** rather than fetched.
The stb image libraries (`extern/stb`, two small public-domain headers) are the first, following the xxhash / imgui model.
Because the source is always on disk, the `stb` target always exists and babel **always** links it.

That drops the entire availability machinery the sqlite rule above carries.
**No `is_available()`, no `BABEL_HAS_*` compile switch, no stub path, no runtime "backend missing" error, no `if(TARGET stb)`.**
The link in `CMakeLists.txt` is unconditional (`target_link_libraries(babel-serializer PRIVATE stb)`).

**The header non-leak still binds**, with no availability gate carrying it: the stb headers appear in exactly one TU, `image/impl/stb_backend.cc`, and the link is `PRIVATE`.
Here the reason is layering hygiene and a swappable backend rather than a conditionally-present engine.

Pick the fetched-and-gated shape (the section above) only when the backend is genuinely heavy enough to keep out of the tree.
Default to committed-and-always-linked for a small self-contained dependency.

---

## The backend is a swappable seam; per-format codecs sit under an aggregator

stb is a **prototyping backend** — eventually most formats want a non-stb path.
So it is kept behind a backend-neutral seam (`babel::impl::stb_decode` / `stb_encode_*` in `image/impl/stb_backend.hh`, which names no stb type).
A future hand-rolled decoder replaces the body of one `impl::` function inside one `.cc` — no public signature moves.
The low-level codecs already parse each format's structural header natively (PNG IHDR, JPEG SOF/JFIF), so that native path has somewhere to grow.

Images also invert the usual "one native structure per format, aggregators later" order, deliberately.
Every image format decodes to the *same* packed pixel buffer, so:

- the **low-level** `babel::png` / `babel::jpg` are the format layer — pixels **plus** the format's own metadata (color type, gamma, ICC, EXIF, ...), much of it `[todo]` until the native walker lands;
- the **aggregator** `babel::image` is the opinionated "just give me pixels" layer, dispatching by format.
  It **delegates to the low-level codecs and never touches stb** — only the codecs reach the seam.

Reach for a low-level codec when you need a format's metadata; reach for the aggregator when you do not.

---

## A reader may take bytes instead of a stream — when the result must hand back views of the input

This is the deviation the stream rule at the top of this file points at.
**Take a `cc::pinned_data<byte const>` instead when the parsed structure has to hand back zero-copy views of the input bytes.**
`gltf` is the first such format, and the test is narrow: a `.glb` carries its vertex / index / texture payload inline.
So each `buffers` entry is a `subdata` of the input that shares its owner: nothing bulk is copied, and the views stay valid after the caller drops its own handle.
A stream cannot promise that — its window is a recycled buffer, so anything kept has to be copied out.

**Why this is not a defeat for the stream rule** (not obvious): parsing the *text* still goes through a stream.
The JSON chunk is a subspan handed to `babel::json::read`, and a span-backed stream is unbuffered — the span **is** the window.
So the parse is as inlined as passing the raw bytes would have been: the pinned input buys the payload's lifetime, not a different parser.

Such a reader still offers the full overload set, and each one documents its cost:

- `read(cc::pinned_data<byte const>, ...)` — the zero-copy entry point;
- `read(cc::read_stream&, ...)` — slurps, then **moves** the slurped buffer into a pin (`cc::make_pinned_data` on an rvalue copies no elements);
- `read(cc::span<byte const>, ...)` / `read(cc::string_view, ...)` — a span is a borrow, so these pin an **owned copy** and say so in the `///` doc.

Reach for this shape only for the embedded-payload case.
A text format has nothing to hand back a view of, and should stay on the plain stream.

---

## `read_options` is the reader counterpart to `write_options`

A reader that needs knobs takes a `read_options` struct **by value, with defaults that touch nothing outside the input bytes** —
mirroring the writer convention below.

Injected I/O travels as a `cc::function_ref`, not a path or a flag: `gltf::read_options::resolve_uri` is how a `.gltf` reaches its
external `.bin`, and the URI arrives exactly as the file spelled it.
**Why** (not obvious): babel owns no filesystem policy.
Percent-decoding, joining against a base directory, and deciding whether a relative path may escape it are all caller decisions.
A format reader that guessed them would be wrong for half its callers.
A default-constructed `function_ref` is invalid, so `is_valid()` is the natural "no resolver" sentinel — no extra flag.

Unresolvable is not an error: the reference is recorded (`uri` kept, `resolved == false`, empty bytes) and the *read* still succeeds.
Only a resolver that itself returns an error fails the read.

---

## A complex format's result carries an issue list, not just a `cc::result`

`cc::result` answers one question: did the read produce a usable structure?
For a large, extension-riddled, reference-heavy format that is not enough, because the interesting outcomes are neither
success nor failure — **we skipped a feature the file uses**, **we could not follow a reference**, **the file is sloppy and we
tolerated it**. All three return a perfectly good structure that is *not everything the file described*, and a caller that only
checks `has_value()` will never know.

So such a format's `data` carries a `cc::vector<issue>`, appended in the order the reader noticed things, with a kind:

- **`unsupported`** — the file uses something this reader does not implement.
  Nothing is wrong with the file; the gap is ours.
- **`unresolved`** — a reference the reader could not follow (an external URI with no resolver). The data is absent.
- **`malformed`** — the file violates the spec in a way we chose to tolerate; the named property fell back to its default.

**The rule that keeps this honest: an issue and an error are mutually exclusive.**
Anything that would make the returned structure *wrong* stays a `cc::result` error — a glTF sparse accessor, an unknown `componentType`.
Anything the caller can act on but survive without is an issue.
Never both, and never an issue *instead of* an error for something that changes how bytes are interpreted.

Two consequences worth stating, because they are what make the list trustworthy:

- **Every tolerated fallback emits one.** A silently defaulted value is indistinguishable from the file having said so, which is precisely the bug class the list exists to kill.
  This is also why a tolerant enum mapper returns `cc::optional` and lets the *call site* record the issue.
  Only the call site knows which element it was reading, and the message must name it.
- **A clean file yields an empty list.** `has_issues()` on a file the reader fully understands must be false, so tests assert it
  and callers can treat non-empty as "show this to the user".

Reach for this when a format has features worth skipping or references worth failing to follow.
A format where every input is either fully understood or an error (JSON, base64) needs no issue list, and adding one would be noise.

---

## Index-heavy formats: one strong enum per index role

The repo-wide rule against `-1` sentinels ([coding-guidelines](../../../../docs/coding-guidelines.md)) bites hardest in a format whose cross-references are bare JSON integers.
glTF has ten distinct index roles, and `primitive.indices` and `primitive.material` are both `int` in the file — as plain `i32` members they would swap without a diagnostic.
So each role gets `enum class <role>_index : int { invalid = -1 };`, and `invalid` reads as "the file left this property out".

**babel's own flattening offsets stay plain `i32`.** `primitive::first_attribute` / `attribute_count` are not a format
cross-reference; they are this library's run encoding, exactly as in `obj::face::first_corner`, and they are never absent.
That is also why `obj` is not a counter-example to the rule: it has a single index role, so there is nothing to confuse.

Pair the enums with a `find(<role>_index) -> Element const*` overload set and validate every stored index once, after parsing.
Then `nullptr` carries exactly one meaning — absent, never out-of-range — which is what makes the accessors safe to use without re-checking.
The validation pass has a second job: `cc::pinned_data::subdata` **asserts** on an out-of-range range and `subdata_clamped` silently truncates.
So neither is a validation channel, and every slice site needs a bounds check that produces a `cc::error` before it.

---

## The writer convention (established by images, babel's first writer)

Images are babel's first format to **write**.
The pair every future writer should mirror:

- `encode(...) -> cc::result<cc::vector<cc::byte>>` — encode to an in-memory blob;
- `write(cc::write_stream& out, ...) -> cc::result<cc::unit>` — encode, then write to a stream (`write` is `encode` + `out.write(...)`, so the two never diverge).

Encoder tuning travels in a per-format `write_options` struct (e.g. `jpg::write_options{ int quality }`), passed by value with sensible defaults.
A writer never reuses the reader's native structure as an input contract beyond the fields it needs; metadata the backend cannot emit is silently ignored and documented.
