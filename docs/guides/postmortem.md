# Postmortem (Session Friction Review)

A short, high-signal review run near the **end of a coding session** to find where the session
*lost momentum*. It is invoked as the `/postmortem` agent skill
([.claude/skills/postmortem/SKILL.md](../../.claude/skills/postmortem/SKILL.md)) — this doc is the
background; the skill is the thing you run.

## What it is — and isn't

A postmortem here is an **engineering-productivity review of the session**, not of the code. It
asks "what made this slow, uncertain, or repetitive?" — not "what broke?".

- It is **not** bug triage. A defect that needs to stay fixed becomes a pinned nexus test, not a
  postmortem entry.
- It is **not** a status report or a changelog. The commit message and diff already cover what
  changed.

The output is about retries, late-discovered invariants, weak or badly-located docs, unclear
APIs, and forced code archaeology.

## When to run it

Near the end of a session that involved non-trivial exploration or hit rough edges — especially
work that touched an under-documented part of a library. A clean, frictionless session is a valid
no-op: a sparse or empty review is the correct result, not a failure.

## What makes a good review

- **Sparse and structural.** Two findings that name a root cause beat eight that list symptoms.
  Omit any category with no strong finding.
- **Smallest-fix-oriented.** Each observation ends with the *minimal* change that would most
  reduce future cost — preferably a one-line doc or contract edit, not a refactor.
- **Specific.** Point at the file, the invariant, the missing link.

A good vs. weak finding, same session:

> **Good:** "The subobject-safe move-assignment requirement is only inferable from the impl in
> `vector.hh`; I retried the change twice before spotting it. Fix: one `///` contract line on the
> header, cross-linked from `docs/coding-guidelines.md`."
>
> **Weak:** "Move assignment was confusing."

The good one names the invariant, how it cost time, and a one-line fix. The weak one is a
complaint with no leverage.

## The closing block

Every review ends with the same three things (the rest is optional):

1. The **top 3 highest-leverage improvements**, ranked.
2. The **shortest plausible path** a future agent would take with those improvements already in
   place.
3. A **self-reflection**: "What is one thing this postmortem format failed to ask that would have
   produced a more useful review?"

## Evolving the skill

The self-reflection is the skill's own feedback loop. If it surfaces the *same* missing dimension
across multiple sessions — not a one-off quirk — propose a one- or two-line addition to
[.claude/skills/postmortem/SKILL.md](../../.claude/skills/postmortem/SKILL.md) and ask before
applying. Keep it lean: the value is selectivity, so resist growing it into a framework or a long
checklist.
