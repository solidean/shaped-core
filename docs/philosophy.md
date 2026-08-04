# philosophy

A few guiding stars for shaped-core.
Not rules — `.clang-format` and [coding-guidelines.md](coding-guidelines.md) are the rules.
This is the *why* behind the taste, so that when a decision is a judgment call it lands the way the rest of the library lands.

## Who it's for

shaped-core is built **for our own tools first** — SOLIDEAN, internal tooling, customer projects, research — and offered to the wider community second.
We **dogfood essentially everything in this repo**: the libraries power the tools we ship, so the feedback loop is short and real, and a rough edge tends to get filed down because we hit it ourselves.

That ordering is a strength rather than a limitation.
Because our own use cases come first, we can be **opinionated** and make the tradeoffs we believe in.
Where a choice ends up different from what a general-purpose library would pick, it is usually because we weighed *our* priorities differently — a deliberate call, not an oversight.

## Optimize for the workload we actually have

Pick the primary workload deliberately and make it excellent.
A **secondary** workload is allowed to be second class — slower, or less convenient — as long as it stays **usable and correct**.
Making the common case great at the cost of the rare case is the trade we want, stated out loud.
"Works everywhere, best nowhere" is the anti-goal.

- Corollary: **name the primary workload in the design.** E.g. the node allocator is tuned for a
  *fixed set of long-lived threads*; thread-churning servers still work but pay more and get less.

## Measure, don't assume

Performance claims come from benchmarks and, where it matters, from the actual codegen (`dev.py assembly show`).
Numbers are hardware-dependent, so when a result is uarch-sensitive, **ship the variants as a benchmark**: it can then be re-checked on new hardware rather than trusted from one machine.

Two failure modes worth naming:

- **A *cause* is a claim too.** "X is slow because of Y" needs the same evidence as the number itself, so read the codegen or profile before you attribute it.
  Inferring the mechanism from the numbers alone is how a plausible-but-wrong explanation ships right next to a correct measurement.
- **Idealized microbenchmarks flatter.** A hand-written mock of a hot path lets the compiler hoist, constant-fold and DCE what the real code cannot.
  A slab base kept in a register, a branch the real access must pay.
  Put the *real* symbol in the benchmark beside the mocks and diff the disassembly; when they disagree, the mock is usually the optimistic one.

## Data structures over micro-tweaks

The big wins come from changing the shape of the data — a local/remote bitmap split, a better layout — not from shaving instructions off a bad structure.
Reach for the structural change first.

## Fail loud, not silent

Prefer a hard `CC_ASSERT` at a real boundary over silent degradation or undefined behavior.
A rare limit we have chosen to accept, such as a counter that only wraps after billions of events, is fine to **guard with an assert** as a loud backstop.
The tradeoff is then documented, and when it is hit it is obvious rather than mysterious.
[error-handling.md](error-handling.md) is where assert-versus-`result` is decided.

## The libraries are living

Every library is mid-growth.
When a task really wants a capability one layer down, **growing the lower library is a first-class option** rather than a detour: surface it, don't paper over it with a local hack.
A workaround is allowed but must be marked as one, naming the extension that would replace it.

## Keep it legible

Value types, explicit data flow, composition over deep inheritance; no hidden global state, no speculative abstraction, no god-objects.
Code should read like the code around it.
Comments say what types can't — invariants, preconditions, the surprising bit — not backstory.
