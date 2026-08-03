---
name: reworking-prose
description: Rework the comments and documentation around a topic wholesale — decide what each level should say, then land every rewrite in one `prose apply` pass; use it when files violate the prose guidelines, when a guideline changes and the existing prose must follow, or when a surface is under-documented and needs more.
when_to_use: "these files violate our doc policy", "fix the comments in", "rework the docs around", "no-flow-prose is firing all over", "the guideline changed, update the prose", "document this properly"
allowed-tools: Read Edit Write Bash Glob Grep mcp__repo_tools__repo_search mcp__repo_tools__repo_structure mcp__repo_tools__file_structure
---

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

Offer 2–3 candidate scopes with counts (files, comment lines, current findings) and let the user pick.
Scope is fluid; "topic" is only the default shape.

### 2. Read the files

Plain `Read`. A member doc is unrewritable without its signature, so there is no shortcut around reading the code the prose is attached to.

### 3. Write the concept

In-session and ephemeral — no file, and only an abridged version reaches the PR description.
Three things, before any new text is written:

- the **reader questions** this surface must answer, concrete and countable ("which allocator does this use", "is this safe to call twice");
- **which level each answer lives at** — and therefore what gets deleted everywhere else;
- a **line budget per surface**, which may be *higher* than today when the point is to document more.

The budget is the anti-bloat device.
It is not a shrink rule.

### 4. Write the plan

To `.tmp/prose-<topic>.plan` (gitignored, and easy for the user to read).

```
## libs/base/clean-core/src/clean-core/container/key_value_cache.hh
[14-17]
| /// A tiered get-or-create cache: key_value_cache over a stack of key_value_provider tiers.
| /// The tier interface is the extension seam for on-disk / networked caches.
[49-50]
[+52]
| /// Eviction is deliberately crude — see apply_bookkeeping.
```

- `[a-b]` replaces those lines, `[a]` one line, `[+n]` inserts before line n, and a span with no `| ` lines **deletes**.
- Spans ascend and may not overlap; line numbers are the file as you read it.
- Everything after `| ` is verbatim final text — comment marker and indentation included, and nothing is inferred.
- A bare `|` is an empty line inside a block.

### 5. Apply

```bash
uv run dev.py lint prose-apply .tmp/prose-<topic>.plan --dry-run   # validate everything, write nothing
uv run dev.py lint prose-apply .tmp/prose-<topic>.plan             # all-or-nothing
uv run dev.py format --dirty-only
uv run dev.py lint shaped --dirty-only
```

Applying is all-or-nothing across every file in the plan, and a file is rejected when the edit changed **code** rather than prose, or when a rule fires on a line the plan **wrote**.
Both are hard failures — fix the plan, do not work around the tool.

### 6. Cold-reader pass

A fresh subagent, no session context: hand it the concept, the guidelines, the old prose, the new prose, and access to the code.
It answers the reader questions from step 3.

Instruct it **symmetrically** — flag dropped facts that mattered *and* added lines that do not earn their place.
"Shorter and still answers everything" is a pass, not a regression.

### 7. One commit

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
