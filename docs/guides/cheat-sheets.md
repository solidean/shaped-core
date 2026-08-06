# Cheat sheets

A cheat sheet is a fast-recall map of a library's public API.
Every important symbol gets a one-line trailing comment giving its return type and intuition, and a closing section lists the gotchas people and agents actually trip on.
It answers "what's the call, and what do I pass it?" in a single scan.
It is **not** a tutorial or a design doc — the header `///` comments and [coding-guidelines](../coding-guidelines.md) are where the *why* lives.
Back to [guides](_index.md).

## Where they live

Each library has exactly one cheat sheet, at a fixed path:

```
libs/<category>/<lib>/cheat-sheet.md
```

The two that matter on almost every task:

- [clean-core](../../libs/base/clean-core/cheat-sheet.md) — replaces most `std::` usage: `cc::vector`, `cc::string`, `cc::optional`, `cc::result`, ….
- [nexus](../../libs/base/nexus/cheat-sheet.md) — how we write tests: `TEST`, `CHECK`, `SECTION`.
  nexus has no `readme.md`, so its sheet is the library's front door too.

Before doing code work, skim the cheat sheet for the library you're touching and its most important dependencies.
[CLAUDE.md](../../CLAUDE.md) makes that a standing habit.

## Format

- **Header block** — title, a one-line purpose, then namespace, dependencies and include convention.
- **Topic sections** — one fenced `cpp` block per topic.
  Lead with the `#include`, then list symbols **one per line**, each with a trailing `// …` comment giving the return type and/or a one-line intuition.
  Show the real names, the key constructors and factories, and the most-used members — not the whole header.
- **Gotchas section** at the end — the sharp edges: ownership, move-only types, zero and empty handling, signed vs unsigned, when something is gated off, what is still a stub.

A topic block looks like this:

```cpp
#include <clean-core/error/optional.hh>   // cc::optional<T> — value | cc::nullopt (no operator* / ->)
cc::optional<int> o = 42;
o.has_value();  o.value();                // value() ASSERTS when empty (no exception)
o.value_or(fallback);                     // value or fallback
```

## Rules that keep them trustworthy

- **Code-faithful.** Every symbol must exist in the headers with that exact name and signature.
  Verify against the source while writing: a cheat sheet that lies is worse than none.
- **Real API only.** Don't document stubs or not-yet-implemented types as if they work.
  Omit them, or note them in one line under Gotchas.
- **Keep it current.** A cheat sheet *is* public API surface, so the repo's docs rule applies — when a change touches the public API, update the sheet in the same change.

## Related

- [_index.md](_index.md) — the other guides.
- [coding-guidelines.md](../coding-guidelines.md) — where a symbol's *why* lives.
