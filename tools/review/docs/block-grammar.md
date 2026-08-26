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
| `artifact` | — | the exact text the review will publish, read back by `review artifact` and `review post` |
| `prose` | — | the body of the point, written neutrally; `glossary: true` makes its bold leads terms |
| `code` | — | a code sample; fenced blocks inside it are highlighted |
| `changes` | change ids | the hunks under discussion, rendered with their diffs; `show:` is required |
| `example` | a name | an example, the command that ran it, and what it printed |
| `recommendation` | — | the agent's opinion, visually separated from the neutral description |
| `ask` | a name | the answerable question |

The word limits on the context tiers warn rather than fail.
They exist because a collapsed tier nobody can skim is a tier nobody opens.

All three are required on every entry whose group is not `meta`, `finalize` or `framing`, for as long as it still has an ask waiting for an answer, and `validate` reports a missing one as an error.
An entry whose asks are all finalized is past the point of needing them, and adding tiers there would edit a question the maintainer has already read.
An entry is answered on its own, out of order, by someone not carrying the changeset in their head, and the tiers are what make that possible.
Each is scoped to that entry's subject rather than to the change as a whole — otherwise every cold tier restates the same paragraph and nobody opens one again.

An `artifact` block is markdown destined for somewhere else, so it is the one block whose body wants headings of its own.
**They have to start at `###`.**
`## ` at the start of a line is how a block begins, whatever the block it lands in, so a comment sectioned with `## ` fails to parse with "unknown block type" naming its first heading.
`###` is safe because a heading needs whitespace after the `##`, and it renders the same everywhere the comment is going.

**Inside a fenced code block, `## ` is prose.**
That is what lets an entry quote this format, which is what every entry in a review of this tool wants to do.
An unterminated fence is an error of its own, since the alternative is silently reading the rest of the entry as code.
Indenting a sample four spaces works too, and is the escape hatch where a fence is not wanted.

## Acknowledgement

An entry whose newest round added material but no `ask` still has to be *seen*, so the page appends one synthetic question to it: a checkbox reading `Read and acknowledged`.
It counts toward progress like any other answer and never appears in the file — no entry has to remember to write one.
Without it an entry nobody has opened is indistinguishable from one that is settled, and the progress count says the review is further along than anyone is.

**It is keyed per round**, as `acknowledged-r<N>`.
An entry can gain material in a later round without gaining a question — a redrafted artifact, a correction, a note.
One acknowledgement for the whole entry would already be answered from the round that did ask something.
The new material would then arrive under a tick earned by the old question.
An acknowledgement superseded by a later round is not treated as an orphan, since only the newest one is ever offered.

`## auto-acknowledge` opts out, for material that is consulted rather than read through — a glossary, a generated listing.
It is a block rather than a front-matter key on purpose: an unknown front-matter key is preserved verbatim, so a typo would silently do nothing,
while a misspelled block type is an error with a line number.
That is the same reason the attribute whitelist exists.

### `show:` is required on a `changes` block

`show: visible` opens the diffs; `show: collapsed` puts them one click away.
There is no default, on purpose: a default would make the quiet choice the unconsidered one, and this choice is about the reader's attention rather than about formatting.

The question to answer is **can this entry be decided without the code?**
Where the prose and the evidence already settle it, the diff is depth material for a reader who wants to go further, and it is `collapsed`.
Where the reader genuinely cannot judge the point without seeing the hunk, it is `visible`.

`collapsed` is the common answer, and a review whose every `changes` block is `visible` has not asked the question.
A screen of diff costs scrolling on every visit to that entry, and human attention is the scarcest thing the tool spends.

## Every block has a name

A block's identity is `<entry>/r<round>/<name>`, and it is **derived** rather than declared, so no entry ever has to be retrofitted with one.

The name is the block's type, indexed only when that type repeats within the same entry *and* round — and then all of them are indexed.
Two prose blocks in round 2 are `prose#1` and `prose#2`; one on its own is `prose`, never a bare `prose` beside a `prose#2`.
An `ask` is named by its heading, which is already unique.
A `context/cold` becomes `context-cold`, since the identity is slash-separated.

**`prose` and `prose#1` resolve to the same block.**
That alias is what keeps an anchor taken mid-round valid after a later append turns the round's only prose block into the first of two.

Ordinals are scoped to a round rather than to the file, which is what makes an anchor stable by construction.
Blocks are only ever appended and a frozen round cannot change, so a later block never renumbers an earlier one.

`name:` is optional, for a block the agent expects to point at later.
Two blocks of one round answering to the same name is a parse error, since a name is what a comment and a `supersedes:` anchor on.

`review show` prints each block's name beside its type, which is where an agent reads one off.

## Superseding a block

A partial round leaves earlier entries out of date, and the only two moves were appending a correction that buries itself at the bottom, or editing a block the maintainer has already read.

`supersedes:` retires one block without touching it.
Rounds stay immutable and the file stays append-only: the correction is a *new* block that names what it replaces.

```markdown
## prose
supersedes: r1/prose#2

The two remaining defects, ==now that the parser is fixed==.
```

- **Within one entry only**, enforced by resolution: a `supersedes:` naming nothing earlier in the same entry is a parse error.
  A correction that lands somewhere the reader is not is the problem this solves rather than a way to solve it.
- **A superseded block discharges nothing**, so a replaced ask cannot double-count.
- **An ask may be superseded only while it has never been answered.**
  Otherwise it walks around the immutability guard: the old question renders struck, the new one takes its place, and the answer sits under a question the maintainer never saw.
  The tool refuses, naming `follows:` as the remedy.
- The page shows both — the replacement, and the original struck and collapsed — because the maintainer needs to see what the entry says now and what it said when they read it.
  `review show` prints live blocks only; `--history` adds the chain.

**`==new==`** marks a span the page highlights, for a block re-stated in full with only its changed points drawn.
The case is a rephrase where three words moved: unreadable as a diff, dishonest as a silent replacement, and invisible to a maintainer who does not re-read an entry end to end before sending.

## Answering a comment

The maintainer can comment on any block, and on any line of a rendered diff.
A comment is never a tracked question — it is a remark, and the agent answers it in the next round by appending a block that names it.

```markdown
## prose
addresses: c1 c3

Both noted. The first needs no change; the second is `045`.
```

Outstanding is computed from those references the way an undischarged change is, never stored.
`validate` refuses to let a round be handed back while a comment from a finalized round has no `addresses:` anywhere.
A block that declines to act satisfies it, because the obligation is to answer rather than to comply.

Comments live in `answers/<entry>.json`, which the server owns, and are tentative until the round is finalized.

## Glossary blocks

`glossary: true` on a `prose` block says its bold leads are terms.

```markdown
## prose
glossary: true

**atom** (atoms) — one unit of change the review must account for.
Not a hunk, and not a diff line.
```

Every other entry then gets those terms underlined where it uses them, with the definition on hover and a click through to the entry that defines them.
Matching is whole-word, case-insensitive, longest first, with naive plurals both ways and the optional alias list in parentheses for the rest.
Once per block rather than once per occurrence, because a term used eleven times in a paragraph becomes eleven underlines.

An attribute rather than a scrape of the glossary entry by convention: the tool would otherwise drop a paragraph that does not parse as a term and say nothing, and `validate` reports one instead.

## Example blocks

An `example` block is a demonstration and what running it produced.

```markdown
## example  clean-core/vector
source: libs/base/clean-core/examples/vector.cc:10-40
run: uv run dev.py example clean-core/vector
capture: stdout
output: attachments/050-vector.txt
```

**`run:` means the tool executed it, and `cmd:` means it did not.**
The two are mutually exclusive and the parser enforces it, so the "not reproduced by the tool" the page draws is a fact about which key was used rather than an honour system.
`cmd:` alone shows a command for the reader to run; `cmd:` with an `output:` shows what the agent captured out of band, which is how a tool `review run` cannot execute still gets its evidence in.

`review run` executes the `run:` blocks, writes the capture into `attachments/`, and splices `output:`, `status:`, `sha:` and `at:` back in.
Nothing else in the tool ever spawns a process out of an entry, and the server never does.

The allowed command prefixes are `run_prefixes` in `review.toml`, **empty by default** — running examples through `dev.py` is a fact about shaped-core, and this tool reviews any git repository.

The recorded sha is what makes the output a claim about a commit: `validate` warns once it no longer matches, and the page draws the block as stale.

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
