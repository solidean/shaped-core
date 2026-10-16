---
name: issue-tracking
description: File, find, claim and close issues — markdown files in any `docs/issues/` folder, driven by `dev.py issues`. Use when something is left open, when a failure might be known already, when starting work on an issue, and when a change resolves one.
when_to_use: '"file an issue", "is this a known issue", "known bug", "what is open", "what should I work on", "claim", "I will take", "close the issue", "TODO", "leave this for later", "write down what is left"'
allowed-tools: Read Edit Write Bash Glob Grep mcp__repo_tools__repo_search
---

[docs/guides/issue-tracking.md](../../../docs/guides/issue-tracking.md) is the system: file format, types, refs, markers, claiming, `.issues.toml`.
Read it once before writing a first issue; this skill is only the moves.

Every command is `uv run dev.py issues <command>`, run from the repository root.
The files are the tracker: editing one by hand is always fine, and `issues check` says whether it is valid.

## Before debugging something odd

Run `issues known <path | path:line | test name | symbol | text>` first.
A hit, closed ones included, often saves the investigation; a closed hit on a failure is a regression.

## When something is left open

Anything a session finds and does not finish becomes an issue, not a note in chat or a TODO comment.

1. Pick the type (`bug feat perf debt chore question epic`) and the narrowest scope that owns it: a library's `docs/issues/` over the repository's.
2. `issues new <type> <slug> --in <dir> -t "<title>" --ref file:<path> --ref test:<name>`, then write the body: what is wrong or wanted, and what is already known, measured vs. hypothesis.
3. A workaround in code gets `TEMPORARY(<id>)` naming a `debt` issue; code an issue concerns may get an `issue: <id>` comment.
4. An issue in a submodule (`--in extern/<sub>`) is that repository's and is committed there.

## Starting on an issue

**Claim it before the first edit**, only when the user asked you to work on it:
`issues claim <id> --as "Claude (for <user's name>)"`.
The claim pushes one commit and nothing else, and needs no pre-push gate; tell the user it went out.
If it is claimed already, stop and ask; never `--force` on your own initiative.

## Resolving an issue

`issues close <id> [--resolution fixed|wontfix|duplicate|obsolete]` in the same commit as the change that resolves it.
Remove the markers naming it in the same change; `issues check` fails on a marker naming a closed issue.
What the issue taught beyond its own life — a decision, a measured invariant — goes into the docs or a test before it closes.
