# Reviewing PRs

What we look for in a shaped-core review, and how we weigh the calls that have no right answer.
The mechanics — fetching the branch, the report structure, feeding this document — belong to the `reviewing-a-pr` skill; this is the taste behind them.

**This document is alive.**
It is written from actual reviews, and every entry that reads like a rule earned that status by being said out loud in one.
An entry with no example under it is a hypothesis, not a rule.
Reviews here happen in chat, point by point, precisely so the reviewer's answers can land back in this file.

## Who the review is for

The reviewer is in contact with the author, and a review speaks for both of them.
So the artifact is **a todo list for the author's next agent session**, not a politely framed request.

- **Write instructions, not suggestions.** "Delete the copy constructor" beats "you might consider making this non-copyable".
- **No softening, no hedging, no thanks-for-the-PR preamble.** The author expects this register and reads it faster.
- **Praise only where it carries information** — "this is the right shape, keep it" tells the next session not to churn it.
  "Great work overall" tells it nothing.
- The review lands as **one comment on the PR**, so it has to stand alone: someone reading only that comment must be able to act on every point.

## What is not a review finding

Cut these before writing.
They cost attention and return nothing.

- **"Does it build" and "do the tests pass."** CI runs ~14 configurations and `dev.py check --fix` runs four presets locally in full.
  Build the branch when you need to *verify a specific claim* — that two layouts really do hash differently, that a symbol really is gone — and then say what you learned, not that it compiled.
- **PR size and splitting.** PRs are ephemeral bookkeeping; only the git repo counts.
  Size matters only as "can this be reviewed", and with agent help that bar is high.
- **Compatibility, migration, deprecation.** The repo is beta through 2026 and likely most of 2027.
  Breaking things is cheap; the wrong shape is expensive.
- **Public API docs written in the intended tense.** See "Docs are for users first" below.
- **Restating the PR body.** The author wrote it.

## Ranked by what it costs to get wrong

### 1. API shape

Equal-first with correctness, and the thing most likely to be *permanent*.
A bug gets fixed in an hour; a type that carves the problem at the wrong joint outlives several rewrites of its body.

- **Is the API hard to hold wrong?** Copyability, move semantics, RAII, whether the mistake is even expressible.
  A value type that is silently copyable when copying duplicates authoritative state is a **bug**, not a nit.
- **Are the lifetimes in the type?** A bare `u32` whose validity is scoped to an epoch is a raw pointer with extra steps.
  Prefer a typed handle — `enum class : u32` when it needs no methods, a refcounted class when it owns a slot.
- **Is the guard on the right class?** A lock that protects a lifecycle belongs on whatever owns that lifecycle, not on one participant in it.
- **Does the abstraction pay for itself?** A "manager" that fixes a layout and hides what its consumer needs is the anti-pattern.
  A small helper over one thing the caller still owns is the pattern.
- **Which library does this belong in?** Dependency direction is a hard rule; "could live lower" is the more common finding.

Report API shape **in symbols**: signatures, the actual type names, and a few lines of call-site code.
Prose about an API is much harder to judge than the API.

### 2. Correctness the type system does not catch

- **Silent wrongness over loud failure.** A misuse that asserts is a footnote; a misuse that renders the wrong thing, or corrupts a table, is the finding.
- **Load-dependent failure is worse than deterministic failure.** A stale index that works until the table fills ships, then breaks in production under load.
- **Lifetime and aliasing.** Who keeps what alive, and can an address be reused while something still keys on it.
- **Ordering and epoch discipline.** GPU-side especially: what is still in flight when this mutates.
- **Assert vs `cc::result`** — see below.
  Picking the wrong one is a real finding in both directions.

### 3. Docs that have gone out of true

This is the **highest-value-per-effort** category, because it is exactly what the GitHub review UI cannot show.
A doc line is only wrong relative to the *final tree*, and the diff view shows it against the author's intent.
Hunt it deliberately.

**A doc claiming something the branch does not do is a defect, not a nit.**

Where they rot, in practice:

- **Leftovers from an earlier state of the branch.** A branch that added a thing and then removed it leaves the cheat-sheet line behind, still describing the removed thing.
  The diff shows that line as an *addition*, so it reads as intentional.
  Always diff the docs against the final tree, never against the PR body.
- **Stale identifiers after a rename** — a parameter renamed in the signature and still named by its old name in the `///` above it.
- **Structure and status tables** (`[done]` / `[in progress]` / `[planned]`) the change should have moved.
- **Cheat sheets** whose field lists no longer match the struct.

Verify each doc claim against the branch by looking up the symbol, not by reading the sentence.

### 4. Prose style and nits

`dev.py lint shaped` already gates the one-semantic-point-per-line rule, so a review rarely needs to.
Drive-by nits are welcome — collect them into a single trailing point so they never crowd out the above.

## Settled calls

Each of these was decided in a review; do not re-litigate them, and apply them as rules.

### Docs are for users first, implementors second

A public API doc may state the **intent** in the present tense even where one backend has not caught up.
"Every backend's `bind_group` asserts the slot matches" is correct writing when one of the backends is a non-recording stub.
The reader is a user of the API, the implication is "every usable backend", and whoever implements the stub has to match the sentence.
Do not file this.

The inverse still holds: a doc claiming a capability *no* backend has is wrong.

### Picking the wrong error mechanism is a real finding

[error-handling.md](../error-handling.md) is the authority, and it splits three ways rather than two.
**Exceptions have a genuine place here** — this is not a "no exceptions ever" codebase, and never review as if it were.

- `CC_ASSERT` is for **contract violations only** — the programmer used the API wrong.
  Assertions being off in `release-*`, with UB past the failed contract, is accepted: we work in `relwithdebinfo` most of the time and test heavily, so violations surface.
  Do not file "this is UB in release" for a plain contract assert.
- `cc::result` / `cc::optional` is for **expected failures the immediate caller can act on**, and anywhere throwing is unwanted.
- **Exceptions are for infrequent failures that must bubble past frames that cannot help, to a handler that exists further up** — a device reset, an allocation the subsystem above can recover from.
  A failure being recoverable but *not locally* is exactly what makes it an exception rather than an assert.

Two things to actually look for:

- **An assert on anything from outside the program** — a file, a shader, a device, an allocation.
  That is the clearest defect in this area, and it is common where a check was written backend-local and never lifted.
- **A fallible operation offering only one of the two surfaces.**
  The house pattern is a `try_*` fallible core plus a thin throwing façade, so a caller can go exception-free without ceremony everywhere else.
  A new `create_*` that only throws, or only returns a `result`, is worth naming.

### A 64-bit hash is not an identity

Using a `u64` hash *as* the identity — storing only the hash and skipping the comparison — needs extraordinary evidence.
The house sizes are `cc::hash128` for identity in non-adversarial settings, and `cc::hash256` where cryptographic guarantees are wanted.

A hash map keyed on the real value is the default, and it is not a cost worth avoiding.
`cc::map` already stores each node's finalized hash and short-circuits chain compares on it, so keying on the value costs one `operator==` per hash match.
When a type has a `hash()` hidden friend and no `operator==`, adding the operator over exactly the fields the hash folds is usually the whole fix.
Those two staying in agreement is then the invariant to state.

### Drive-by cleanups are welcome where the PR already is

The libraries are in flux, so cleanups get postponed by prioritization rather than by policy — fringe and niche APIs carry debt on purpose.
That is a schedule, not ossification.

So **asking to down-pay debt in code the PR is already touching is a good finding**, and worth making concrete: name the call sites and the replacement.
Asking to clean up code the PR does not touch is not.

### Portability floor: reject the non-portable thing on the dev box

Where a feature exists on the backend we develop against but not on a backend we intend to ship, we **fail everywhere, loudly, on the dev box** rather than let it surface later on the weaker target.
[concepts/views.md](../../libs/graphics/shaped-graphics/docs/concepts/views.md) is the worked precedent.
Storage-buffer offsets take WebGPU's 256-byte floor as a hardcoded portable rule, which "fails loudly on a dx12 dev box rather than surfacing later on WebGPU".
It carries a documented escape hatch for callers who knowingly target only the looser backends.

Two corollaries a review should check:

- **Keep the code paths.** Rejecting the feature at the API door is not the same as deleting the plumbing; the point is that conditional or full support later needs no redesign.
- **Say why, and where.** The rejection must point at the portability reason in a doc, not just assert "not supported yet".

### Commit messages are not evidence

Commit messages are agent-written and unreliable, especially about **provenance**.
A message saying "review feedback" is not proof a human decided anything.
Never let one close a question — raise the question anyway, and say where you saw it.

## Asking good questions

Questions are how this document grows, so they have to be answerable without the reviewer re-deriving the context.

- **Always name where you saw it** — file, line, the actual code.
- **Give the tradeoff, both sides**, and what it would cost to change.
- **Say what you would do**, so a one-word answer is possible.
- **Keep them few and separate** from the findings, at the end.

## Open questions

Things not yet ruled on.
Each becomes a rule above, an example, or a deletion.

- Where is the line between a design question worth raising and second-guessing a design the author already thought through?
  A partial answer so far: when a redesign is already the author's planned next step, the review's job is to keep this PR from cementing the old shape, not to specify the new one.
- How much of a proposed redesign belongs in the review comment itself versus a linked issue, once a finding turns into a multi-type API change?

## Reference

- [CLAUDE.md](../../CLAUDE.md) — hard rules, layering, and the style preferences a review assumes.
- [docs/coding-guidelines.md](../coding-guidelines.md) — design conventions and the prose rule.
- [docs/error-handling.md](../error-handling.md) — the assert / `cc::result` / exception split a review applies.
- [docs/guides/prose.md](prose.md) — the lint workflow behind the prose findings.
- the `reviewing-a-pr` skill — the flow and the report structure.
