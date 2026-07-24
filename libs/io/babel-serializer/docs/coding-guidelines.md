# babel-serializer coding guidelines

These build on the repo-wide [coding-guidelines](../../../../docs/coding-guidelines.md) — read
that first; everything there still applies. This document only captures the **babel-specific**
rules and the places where generic advice does *not* apply to babel for non-obvious reasons.

It is intentionally thin for now. **Extend it as we go:** whenever a babel decision goes against
generic advice for a reason that isn't obvious from the code, that's the signal to add the rule here.

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

**Why** (not obvious): IO is error-riddled by nature — a file may not exist, may be truncated,
may be the wrong format. "The engine for this format wasn't compiled into your build" is one more
such runtime condition, not a categorically different one, so it rides the same `cc::result`
channel the caller already has to handle. Making it a *compile-time* condition instead would push
`#if`s into user code and split the API into "present" and "absent" shapes — exactly what a caller
should never have to reason about.

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

The header keeps the third-party headers out too: forward-declare the backend's opaque handles
(`struct sqlite3;`) so the real backend header stays out of `.hh` and babel links the backend
`PRIVATE`. The `.cc` includes the real header only inside its `#if <backend>` branch.

### The stub path is real code, and it is tested

When the backend is absent the `.cc` still compiles a **complete stub**: every entry point defined,
returning the availability error (or a safe default for the infallible accessors). It links, so the
whole API resolves. Tests for such a format **branch on `is_available()` at runtime — no `#if` in
the test** — and include one test that pins the availability contract directly (both build modes
must satisfy it). See `tests/data/sqlite-test.cc`.

---

## Committed-in-source backend: always linked, so absence is not a runtime state

Some backends are **committed in-source** rather than fetched.
The stb image libraries (`extern/stb`, two small public-domain headers) are the first, following the xxhash / imgui model.
Because the source is always on disk, the `stb` target always exists and babel **always** links it.

That drops the entire availability machinery the sqlite rule above carries: **no `is_available()`, no `BABEL_HAS_*` compile switch, no stub path, no runtime "backend missing" error, no `if(TARGET stb)`**.
The link in `CMakeLists.txt` is unconditional (`target_link_libraries(babel-serializer PRIVATE stb)`).

**What still binds is the header non-leak.**
The third-party headers must not reach a babel public header: `image/png.hh` / `jpg.hh` / `image.hh` include no `stb_*.h`.
The stb headers appear in exactly one TU — `image/impl/stb_backend.cc` — and the backend is linked `PRIVATE`.
So a consumer of babel never sees stb, exactly as with sqlite; only the *reason* differs (here it is layering hygiene and a swappable backend, not a conditionally-present engine).

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
- the **aggregator** `babel::image` is the opinionated "just give me pixels" layer — it dispatches by format and **delegates to the low-level codecs, never touching stb** (only the codecs reach the seam).

Reach for a low-level codec when you need a format's metadata; reach for the aggregator when you do not.

---

## The writer convention (established by images, babel's first writer)

Images are babel's first format to **write**.
The pair every future writer should mirror:

- `encode(...) -> cc::result<cc::vector<cc::byte>>` — encode to an in-memory blob;
- `write(cc::write_stream& out, ...) -> cc::result<cc::unit>` — encode, then write to a stream (`write` is `encode` + `out.write(...)`, so the two never diverge).

Encoder tuning travels in a per-format `write_options` struct (e.g. `jpg::write_options{ int quality }`), passed by value with sensible defaults.
A writer never reuses the reader's native structure as an input contract beyond the fields it needs; metadata the backend cannot emit is silently ignored and documented.
