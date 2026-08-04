# Postmortem (Session Friction Review)

A short, high-signal review run near the **end of a coding session**, to find where the session *lost momentum*.
It is invoked as the `/postmortem` agent skill ([SKILL.md](../../.claude/skills/postmortem/SKILL.md)), which is the thing you run; this doc is the background.
Back to [guides](_index.md).

## What it is — and isn't

A postmortem here is an **engineering-productivity review of the session**, not of the code.
It asks "what made this slow, uncertain, or repetitive?", not "what broke?".

- It is **not** bug triage.
  A defect that needs to stay fixed becomes a pinned nexus test, not a postmortem entry.
- It is **not** a status report or a changelog.
  The commit message and diff already cover what changed.

What it *is* about — retries, late-discovered invariants, weak or badly-located docs, unclear APIs, forced code archaeology — is enumerated in the skill, one bullet per category.

## When to run it

Near the end of a session that involved non-trivial exploration or hit rough edges, especially work that touched an under-documented part of a library.
A clean, frictionless session is a valid no-op: a sparse or empty review is the correct result, not a failure.

## What makes a good review

The skill states the criteria it applies — sparse over exhaustive, smallest-fix-oriented, specific.
What that looks like in practice is the difference below, and it is the reason this doc exists separately from the skill.

A good and a weak finding, from the same session:

> **Good:** "The subobject-safe move-assignment requirement is only inferable from the impl in `vector.hh`.
> I retried the change twice before spotting it.
> Fix: one `///` contract line on the header, cross-linked from `docs/coding-guidelines.md`."
>
> **Weak:** "Move assignment was confusing."

The good one names the invariant, how it cost time, and a one-line fix.
The weak one is a complaint with no leverage.

## Evolving the skill

Every review closes with a self-reflection on what the format failed to ask, which is the skill's own feedback loop.
If it surfaces the *same* missing dimension across several sessions, rather than a one-off quirk, propose a one- or two-line addition to [SKILL.md](../../.claude/skills/postmortem/SKILL.md).
Ask before applying it.
Keep it lean: the value is selectivity, so resist growing it into a framework or a long checklist.

## Related

- [.claude/skills/postmortem/SKILL.md](../../.claude/skills/postmortem/SKILL.md) — the skill this doc backs; it owns the categories and the output shape.
- [_index.md](_index.md) — the other guides.
