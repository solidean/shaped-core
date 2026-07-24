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
