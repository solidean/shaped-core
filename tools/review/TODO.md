# review — TODO

Ideas agreed but not built.
Each one is here because it was decided in a session and would otherwise be lost; none is a commitment to a design.

The known *gaps* — remote and mobile answering, the unimplemented `rank:` option kind, the missing offline form — live in the [readme](readme.md#not-yet) instead.
This file is for work that would add something the tool cannot do at all.

## Linking a symbol, not just a file

**Entries name symbols far more often than they name files**, and a file link gets a reader to the right file rather than to the declaration they were after.

The annotation pass is shaped around this: providers plug into it, three exist, and the fourth has somewhere to go.
What is missing is the index behind it, and two findings from the design review say why that is not simply "file linking, one step further".

**Ambiguity here is unfixable by the author.**
A file reference that resolves two ways is always fixable by writing a longer path, which is what lets `validate` fail on one.
A symbol has overloads, has the same name in three namespaces, and is declared in one file and defined in another — none of which the agent can disambiguate by writing more.
So a symbol resolver has to be allowed to give up quietly and often, and the severity model the file provider uses does not carry over.

**A code span is mostly not a symbol.**
Entries are full of backticked things that look like identifiers and are not: `show: visible`, `--dirty-only`, `context/cold`, an option label, a front-matter key.
A matcher that links one of those is strictly worse than no matcher, because it teaches the reader that the underlines are noise.

**The tractable subset is qualified names only** — anything with a `::` or a `.` between two identifier segments.
Those are nearly unambiguous, they are what an entry writes when it means a symbol, and everything unqualified is left alone.
That drops most of the value and nearly all of the risk, which is the right trade for something nobody depends on.

### Where an index could come from

*A compilation database.* Accurate, and it needs a configured build of the repository under review — disqualifying for a tool whose selling point is that it works on any checkout.

*shaped-linter.* It already parses this repo's C++ without LLVM and could emit a declaration index as a side product.
It is also shaped-core-specific, so it can only ever be an optional provider rather than the mechanism.

*A ctags-style regex sweep.* Cheap, language-agnostic-ish, and wrong often enough to matter — silently.

The overfit is allowed: better than eighty per cent of reviews here are of shaped-core itself.
But it has to arrive as a provider with no default, chosen per review and possibly overridden per entry, rather than as something the pass assumes.

## A semantic peek

Hovering a file reference shows a window: up through the comment block above the line, down by the review's context setting.
That rule is mechanical and good enough to be worth having.

**What a reader actually wants is the enclosing declaration with its doc comment**, which is the symbol index under a different name.
Recorded here so that whoever builds the index knows it has a second customer.

## Questions, as something the tool tracks

Comments are remarks: the agent answers one by appending a block with `addresses:`, and nothing new is tracked.

**A question expects an answer back**, which the current shape does not model.
The agent replies in prose and the maintainer reads it in the entry, with nothing saying the reply is owed or has arrived.
That was deliberate: the tool has exactly one notion of something outstanding, and a second one means a second definition of "still open" and a second way for `status` to be wrong.

Revisit once comments have been used on a few reviews.
If the appended-ask path turns out to be enough, this stays closed; if maintainers keep asking questions that go unanswered, it is a real gap.

## Making a review durable

A review folder is scratch under `.tmp/`, and the tool changes under it with no migration promise.
That is right while the tool is being built and wrong for a review anyone wants to keep.

**An export mode** would turn one into a durable artifact: the entries, the answers, the rounds and the attachments, in a form that does not depend on this version of the tool.
What that form is — a single markdown file, a static page, a zip — is undecided, and so is whether it can be read back.

## Provenance

Everything this file used to hold — comments anywhere, superseding a block, an identity for a review — was designed and built on 2026-08-26.
What is left came out of that design review as the parts deliberately not built, plus one thing the review itself turned up.
