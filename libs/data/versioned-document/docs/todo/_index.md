# Milestones

The ordered plan to build [versioned-document](../../readme.md) and [versioned-document-file](../../../versioned-document-file/readme.md).
Both libraries are built to one plan, so both sets of milestones live here.

Read [concept.md](../concept.md) before starting any of them, and [decisions.md](../decisions.md) before proposing anything that contradicts one.

## The rule that binds every milestone

**The full design is built, and partial implementation is never a licence to simplify it.**

"Nothing uses it yet" is a statement about the calendar, not about the design.
It is not a reason to drop a layer, collapse a seam, skip a case, or replace a general mechanism with the special case that happens to be needed this week.

A design cut down while it is half-built is not a smaller design — it is a broken one, and the breakage shows up a year later as a format that cannot express what it was designed to express.
That is why the whole thing is specified before any of it is written.

If a milestone's design turns out to be *wrong*, say so and change it deliberately, in [decisions.md](../decisions.md).
That is a completely different act from quietly building less.

## Order

Each milestone depends on the ones before it.
Within a milestone, the work items are roughly ordered but need not be done in sequence.

| # | Milestone | Where |
|---|-----------|-------|
| 0 | [Prerequisites](milestone-0.md) **[done]** — `cc::blake3`, `cc::interned_string`, `babel::sqlite` additions | clean-core, extern, babel-serializer |
| 1 | [The value codec](milestone-1.md) **[done]** — canonical binary values, byte equality | versioned-document |
| 2 | [Ids, ops and the DAG](milestone-2.md) **[done]** — content addressing, materialization, multi-values | versioned-document |
| 3 | [Raw and typed documents](milestone-3.md) **[done]** — interpretation, the immutable index | versioned-document |
| 4 | [The file](milestone-4.md) **[done]** — schema, load, publish, workspace, the actor | versioned-document-file |
| 5 | [Assets and blobs](milestone-5.md) — the content store and its sharing | versioned-document-file |
| 6 | [Snapshots, pruning and recovery](milestone-6.md) — and the docs' final pass | both |

## What "done" means

A milestone is done when every acceptance criterion in its file holds, and:

- `uv run dev.py check --fix` passes;
- its tests are in the library's `*-test` binary and run in the normal suite;
- [structure.md](../structure.md)'s tags for the pieces it landed are flipped from `[planned]`;
- the affected cheat-sheet entries have lost their `[planned]` marking;
- anything discovered that contradicts the design has been written down, in [decisions.md](../decisions.md), rather than absorbed silently.
