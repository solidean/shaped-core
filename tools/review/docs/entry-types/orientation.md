# orientation

**The first entry, and the only one written to be read before any finding.**

*Applies: always, when there is a changeset.*

## What it is for

The maintainer opens a review knowing roughly what they asked for and nothing about what arrived.
Every other entry assumes that gap is already closed — a finding about a line map is unreadable to someone who does not yet know the tool has one.

So this entry puts the reader in the right frame: what the change is, what it claims about itself, the concepts the rest of the review will lean on, and what the reviewer went looking for.
It is the entry that makes the rest skimmable.

It is **not** a summary of the diff.
`015-changes` is generated and already carries the range, the commits and where they land.

## What goes in it

- **What this change does**, in three or four sentences, in the reviewer's own words rather than the PR body's.
  Writing it in your own words is the point: a reviewer who cannot state the change plainly has not understood it yet, and that is worth finding out here.
- **What it claims.** The PR body's own promises, named so the review can check them.
  This is where "the page has not been exercised by a human" belongs — a claim the author made that the review should test.
- **The concepts.** Two or three ideas a reader needs before the findings make sense, each in a sentence.
  Name them with the symbols they exist as.
- **What I went looking for.** The shape of the pass: what was read in full, what was skimmed, what was accepted wholesale and why.
  This is the honest half, and it is what makes "I found three bugs" a claim with a scope.

## Shape

`context/cold` and `context/repo` earn their keep here more than anywhere else, because this is the entry a reader with no background actually starts from.

An `ask` is optional and usually worth having: **is this the change you thought you were getting?**
A no here is worth knowing before the maintainer spends an hour on findings about the wrong thing.

Do not attach `changes` blocks.
This entry discharges nothing; the findings do.

## Example skeleton

```markdown
---
id: 010
title: what this change is
group: meta
state: open
---

## context/cold

For a reader new to both the change and the repository.

## prose

**What it does.** ...

**What it claims.** The PR body says ... — the review checks each of these.

**Concepts.** `foo::bar` is ...; a *widget* is ...

**What I went looking for.** Read every line of `lib/`; skimmed the assets; accepted the vendored drop wholesale because ...

## ask  right-change

Is this the change you thought you were getting?

- radio: yes, review it as it stands  (recommended)
- radio: no — say what is missing and I will re-scope before writing findings
```
