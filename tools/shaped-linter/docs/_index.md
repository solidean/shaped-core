# shaped-linter docs

Documentation hub for shaped-linter, the `scl` custom linter.
For what the tool is, how to run it, and the `prose apply` plan grammar, start at the [readme](../readme.md).
Repo-wide docs are at [docs/_index.md](../../../docs/_index.md).

## Topics

- [architecture](architecture.md) — how the layers fit together: the three front ends, why prose is a layer rather than a step, and what spans buy.
  Read it before growing the lexer or the parser, and for the boundary the parser deliberately does not cross.
- [writing-a-rule](writing-a-rule.md) — the authoring procedure end to end: picking a layer and a language set, emitting a finding, the `fix` / `hint` line, registering, testing.
  This is the one to follow when adding a rule; the other two are reference.
- [coding-guidelines](coding-guidelines.md) — the tool's own conventions: a rule is a folder, the two test layers, and **the corpus format** — the annotation spec every corpus file is written against.

## Elsewhere

- [docs/guides/prose.md](../../../docs/guides/prose.md) — the guide over the prose rules: where each answer lives, and when a pile of findings becomes a rework.
- [the `reworking-prose` skill](../../../.claude/skills/reworking-prose/SKILL.md) — the session workflow around `prose apply`.
- [docs/coding-guidelines.md](../../../docs/coding-guidelines.md) — the repo conventions the rules enforce, and the authority behind each rule's rationale.

## Conventions

Code follows the repo [coding-guidelines](../../../docs/coding-guidelines.md) plus the tool-local ones above, and `.clang-format` is authoritative for formatting.
