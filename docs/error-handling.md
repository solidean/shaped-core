# Error handling

How shaped-core decides between **assertions**, **`cc::result` / `cc::optional`**, and **exceptions**.
This is the repo-wide authority; the [coding guidelines](coding-guidelines.md) carry only the summary
table and defer here.

The three mechanisms are not interchangeable styles — each answers a different question about *who caused
the failure and who can recover from it*. Picking the wrong one is a real defect: an assertion on a
recoverable condition crashes a correct program in release; a `result` on a programmer bug pushes ceremony
onto every call site that can't act on it.

## The three mechanisms

| Mechanism                 | For                                                              | Compiled out in release? |
|---------------------------|-----------------------------------------------------------------|--------------------------|
| `CC_ASSERT`               | **Contract violations** — the programmer used the API wrong     | **Yes**                  |
| `cc::result` / `optional` | **Frequent or expected failures** you can handle **locally**    | No                       |
| Exceptions                | **Exceptional failures** that must **bubble up** (non-local)    | No                       |

## Assertions — for broken contracts only

`CC_ASSERT(cond, msg)` states an **invariant that a correct program never violates**: a precondition, a
postcondition, an internal consistency check. It means *"the programmer used this wrong,"* not *"something
went wrong."*

Because assertions are **compiled out in release** (see [build configurations](coding-guidelines.md) — the
only intended Debug/Release behavior difference), an assertion is a promise that the condition *cannot* fire
in correct code. That yields two hard rules:

- **Never assert on user input or environment.** A file that doesn't parse, a shader that doesn't compile,
  a socket that drops, an allocation the OS refuses, a device that resets — none of these are contract
  violations. A correct program *must* keep working when they happen, and a release build with assertions
  compiled out still has to handle them. So they are **not** assertions. (They are `result` or an exception
  — see below.) "User" here is broad: the calling programmer's *arguments* can violate a contract, but data
  and conditions from outside the program never do.
- **Assertions must be side-effect free.** The checked expression must not be load-bearing for correctness —
  the program must behave identically with `CC_ASSERT` compiled out. (Temporary debug output inside a custom
  handler is fine.)

The test: *could a correct caller have avoided this by reading the docs and checking a cheap precondition?*
If yes, and stating it as a contract is cheap → assert. If the failure is inherent to the operation no
matter how correctly it's called (I/O, allocation, compilation, device state) → not an assert.

Assert **liberally** for genuine contracts — cheap precondition checks on public API are a feature, not
noise (they turn silent misuse into an immediate, located failure). A library is expected to *test* that its
public API actually detects these violations (`CHECK_ASSERTS` / `CHECK_THROWS` in nexus), so validation
isn't quietly dropped.

## `cc::result` / `cc::optional` — for expected, locally handled failures

Reach for `cc::result<T>` (carries an error) or `cc::optional<T>` (just "absent") when the failure is
**expected, potentially frequent, and something the immediate caller can act on** — and you are in a context
where throwing is unwanted (hot paths, `noexcept` code, code that must stay usable without exceptions).

- `cc::optional<T>` when *absence* is the whole story (a lookup miss, a "does this fit?" probe).
- `cc::result<T>` when the caller wants to know *why* it failed.

**When in doubt, `result` is the safe default.** It is never *wrong*, only sometimes verbose: a `result`
can always be escalated later — `.value()` turns an unhandled error into an assertion/abort, and a thin
wrapper can turn it into a throw. You can start with `result` and add a throwing façade without touching the
core (see the pattern below). The reverse — retrofitting a `result` return onto a function that asserts or
throws — ripples through every call site.

The cost `result` imposes is **ceremony at every call site that can't act on the error** (the `.value()` /
propagation fatigue). So don't reflexively return `result` for a failure no caller will branch on — that
failure wants an assertion (if it's a bug) or an exception (if it's exceptional but non-local). The rule of
thumb: *return `result` only when you can name a caller that branches on it.*

## Exceptions — for exceptional, non-local failures

Exceptions are for failures that are **infrequent**, **must propagate up** past several frames that can't do
anything useful, and yet **can ultimately be handled** somewhere. The canonical shape: a resource allocation
deep in a call tree fails, and the only party who can recover is a coarse subsystem far above (an asset
budget, a streaming manager, a "rebuild the device" handler).

- **Handleable, not fatal.** If nobody can recover — a truly impossible state — that's a bug: assert, or
  abort with a diagnostic. Exceptions are for failures with a real (if distant) handler.
- **Non-local by nature.** If the immediate caller handles it, that's a `result`, not an exception.
- **Infrequent.** Exceptions are not control flow. A failure that happens on a hot path or routinely is a
  `result`.
- **Typed and informative.** A library defines a small, purpose-built set of exception types carrying the
  extra context a handler needs (not a single opaque type, not raw `std::` exceptions).

Crucially, a failure being *recoverable-but-not-locally* is exactly why device resets and allocation
failures are **exceptions, not assertions**: "the program must keep working" rules assertions out, but a
handler exists (higher up), so they're not merely fatal either.

## Pattern: fallible core, throwing façade (`try_*` + throwing default)

When an operation can fail in a way most callers won't handle but some must, offer **both**, without
duplicating logic:

- The **fallible core** returns `cc::result` / `optional` and never throws (`try_do_thing`). All the real
  work — and, for a virtual/backend interface, the *only* method backends implement — lives here.
- The **default façade** wraps the core and **throws** on error (`do_thing`). It is a thin, non-virtual
  forwarder: call `try_`, return the value, throw the error.

```cpp
// fallible core — the caller who has a fallback uses this; never throws
[[nodiscard]] cc::result<buffer> try_create_buffer(buffer_desc const&);

// throwing default — clean call sites for the common case with no local recovery
[[nodiscard]] buffer create_buffer(buffer_desc const& d)
{
    auto r = try_create_buffer(d);
    if (r.is_error())
        throw some_alloc_exception(...);   // typed, carries the desc / reason
    return cc::move(r.value());
}
```

This gives clean ergonomics by default (no `.value()` on every create) while keeping the library **usable
without exceptions**: a caller that never wants to catch simply stays on the `try_*` surface. Requiring some
care to go exception-free is acceptable; making it the default is not.

Keep the two channels distinct. A `try_*` returning "absent"/error must mean a **recoverable** failure the
caller can retry or route around (out of budget, doesn't fit). A **sticky, global** condition like a lost
device is *not* that — it should surface through its own status/channel checked where recovery happens
(e.g. at submit/present), so a caller retrying on a capacity failure doesn't spin forever on a dead device.

## Choosing — a short decision guide

1. Is this a **contract violation** — a correct caller could have prevented it with a cheap precondition?
   → **`CC_ASSERT`**. (And it must be side-effect free and safe to compile out.)
2. Otherwise the program must survive it. Can the **immediate caller** handle it, and/or is it frequent /
   on a path that must not throw? → **`cc::result` / `optional`**.
3. Otherwise it must **bubble up** to a distant handler, is infrequent, but *can* be handled there →
   **exception** (typed).
4. Genuinely unrecoverable anywhere → abort with a great diagnostic (a failed assert-style fatal), not a
   silently-ignored error.

When steps 2 and 3 both look plausible, prefer the **fallible core + throwing façade** above so you don't
have to choose for the caller.

## See also

- [coding guidelines](coding-guidelines.md) — the summary table and the surrounding conventions
  (assertions side-effect free, Debug/Release parity).
- [clean-core cheat-sheet](../libs/base/clean-core/cheat-sheet.md) — `cc::result`, `cc::optional`, and
  `CC_ASSERT` at a glance.
