---
name: postmortem
description: Produce a short, high-signal session friction review near the end of a coding session — where momentum was lost (retries, late-discovered invariants, weak docs, forced code archaeology), not a bug report. Sparse, selective output is the point.
when_to_use: "postmortem", "session friction review", "session retrospective", "what slowed us down", "friction review"
argument-hint: "[optional focus area]"
allowed-tools: Read Grep Glob
---

## Reference

Background and rationale: [docs/guides/postmortem.md](../../../docs/guides/postmortem.md)

## What this is

An engineering-productivity review of *this session* — where it lost momentum,
not where the code had bugs. Work from the conversation and diff already in
context; only open a file to confirm a specific friction claim.

If a `[focus area]` argument is given, weight the review toward that part of the
session, but still report anything high-leverage elsewhere.

## What to look for

- Major exploration costs — concepts that needed multi-hop investigation.
- Retry loops — the same thing attempted several times, especially retries
  caused by weak or slow feedback.
- Hidden invariants discovered late — assumptions only inferable from the impl.
- Unclear or misleading APIs and naming that obscured intent.
- Information-locality failures — the thing you needed lived far from where you
  needed it (docs in one place, the rule in another).
- Load-bearing assumptions confirmed only indirectly — a conclusion the session
  leaned on but verified via passing tests or inference rather than a stated
  guarantee (flag as latent risk).

## Discipline (this matters more than coverage)

- **Not exhaustive. Sparse output is the goal.** Two sharp findings beat eight
  padded ones.
- **Omit any section with no strong finding.** Empty is a valid result.
- Only include issues that materially increased **uncertainty, retries, code
  archaeology, context switching, or implementation risk**.
- Prefer deep structural causes over surface-level complaints.

## For each observation

1. What caused the friction.
2. How it manifested this session.
3. The *smallest* change that would most reduce future cost.
4. Tag whether it was self-inflicted (an agent reasoning shortcut) or structural
   (the codebase did not surface what was needed). This only changes *where* the
   fix goes — both must resolve to a concrete change in the repo (a doc, comment,
   hint, or name), never to "the agent should try harder."

## Recommended output shape

All sections optional except the final leverage summary. Include only those with
real findings, in any order:

```
### Exploration costs
### Retry loops
### Hidden invariants
### Unclear APIs & naming
### Information locality
```

## Always end with

- **Top 3 highest-leverage improvements** (ranked).
- **Shortest plausible path** a future agent could take *with those improvements
  already in place*.
- **Self-reflection:** "What is one thing this postmortem format failed to ask
  that would have produced a more useful review?"
- If that reflection points at a *recurring* missing dimension (not a one-session
  quirk), propose a one- or two-line addition to this `SKILL.md` — show the exact
  line and where it goes — and ask before applying. Do not silently rewrite it.

## Example invocation

```
/postmortem
/postmortem clean-core containers   # weight toward that part of the session
```
