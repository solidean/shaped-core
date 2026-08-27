# Entry types

Recurring kinds of entry, as instructions rather than as code.

The tool knows nothing about any of these.
An entry type is a document in this folder saying what the entry is for, when it applies, and how to write a good one —
so the set grows by writing prose, and a review can carry a type this tool has never heard of.

That is deliberate.
The block grammar is the contract between the agent and the page, and it has to be stable and small.
What a *good entry about an API change* looks like is taste, it differs per repository, and it changes as we review more —
so it belongs here, next to [reviewing-prs.md](../../../../docs/guides/reviewing-prs.md), rather than in `grammar.py`.

## How to use this folder

**Read this index every review.**
**Read a type in full only when it applies.**
The "when it applies" line under each type is what that decision is made on, and it is written to be decidable from the changeset alone.

A type is a template, not a schema.
Take its structure and its examples, and drop anything the change does not have.

## The types

- [orientation](orientation.md) — **what this change is, before any finding.**
  The first entry in every review with a changeset, written by the reviewer after reading the branch.
  What it does, what it claims, the concepts a reader needs, and what the reviewer went looking for.
  *Applies: always, when there is a changeset.*

- [glossary](glossary.md) — **every term the change and the review use, defined once.**
  A lookup, alphabetical and complete, so no other entry has to re-explain a noun.
  Distinct from orientation's concepts paragraph, which is the shape of the change rather than its vocabulary.
  *Applies: whenever the change coins names or uses a word in a narrower sense than usual — which is nearly always.*

- [api-surface](api-surface.md) — **the API this change moves, in symbols.**
  Signatures and a few lines of call-site code, not prose about signatures.
  *Applies: when the change adds, removes or reshapes a public symbol — a header, an exported type, a CLI surface.
  Skip it for a change that is entirely internal, or for a tool with no library API.*

- [example-evidence](example-evidence.md) — **what the examples actually printed, run rather than described.**
  The one type with a block of its own, because "quote the output" is a claim nobody downstream can check.
  *Applies: when the change adds examples, or reworks existing ones enough that a reader would want to see them run.*

- [verdict](verdict.md) — **the big picture, at the end.**
  Is the shape right, does it fit the repository's philosophy, would we want this in a year.
  The judgement that detail-oriented entries lose.
  *Applies: always, when there is a changeset — written last, after the findings are known.*

- [draft-artifact](draft-artifact.md) — **the comment you are about to post, shown as itself.**
  The maintainer approved each finding as it was raised; this is the first time they see them assembled, in order, at the length someone else will read.
  One ask, and it is a gate rather than a question.
  *Applies: a `pr-comment` or `land-changes` review, once the answers stop producing follow-ups.*

## Where they sit

The numbering is the reading order, and these three anchor it:

```
010-orientation.md     the reviewer's own framing        (authored)
015-changes.md         the range, its commits, its shape (generated)
018-glossary.md        the vocabulary                    (authored)
0xx-...                the findings                      (authored)
980-verdict.md         the big picture                   (authored)
985-draft-comment.md   the artifact, before it is posted (authored)
990-coverage.md        the gates                         (generated)
```

Findings go between, numbered with gaps so a later round can insert one.

## Adding a type

Write a file here, add it to the list above with a one-line *when it applies*, and keep both short.
A type nobody can decide the applicability of in one line is two types, or it is not a type.

The bar is that the type has come up in more than one review.
A structure invented for one changeset belongs in that entry, not in this folder.
