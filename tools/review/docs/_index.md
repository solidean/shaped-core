# review docs

Documentation hub for the review tool.
For what it is, how to run it, and the command surface, start at the [readme](../readme.md).
Repo-wide docs are at [docs/_index.md](../../../docs/_index.md).

## Topics

- [coverage-model](coverage-model.md) — net-diff line space, what an atom is, and how a change from any provenance claims one.
  Read it before changing anything under `lib/space/` or `lib/changeset/`, and for why a bulk claim and a commit-local hunk are accounted for identically.
- [block-grammar](block-grammar.md) — the entry file format: block types, the attribute prelude, asks and options.
  This is what an agent writes and what the page renders, so it is the contract between them.
- [review-folder](review-folder.md) — every file in a review folder, who writes it, and what survives a re-ingest.
- [entry-types/](entry-types/_index.md) — recurring kinds of entry, as instructions rather than as code.
  Read its index every review and a type in full only when it applies; the tool knows about none of them, which is why the set grows by writing prose.

## Elsewhere

- [docs/guides/reviewing-prs.md](../../../docs/guides/reviewing-prs.md) — what we look for in a review.
  The tool carries none of that taste; it only makes sure nothing goes unlooked-at.
- [the `reviewing-a-pr` skill](../../../.claude/skills/reviewing-a-pr/SKILL.md) — the session workflow over these commands.
- [docs/dev-py-driver.md](../../../docs/dev-py-driver.md) — why a tool with third-party dependencies is a standalone script rather than a `dev.py` subcommand.

## Conventions

Code follows the repo [coding-guidelines](../../../docs/coding-guidelines.md), including the prose rule for comments and docstrings.
The tool couples to shaped-core in exactly one place — the `console` import in [`tools/review/__init__.py`](../__init__.py) — so that a downstream fork edits one line.
