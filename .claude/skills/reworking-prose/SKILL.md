---
name: reworking-prose
description: Rework the comments and documentation around a topic wholesale — decide what each level should say, then land every rewrite in one `prose apply` pass; use it when files violate the prose guidelines, when a guideline changes and the existing prose must follow, or when a surface is under-documented and needs more.
when_to_use: "these files violate our doc policy", "fix the comments in", "rework the docs around", "no-flow-prose is firing all over", "the guideline changed, update the prose", "document this properly"
allowed-tools: Read Edit Write Bash Glob Grep mcp__repo_tools__repo_search mcp__repo_tools__repo_structure mcp__repo_tools__file_structure
---

[docs/guides/prose.md](../../../docs/guides/prose.md) is the guide around this skill: when a pile of findings becomes a rework, and where each answer lives.

## What this is

Fixing prose one finding at a time does not work.
A `no-flow-prose` hit says a line was reflowed; the actual defect is usually that the comment says three things, two of which belong somewhere else or nowhere.
Repairing the line launders the defect as done, and because a local edit can only rewrite the span it is anchored to, the result reliably gets longer.

So the unit of work here is a **documentation surface**, not a line, and the mechanism is a **plan** — every rewrite for that surface written once, validated as a set, applied in a single invocation.

The same skill grows documentation.
A surface that is too thin is the same job with a bigger budget.

## The loop

### 1. Propose a scope

You are usually handed files, not a topic: *"X, Y and Z violate the policy."*
Find the topic that makes fixing them coherent.

Walk outward from those files: same-area headers, the docs their comments **cite**, the cheat-sheet entries for their symbols.
Redundancy lives *across* levels — repo docs, library docs, cheat sheet, header `///`, inline `//` — and a rewrite that fixes a header while leaving the doc it points at stale has fixed nothing.

**Code and docs are one scope, not two.**
A topic's prose spans levels by nature, so the default scope covers every level the topic touches.
That is the headers, the `.cc` rationale comments, the concept docs they cite, and the cheat-sheet sections for those symbols.
Splitting them into separate passes is the failure mode this skill exists to prevent — it is what leaves a rewritten header pointing at a doc that still contradicts it.
A code-only or docs-only scope is the exception, and needs a reason beyond size.

Offer 2–3 candidate scopes with counts (files, prose lines, current findings) and let the user pick.
`uv run dev.py lint prose-stats <path>...` gives the prose lines and words, per file and total, for any file or directory — that is where the counts come from, not from an estimate.
Make every candidate span levels, and let the choice be *how far out the topic reaches* — never *code or docs*.
Naming the doc surface as the expensive option is how you talk the user out of the thing they wanted.
If a level is genuinely too big for one pass, say what you are deferring and why, rather than offering it as an upgrade.
Scope is fluid; "topic" is only the default shape.

While walking outward, note what is **missing**: a central type with no concept doc, a cited path that does not resolve, a default that exists only in the cheat-sheet.
A gap is a scope candidate like any other — this skill grows documentation as readily as it trims it.

### 2. Read the files

Plain `Read`. A member doc is unrewritable without its signature, so there is no shortcut around reading the code the prose is attached to.

### 3. Write the documentation concept

In-session and ephemeral — no file, and only an abridged version reaches the PR description.
This is the step that makes the rest mechanical, so it is never compressed into "and then I rewrote the comments".

Work it out in this order, before any new text is written:

- the **reader questions** this surface must answer, concrete and countable ("which allocator does this use", "is this safe to call twice");
- **the one level that owns each answer** — the concept doc, the header, the cheat sheet, or an inline `//` at the pitfall itself;
- **what every other level gets instead** — never a second full copy, and never silence.
  A **mini-duplication** is the one-liner you repeat where making the reader jump would cost more than the repeat.
  A **mini-hint** is a named symbol or a cited path that says where to look.
  An answer that lives at exactly one level and is unreachable from the others is not documented.
  **"Another level owns it" licenses a shorter line, not always no line** — deleting a whole section outright is the easy over-correction, and a landing page is where it hurts most;
- a **word budget per surface**, which may be *higher* than today when the point is to document more.

Verbosity is the enemy and so is missing documentation — the concept is what lets you cut hard without stranding a reader.
Every line has to pull its weight, and a fringe concept documented at three levels is the same defect as an undocumented central one.

**Before cutting a recap, name the level that owns the answer — then check that level exists.**
Deleting a "what this already does" recap is right when a concept doc owns it, and wrong the moment none does.
That failure is invisible downstream: every span validates, every rule passes, and the answer is simply gone from the tree.
A missing owner is a step-1 gap that has just turned urgent — write the doc, or keep the recap until someone does.
**A gap may be an unreachable owner rather than a missing one.**
Before writing a new doc for a gap, grep for the answer outside the surface you scoped — a tool readme, a library-local doc, a `.cmake` comment.
If it exists but nothing in the reader's reach links to it, the fix is a cross-link plus a short guide-level page, not a fourth copy of the mechanism.

**Compression is where a rework introduces its own errors, and the tell is a word doing double duty.**
Collapsing a list that mixes "what exists" with "what is intended" — tiers, roadmaps, support levels — reads afterwards as an assertion that all of it exists.
When a sentence you are shortening carries a hedge, decide whether the hedge was the load-bearing half before dropping it.


**Backstory is the first thing to cut**, per [Code Comments](../../../docs/coding-guidelines.md#code-comments).
No narration of why an approach was chosen, and above all no cross-module causal annotation — "because *(some implementation fact three libraries over)*" reads as insight and ages into a lie.
Delete it by default: a real Chesterton's fence is rare enough (roughly 20:1 against on this tree) to be the exception you argue for.
A measurement narrative belongs in the concept doc — never next to the code it once explained.

**Dangling references go too, and a rework is where they surface.**
A discarded prototype, a removed subsystem, a plan, a handover note, "an earlier version pinned X".
The reader cannot look any of it up, so it reads as a live part of the system that they simply cannot find.
Where a removed approach still matters it is a constraint on the *current* design: keep the constraint, drop the history.

The budget is the anti-bloat device.
It is not a shrink rule.
A surface that has been reworked or edited often is already dense: its duplication is compressed restatement, not verbose blocks.
Deleting a one-liner repeated at three levels saves twenty words, where a first pass over a young header sheds a whole mechanism.
So budget by surface age, and expect a mature surface to come back near flat — the win there is correctness and findability, not volume.
Correcting a stale claim costs words too: the truth is almost always longer than the wrong version it replaces.


Set it against the numbers `prose-stats` gave you in step 1, and check it with `--stats` in step 5.
**Budget on words, not lines.** Un-reflowing a justified block adds lines by construction, so `+lines` is not a failure — `+words` on a surface you meant to trim is.
The dry run reports the same lines and words a real run would, so the budget is tested before the plan lands.
Do not reconstruct either figure with `wc` / `grep -c` pipelines; both come from the same extraction the linter uses.

### 4. Write the plan

To `.tmp/prose-<topic>.plan` (gitignored, and easy for the user to read).

**Keep a plan under ~800 lines, and split the surface into several when it would run over.**
One concept still covers the whole surface — this is chunking the *landing*, not the scoping.
A big surface as one plan means a long write before the first dry run, and one bad span rejects every file in it;
a chunk per file, or per closely-related group, gets each one validated, budget-checked and applied while it is still fresh in mind.
Name them for the chunk (`prose-async-doc.plan`, `prose-async-headers.plan`, …) and land them in sequence under one commit.
Apply each chunk before writing the next: applying shifts line numbers, so a later chunk's spans must be read after the earlier ones land.

```
## libs/base/clean-core/src/clean-core/container/key_value_cache.hh
[14-17]
| /// A tiered get-or-create cache: key_value_cache over a stack of key_value_provider tiers.
| /// The tier interface is the extension seam for on-disk / networked caches.
[49-50]
[+52]
| /// Eviction is deliberately crude — see apply_bookkeeping.
```

The grammar is specified in full by [the shaped-linter readme](../../../tools/shaped-linter/readme.md).
That includes the `| `-prefix rule: a replacement line missing it parses as a plan directive, which fails the plan before any file is read.
Four things bite while writing a plan, so they are repeated here:

- `[a-b]` replaces those lines, `[a]` one line, `[+n]` inserts before line n, and a span with no `| ` lines **deletes**.
- Spans ascend, may not overlap, and their line numbers are the file **as you read it**.
- A markdown **fenced block is editable too**, so a stale status tree or table does not need a hand-edit.
- A bare `|` is an **empty replacement line**, so a span may cover a blank line rather than being split in two around it.

**Take every span's line numbers from a fresh read of that file, in the same session as writing the plan.**
An off-by-one span silently swallows the declaration under the comment block, which is the one mistake this format makes easy.
Reconstructing numbers from an earlier read, a search result or memory is how it happens.

**Re-read the lines a span replaces, not just its numbers.**
A wide span drops whatever it covered that the rewrite did not carry forward, and a trailing sentence on the span's last line is the one that goes.
Nothing downstream notices: the span is in range, the dry run passes, the code-unchanged check sees no tokens, and `--stats` moves by a plausible amount.

`prose apply` catches it every time — that is what the code-unchanged check is for — but read its message carefully:
it reports where the *token streams diverge*, which is downstream of the span that is actually wrong.
`'double' where 'u64' was` means a declaration went missing somewhere above, not that anything is wrong at the reported line.

### 5. Apply

```bash
uv run dev.py lint prose-apply .tmp/prose-<topic>.plan --dry-run --stats   # validate everything, write nothing
uv run dev.py lint prose-apply .tmp/prose-<topic>.plan --stats             # all-or-nothing
uv run dev.py format --dirty-only
uv run dev.py lint shaped --dirty-only
```

Applying is all-or-nothing across every file in the plan, and a file is rejected when the edit changed **code** rather than prose, or when a rule fires on a line the plan **wrote**.
Both are hard failures — fix the plan, do not work around the tool.

**One dry run reports every problem the plan has**, so fix them as a batch rather than re-running per finding.
Prose findings come back with carets over the *rewritten* text, which is the text to correct in the plan — not what is still on disk.

**Budget two dry runs per markdown chunk, because your own rewrites will trip `no-long-prose-line`.**
Joining a justified block into one semantic point routinely lands at 210–290 characters, over the 200 ceiling — structural, not carelessness.
It is a markdown problem: a `///` block is already short, so code chunks nearly always pass first time and a reflowed markdown paragraph nearly always does not.
So the rhythm is: write, dry-run, split each flagged line at its seam, dry-run again, apply.
Do not hand-count characters while writing the plan; the dry run finds them for free and you will guess wrong.

`--stats` prints the prose delta per file and in total, which is where step 3's budget gets checked.
Read it on the dry run: a surface that was meant to shrink and came back `+40` is a plan to revise, not a result to land.
A file whose delta is `+0 / +0` usually means the rewrite only moved words around, which is worth a second look before it lands as churn.

### 6. Cold-reader pass

A fresh subagent, no session context: hand it the concept, the guidelines, the old prose, the new prose, and access to the code.
It answers the reader questions from step 3.

Instruct it **symmetrically**, weighting both directions equally:

- can a reader **find, understand and apply** every important point — does each level carry a hint that leads there, or did a deletion strand them;
- does **every remaining line pull its weight** — added lines that do not earn their place, a fringe concept documented at three levels, backstory that survived the cut.

"Shorter and still answers everything" is a pass, not a regression.
So is "one line longer, and now findable".

Point it at the code as well as the prose, and ask it to check the claims:
which exception types a scope actually throws, whether the stated preconditions match the `CC_ASSERT`s, whether a doc still describes a capability the type has since grown.
A cold reader finds factual drift that no prose rule can see, and that is often the most valuable thing it returns.

Three lines in the prompt raise the yield a lot:

- **Name the two or three deletions you are least sure about**, and ask directly whether each stranded a reader.
  "Did anything get lost" returns generalities; a named deletion returns a verdict.
- **Say that factual errors have already been found and fixed in this pass**, so it assumes more survive instead of reading the prose as trustworthy.
- **Ask it to separate what it verified against code from what it suspects.**
  The verified half is actionable as-is; the suspected half is where its judgement calls sit, and you will decline some of them.

### 7. Correction pass

**Expect a second `prose apply`, not a hand-edit.**
A cold reader that finds nothing is the exception; a real review returns dropped facts, wrong exception types and files you wrongly left out of scope.
Write those as a second plan (`.tmp/prose-<topic>-fixes.plan`) and land them the same way — the code-unchanged and rule-fires checks matter just as much on the corrections.

Do not fold the review's findings into the working tree by hand.
A hand-edit skips both checks, and it is the corrections — written under time pressure, against line numbers that just moved — that most need them.

Two apply passes, one commit.

### 8. One commit

## Two rules that override the defaults

**The finding is not the work item.**
Never repair a `no-flow-prose` hit in isolation.
If a scope is not worth a concept, it is not worth doing under this skill.

**Pre-existing violations outside your topic are not yours.**
The usual "everything dirty must pass the gate" rule does not apply to prose here.
`--dirty-only` is line-exact for prose rules: touching one section of a long file puts only *those lines* in the gate, and the file's older violations stay out of scope by construction.
That is what makes editing one markdown section a terminating job instead of a sweep.
Do not fix them anyway because they are on screen.

## When to skip the plan

A single comment, or a markdown page that needs restructuring rather than line surgery.
A restructure is a `Write`, not a set of spans — the plan format has nothing to offer there.
Everything in between is what `prose apply` is for.

## Reference

- Prose rules: [docs/coding-guidelines.md](../../../docs/coding-guidelines.md), section "Prose style - one semantic point per line".
- The rule that finds them: [no_flow_prose.md](../../../tools/shaped-linter/rules/prose/no-flow-prose/no_flow_prose.md).
- The applier, and the exact plan grammar: `shaped-linter prose apply --help`, plus [tools/shaped-linter/readme.md](../../../tools/shaped-linter/readme.md).
