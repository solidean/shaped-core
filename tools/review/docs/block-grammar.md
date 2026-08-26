# The entry block grammar

What an agent writes and what the page renders, so it is the contract between them.
It is also what a maintainer edits by hand when that is faster, which is why every rule below is about being hard to corrupt rather than about being expressive.

## Shape

An entry is front matter plus a sequence of `## <type>` blocks.

```markdown
---
id: 040
title: stale index survives table growth
group: correctness
state: open
severity: bug
---

## context/delta

What this entry adds over the ones before it.

## changes  CHANGE-7Q2M CHANGE-K3PP
show: collapsed

Optional commentary on those hunks.

## ask  split-vs-serialize
discharges: CHANGE-7Q2M

Should the completion signal split per resource?

- radio: split per resource, one fence each  (recommended)
- radio: keep one fence, serialize submission
- check: add a regression test
```

Blocks do not nest and are not fenced.
So an agent writing one cannot leave the file unbalanced; the worst it can do is name a type that does not exist, and that is an error with a line number.

## Front matter

`id` and `title` are required.
`group`, `state`, `severity` and `resolved-by` are known; anything else is preserved verbatim, so a review can carry fields the tool has no opinion on.

`state` is `open`, `obsolete` or `superseded`.
`severity` is `bug`, `design`, `api`, `docs`, `nit`, `question` or `lgtm`.

Filenames are `NNN-slug.md` with gaps — `010`, `020`, `030` — so a later round inserts `045` without renumbering anything.

## Block types

| type | takes | what it is |
|---|---|---|
| `context/cold` | — | for a reader new to the change *and* the codebase; collapsed by default, ~150 words |
| `context/repo` | — | knows the codebase, new to the change; collapsed by default, ~120 words |
| `context/delta` | — | what this entry adds over the previous ones; always shown |
| `auto-acknowledge` | — | this entry is reference material, so reading it is not something to record |

All three are required on every entry whose group is not `meta`, `finalize` or `framing`, and `validate` reports a missing one as an error.
An entry is answered on its own, out of order, by someone not carrying the changeset in their head, and the tiers are what make that possible.
Each is scoped to that entry's subject rather than to the change as a whole — otherwise every cold tier restates the same paragraph and nobody opens one again.

## Acknowledgement

An entry that poses no `ask` still has to be *seen*, so the page appends one synthetic question to it: a checkbox reading `Read and acknowledged`.
It is filed under the answer key `acknowledged`, counts toward progress like any other answer, and never appears in the file — no entry has to remember to write one.
Without it an entry nobody has opened is indistinguishable from one that is settled, and the progress count says the review is further along than anyone is.

`## auto-acknowledge` opts out, for material that is consulted rather than read through — a glossary, a generated listing.
It is a block rather than a front-matter key on purpose: an unknown front-matter key is preserved verbatim, so a typo would silently do nothing,
while a misspelled block type is an error with a line number.
That is the same reason the attribute whitelist exists.
| `prose` | — | the body of the point, written neutrally |
| `code` | — | a code sample; fenced blocks inside it are highlighted |
| `changes` | change ids | the hunks under discussion, rendered with their diffs; `show:` is required |
| `recommendation` | — | the agent's opinion, visually separated from the neutral description |
| `ask` | a name | the answerable question |

The word limits warn rather than fail.
They exist because a collapsed tier nobody can skim is a tier nobody opens.

### `show:` is required on a `changes` block

`show: visible` opens the diffs; `show: collapsed` puts them one click away.
There is no default, on purpose: a default would make the quiet choice the unconsidered one, and this choice is about the reader's attention rather than about formatting.

The question to answer is **can this entry be decided without the code?**
Where the prose and the evidence already settle it, the diff is depth material for a reader who wants to go further, and it is `collapsed`.
Where the reader genuinely cannot judge the point without seeing the hunk, it is `visible`.

`collapsed` is the common answer, and a review whose every `changes` block is `visible` has not asked the question.
A screen of diff costs scrolling on every visit to that entry, and human attention is the scarcest thing the tool spends.

## The attribute prelude

A block body opens with a run of `key: value` lines, starting at the body's **first** line.

Three conditions must hold together for a line to be an attribute: prelude position, a lowercase-kebab key, and membership in **that block type's** whitelist.
That is what keeps prose containing a colon from being eaten.

A key that looks like an attribute but is not whitelisted is an **error** with a did-you-mean, never silently prose.
A typo'd `discharge:` degrading into a sentence — dropping the discharge without saying so — is exactly the failure this grammar exists to prevent.

A body whose first line is blank has no prelude at all.
That is the escape hatch for prose that genuinely must start with `something:`.

## Asks

The ask is the answerable unit, and **discharge sits on it rather than on the entry**.
One entry can therefore carry five questions discharging five different change sets, which is what keeps an LGTM entry from costing one click per hunk.

- The **name** is the answer key, unique within the entry, lowercase with dashes.
- `discharges:` lists the change ids this question accounts for.
- `follows:` names the earlier ask this is a follow-up to.
- Option lines are `- radio:`, `- check:` or `- rank:`; a trailing `(recommended)` is recognised and shown as a badge.
- A freeform text box is **always** added by the server, never authored.
  Forgetting it is not possible.

### A finalized ask is immutable

Once an answer to it has been frozen into a round, the question's wording is fixed.
Reword it and the tool refuses, naming the remedy: add `## ask <new-name>` with `follows: <old>` and ask the follow-up there.

The hash behind that covers the ask's name, its prose, and its ordered options — what the maintainer actually saw.
It excludes `round:` and `discharges:`, so stamping a round or widening a discharge set can never orphan an answer that was already given.

## Rounds

Every block is stamped `round: N` when the tool next touches the review.
Blocks written after a round is finalized carry the next round, and every earlier block keeps its own, so the page can draw a divider between what the maintainer has read and what is new.

Stamping is a **splice** against the parsed block spans, applied last-to-first.
The tool never re-serializes an entry.
A writer that normalized whitespace would change an ask's text, and then report the maintainer's finalized question as modified —
the tool accusing them of an edit the tool itself made.

## Generated blocks

A block carrying `generated: <key>` is the tool's to rewrite, and nothing else in an entry ever is.
`review generate` replaces exactly those blocks, so commentary written directly underneath one survives every refresh.

Only the overview and the coverage report are generated, and the coverage entry's `title:` is refreshed along with its block, since it carries a live percentage.
Every other entry is authored end to end: judging what matters in a change is the most useful thing an entry can carry, and nothing mechanical produces it.
