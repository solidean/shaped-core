---
created: 2026-08-26
refs:
  - file: tools/review/lib/annotate/providers.py
  - file: tools/review/lib/annotate/index.py
---

# Linking a symbol, not just a file

**Entries name symbols far more often than they name files**, and a file link gets a reader to the right file rather than to the declaration they were after.

The annotation pass is shaped around this: providers plug into it, three exist, and the fourth has somewhere to go.
What is missing is the index behind it, and two findings from the design review say why that is not simply "file linking, one step further".

## Ambiguity here is unfixable by the author

A file reference that resolves two ways is always fixable by writing a longer path, which is what lets `validate` fail on one.
A symbol has overloads, has the same name in three namespaces, and is declared in one file and defined in another.
The agent can disambiguate none of that by writing more.
So a symbol resolver has to be allowed to give up quietly and often, and the severity model the file provider uses does not carry over.

## A code span is mostly not a symbol

Entries are full of backticked things that look like identifiers and are not: `show: visible`, `--dirty-only`, `intro`, an option label, a front-matter key.
A matcher that links one of those is strictly worse than no matcher, because it teaches the reader that the underlines are noise.

**The tractable subset is qualified names only** — anything with a `::` or a `.` between two identifier segments.
Those are nearly unambiguous, and they are what an entry writes when it means a symbol; everything unqualified is left alone.
That drops most of the value and nearly all of the risk, which is the right trade for something nobody depends on.

## Where an index could come from

- *A compilation database.* Accurate, and it needs a configured build of the repository under review.
  That disqualifies it for a tool whose selling point is that it works on any checkout.
- *shaped-linter.* It already parses this repo's C++ without LLVM and could emit a declaration index as a side product.
  It is also shaped-core-specific, so it can only ever be an optional provider rather than the mechanism.
- *A ctags-style regex sweep.* Cheap, language-agnostic-ish, and wrong often enough to matter — silently.

The overfit is allowed: better than eighty per cent of reviews here are of shaped-core itself.
But it has to arrive as a provider with no default, chosen per review and possibly overridden per entry, rather than as something the pass assumes.
