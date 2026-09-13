# Error handling

How shaped-core decides between **assertions**, **`cc::result` / `cc::optional`**, and **exceptions**.
This is the repo-wide authority; the [coding guidelines](coding-guidelines.md) carry only the summary
table and defer here.

The three mechanisms are not interchangeable styles — each answers a different question about *who caused the failure and who can recover from it*.
Picking the wrong one is a real defect.
An assertion on a recoverable condition crashes a correct program in release; a `result` on a programmer bug pushes ceremony onto every call site that cannot act on it.

## The three mechanisms

| Mechanism                 | For                                                              | Compiled out in release? |
|---------------------------|-----------------------------------------------------------------|--------------------------|
| `CC_ASSERT`               | **Contract violations** — the programmer used the API wrong     | **Yes**                  |
| `cc::result` / `optional` | **Frequent or expected failures** you can handle **locally**    | No                       |
| Exceptions                | **Exceptional failures** that must **bubble up** (non-local)    | No                       |

## Assertions — for broken contracts only

`CC_ASSERT(cond, msg)` states an **invariant that a correct program never violates**: a precondition, a postcondition, an internal consistency check.
It means *"the programmer used this wrong"*, not *"something went wrong"*.

Because assertions are **compiled out in release**, an assertion is a promise that the condition *cannot* fire in correct code.
That is the only intended Debug/Release behavior difference — see [build configurations](coding-guidelines.md).
It yields two hard rules:

- **Never assert on user input or environment.**
  A file that doesn't parse, a shader that doesn't compile, a socket that drops, an allocation the OS refuses, a device that resets — none of these are contract violations.
  A correct program *must* keep working when they happen, and a release build with assertions compiled out still has to handle them.
  So they are **not** assertions; they are a `result` or an exception, per the sections below.
  "User" here is broad: the calling programmer's *arguments* can violate a contract, but data and conditions from outside the program never do.
- **Assertions must be side-effect free.**
  The checked expression must not be load-bearing for correctness — the program must behave identically with `CC_ASSERT` compiled out.
  Temporary debug output inside a custom handler is fine.

The test: *could a correct caller have avoided this by reading the docs and checking a cheap precondition?*
If yes, and stating it as a contract is cheap → assert.
If the failure is inherent to the operation no matter how correctly it is called — I/O, allocation, compilation, device state — then it is not an assert.

Assert **liberally** for genuine contracts.
Cheap precondition checks on public API are a feature rather than noise: they turn silent misuse into an immediate, located failure.
A library is expected to *test* that its public API actually detects these violations (`CHECK_ASSERTS` / `CHECK_THROWS` in nexus), so validation is not quietly dropped.

## `cc::result` / `cc::optional` — for expected, locally handled failures

Reach for `cc::result<T>` (carries an error) or `cc::optional<T>` (just "absent") when the failure is **expected, potentially frequent, and something the immediate caller can act on**.
The same choice applies wherever throwing is unwanted: hot paths, `noexcept` code, code that must stay usable without exceptions.

- `cc::optional<T>` when *absence* is the whole story (a lookup miss, a "does this fit?" probe).
- `cc::result<T>` when the caller wants to know *why* it failed.
- `cc::result<T>` is `cc::result<T, cc::any_error>`; the error type defaults to `cc::any_error`, which is what the propagation idioms below are built on.

**When in doubt, `result` is the safe default.**
It is never *wrong*, only sometimes verbose — and it can always be escalated later, since a thin wrapper turns it into a throw.
You can start with `result` and add a throwing façade without touching the core, per the pattern below.
The reverse, retrofitting a `result` return onto a function that asserts or throws, ripples through every call site.

The cost `result` imposes is **ceremony at every call site that cannot act on the error** — the `.value()` and propagation fatigue.
So don't reflexively return `result` for a failure no caller will branch on: that failure wants an assertion if it is a bug, or an exception if it is exceptional but non-local.
The rule of thumb: *return `result` only when you can name a caller that branches on it.*

### Propagating an error, and enriching it on the way

`cc::error(...)` is the only sanctioned way to *return* a failure, so success stays the default path and no overload silently reads an error as a value.
The default error type `cc::any_error` is move-only and register-sized, and captures its source location — optionally a stacktrace too.

```cpp
cc::result<mesh> load_mesh(cc::string_view path)
{
    auto bytes = read_file(path);
    CC_RETURN_IF_ERROR(bytes).with_context("loading mesh");   // early-return; keeps the inner error's site
    return parse_mesh(bytes.value());                         // safe: bytes has a value here
}
```
`CC_RETURN_IF_ERROR(r)` is the propagation idiom: it evaluates `r` exactly once, returns early when `r.has_error()`, and allows a trailing `.with_context("…")` on that return.
Context is **additive** — every frame that propagates can name what it was doing, and `to_string()` renders the whole chain against the original site.
On a `result` you can also use `with_context_lazy`, which takes a callable so a message that costs something to build is only built on the error path.
It is not available on the macro's return expression, which carries an `as_error_t` rather than a `result`.

Getting the value out has four forms, and they are **not** interchangeable:

| Call | On error |
|---|---|
| `value()` | `CC_ASSERT` — aborts wherever assertions are on, which is every default preset, but it is compiled out in release, so there it is **undefined behavior** |
| `value_assert(msg)` | asserts in **every** configuration (`CC_ASSERTS_ALWAYS`): the release-safe abort |
| `value_or(fallback)` | returns the fallback |
| `or_throw<Exception>()` | throws `cc::result_exception`, carrying the `any_error`; a custom `Exception` must provide a static `from_error` |

**`value()` is for a result you have already checked**, never for asserting that it succeeded.
Where an unhandled error must be fatal, `value_assert` says so and survives the release build; where it must escalate, `or_throw` does.

## Exceptions — for exceptional, non-local failures

Exceptions are for failures that are **infrequent**, **must propagate up** past several frames that can't do anything useful, and yet **can ultimately be handled** somewhere.
The canonical shape: a resource allocation deep in a call tree fails, and the only party who can recover is a coarse subsystem far above.
That is an asset budget, a streaming manager, a "rebuild the device" handler.

- **Handleable, not fatal.** If nobody can recover, because the state is truly impossible, that is a bug: assert, or abort with a diagnostic.
  Exceptions are for failures with a real, if distant, handler.
- **Non-local by nature.** If the immediate caller handles it, that is a `result`, not an exception.
- **Infrequent.** Exceptions are not control flow, so a failure that happens on a hot path or routinely is a `result`.
- **Typed and informative.** A library defines a small, purpose-built set of exception types carrying the extra context a handler needs — not a single opaque type, and not raw `std::` exceptions.

Crucially, a failure being *recoverable-but-not-locally* is exactly why device resets and allocation
failures are **exceptions, not assertions**: "the program must keep working" rules assertions out, but a
handler exists (higher up), so they're not merely fatal either.

## Pattern: fallible core, throwing façade (`try_*` + throwing default)

When an operation can fail in a way most callers won't handle but some must, offer **both**, without
duplicating logic:

- The **fallible core** returns `cc::result` / `optional` and never throws (`try_do_thing`).
  All the real work lives here — and for a virtual or backend interface, it is the *only* method backends implement.
- The **default façade** wraps the core and **throws** on error (`do_thing`).
  It is a thin, non-virtual forwarder: call `try_`, return the value, throw the error.

```cpp
// fallible core — the caller who has a fallback uses this; never throws
[[nodiscard]] cc::result<buffer> try_create_buffer(buffer_desc const&);

// throwing default — clean call sites for the common case with no local recovery
[[nodiscard]] buffer create_buffer(buffer_desc const& d)
{
    auto r = try_create_buffer(d);
    if (r.has_error())
        throw some_alloc_exception(...);   // typed, carries the desc / reason
    return cc::move(r).value();
}
```

A façade that does not need its own typed exception is one line: `return try_create_buffer(d).or_throw();`.

This gives clean ergonomics by default — no `.value()` on every create — while keeping the library **usable without exceptions**.
A caller that never wants to catch simply stays on the `try_*` surface.
Requiring some care to go exception-free is acceptable; making it the default is not.

### Offer both only where both are real

The pattern earns its keep when **some** callers genuinely handle the failure.
Where none can, publishing the fallible core is worse than not: it implies a choice that does not exist, and every call site then carries a branch that only ever goes one way.

sg's resource creation is the worked example.
A buffer, texture, heap or binding group that cannot be created has failed for want of memory, and a caller has no fallback.
So those expose **one** spelling, `create_*`, and the fallible core stays private.
It still throws where a backend knows synchronously, and that is **best-effort rather than a contract**.
A backend whose creation is asynchronous (WebGPU) cannot know, and reports on the deferred channel below instead.

Pipelines are the counter-example in the same file, and the difference is evidence rather than taste.
sg's own pipeline cache *does* act on a build failure, turning it into a failed async so a render routine reports `failed` instead of throwing on a worker.
A caller that demonstrably acts on the error keeps the fallible form.

## Deferred errors: failures that arrive after the call

Everything above assumes the answer is available when the call returns.
Some backends cannot promise that — WebGPU reports a bad buffer or a failed pipeline through a promise that settles long after the call, and a validation layer speaks up whenever it notices.

`ctx.take_pending_errors()` is that channel: a list of `sg::device_error`, drained once a frame, in the order the backend saw them.
Entries accumulate until taken, so a caller that never asks grows a list rather than losing anything.
The kinds are deliberately coarse — `device_lost`, `creation_failed`, `validation` — because a caller branching on a fine-grained cause is a caller behaving differently per backend.

Device loss is reported here too, so a frame loop that drains this channel never also has to poll `is_device_lost()`.

Keep the two channels distinct.
A `try_*` returning "absent" or an error must mean a **recoverable** failure the caller can retry or route around — out of budget, doesn't fit.
A **sticky, global** condition like a lost device is *not* that.
It surfaces through its own status channel, checked where recovery happens (at submit or present), so a caller retrying on a capacity failure doesn't spin forever on a dead device.

## Choosing — a short decision guide

1. Is this a **contract violation** — a correct caller could have prevented it with a cheap precondition?
   → **`CC_ASSERT`**, side-effect free and safe to compile out.
2. Otherwise the program must survive it.
   Can the **immediate caller** handle it, or is it frequent, or on a path that must not throw?
   → **`cc::result` / `optional`**.
3. Otherwise it must **bubble up** to a distant handler, is infrequent, but *can* be handled there.
   → **exception** (typed).
4. Genuinely unrecoverable anywhere → abort with a great diagnostic, a failed assert-style fatal, rather than a silently-ignored error.

When steps 2 and 3 both look plausible, prefer the **fallible core + throwing façade** above, so you don't have to choose for the caller.

## See also

- [coding guidelines](coding-guidelines.md) — the summary table and the surrounding conventions.
- [clean-core cheat-sheet](../libs/base/clean-core/cheat-sheet.md) — `cc::result`, `cc::optional` and `CC_ASSERT` at a glance.
- [result.hh](../libs/base/clean-core/src/clean-core/error/result.hh) — the API itself: `any_error`, `with_context`, `or_throw`, `CC_RETURN_IF_ERROR`.
