# glossary

**Every term the PR and the review use, defined once.**

*Applies: whenever the change introduces named concepts — a new type, a coined noun, a word used in a narrower sense than usual.
Skip it only for a change that adds no vocabulary at all.*

## What it is for

A change that is worth reviewing almost always coins vocabulary, and the review then inherits it.
Twelve entries all lean on the same four nouns, each entry defining them again badly or not at all — and the maintainer,
who reads entries in whatever order the nav offers, meets a term for the "first" time in three different places.

So the terms get one home, early, and every other entry is free to use them without re-explaining.

**This does not replace the concepts paragraph in [orientation](orientation.md).**
That paragraph is prose, it names two or three ideas, and its job is to put the reader in the right frame.
The glossary is a *lookup*: alphabetical, complete, and read by jumping into rather than by reading through.
One is the shape of the change; the other is the vocabulary of it.

## What goes in it

- **Terms the change coins.** A new type, a new noun, a new file kind.
  These are the reason the entry exists.
- **Terms it uses in a narrower sense than the word normally carries.**
  These are the dangerous ones: a reader who thinks they know the word does not stop to check.
- **Terms the *review* needs** that the change did not coin — a repository convention, a git concept the findings turn on.
- **Terms that were renamed**, with the old name, because the maintainer's memory holds the old one.

Each entry is a sentence or two, and names the symbol it exists as.
Define what the reader could be **wrong** about — where a term's boundary is, what it is *not*, what it is often confused with.

## What to leave out

- Anything a competent reader of this repository already knows.
  A glossary that defines "commit" trains people to skip it.
- **Restating a signature.**
  Link the symbol; the [api-surface](api-surface.md) entry is where shape lives.
- **Terms used exactly once, in one entry.**
  Define those where they are used.

## Shape

**Mark the block `glossary: true`**, and the tool underlines every term wherever another entry uses it, with the definition on hover and a click through to here.
A term with irregular forms takes an alias list — `**atom** (atomic, atoms) — …` — since the matcher's plurals are naive.
`validate` reports a paragraph in the block that does not parse as `**term** — definition`, which is the whole reason the block is marked rather than scraped.

Numbered `018`, right after the generated `015-changes`, so it is met before any finding.
Alphabetical within the block — a lookup that is ordered by importance is not a lookup.

Usually **no ask at all**, and that is fine: an entry can exist to be read.
Where there is one, the useful question is whether a name is right, since naming is cheap to change now and expensive later.

Do not attach `changes` blocks.

## Example

```markdown
---
id: 018
title: glossary
group: meta
state: open
---

## prose

**atom** — one unit of change the review must account for: an added line, a removed line, or one file-level fact
no line can express (`lib/space/netspace.py`).
Not a hunk, and not a diff line: a context line is not an atom, which is why the denominator is what it is.

**claim** — the set of atoms one change accounts for.
Always a display hunk's span intersected with net space, which is what drops context lines without arithmetic.

**discharge** — an `ask` naming the change ids it answers for.
Sits on the ask rather than on the entry, so one LGTM ask can account for fifty hunks.
Distinct from *coverage*: a change can have an id and still be undischarged.
```
