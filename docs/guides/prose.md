# Prose and linting (`dev.py lint`)

The repo lints its prose the way it lints its code.
`uv run dev.py lint` is the front door for both halves: the clang-tidy gates and shaped-linter over C++, and shaped-linter's prose rules over comments, docstrings and markdown.
Back to [guides](_index.md).

```bash
uv run dev.py lint shaped --dirty-only   # the loop: your changed lines only
uv run dev.py lint shaped --fix          # apply what is mechanically safe
uv run dev.py lint clang-tidy            # the C++ gates (see building-and-testing.md)

uv run dev.py lint prose-stats <path>... # how much prose a surface carries, before you plan
uv run dev.py lint prose-apply <plan>    # land many rewrites in one pass
```

Three levels answer three different questions, and none of them repeats another:

| you want | look at |
|---|---|
| what good prose *is* here | [coding-guidelines.md](../coding-guidelines.md#prose-style--one-semantic-point-per-line) — the rule, its scope, and worked good/bad examples |
| what the tool does, and the exact plan grammar | [tools/shaped-linter/readme.md](../../tools/shaped-linter/readme.md) — every rule, `--changed-lines`, `prose apply`'s two validations, `prose stats` |
| how to run a rework in a session | the [`reworking-prose` skill](../../.claude/skills/reworking-prose/SKILL.md) — scope, concept, plan, apply, cold read |

This guide is the seam between them: when to reach for which, and what bites.

## Run it early, not at the gate

**Run `uv run dev.py lint shaped --dirty-only` as soon as your first bigger chunk compiles.**
Its rules encode conventions the guidelines only summarize, and seeing them fire on *your* code is the fastest way to learn how they apply.
`--fix` applies what is mechanically safe; a `hint:` is a judgement call left to you.

`check` runs the same thing as the `shaped-lint` gate, dirty-only, before `format` — see [Pre-commit checks](building-and-testing.md#pre-commit-checks) for the gate order and why it is load-bearing.

## Dirty-only is line-exact for prose

A prose finding sits on exactly one line, and that line either changed or it did not, so a dirty-only run can be scoped to changed lines exactly rather than approximately.
The [readme](../../tools/shaped-linter/readme.md#--changed-lines-and-why-dirty-only-is-line-exact-for-prose) has the mechanism.

The consequence is the one to carry: **editing one section of a long document puts only those lines in the gate.**
That is what makes a drive-by prose fix a terminating job rather than an unbounded sweep of the file.
It is also why the repo's prose backlog only shrinks when someone attacks a surface deliberately.

Pre-existing violations outside the lines you touched are not yours to fix.

## One finding is not the work item

A `no-flow-prose` hit says a line was reflowed.
The defect is usually that the comment says three things, two of which belong somewhere else or nowhere.
Repairing the line launders that as done, and a local edit that can only rewrite its own span reliably makes the text longer.

So the rule of thumb: **more than a couple of findings on one topic is a rework, not a run of local edits.**
That is what `prose-apply` and the [`reworking-prose` skill](../../.claude/skills/reworking-prose/SKILL.md) are for.
Every rewrite for a surface is written once, validated as a set, and applied in one invocation.
`prose-stats` is how a scope gets a real number before the plan exists, and `prose-apply --stats` is how the plan gets checked against it.

## What bites

* **The prose rules have no auto-fix.** `lint shaped --fix` does nothing for them, by design: obeying the rule means modelling the prose, not splicing in a newline.
* **An abbreviation ending in a dot is the known false positive** (`e.g.`, `i.e.`, `vs.`).
  The fix is to add the word to the list in [no_flow_prose.cc](../../tools/shaped-linter/rules/prose/no-flow-prose/no_flow_prose.cc), never to reword the sentence.
* **Un-reflowing a markdown paragraph routinely trips `no-long-prose-line`.**
  Joining a justified block into one point lands at 200–290 characters often enough that a second `prose-apply --dry-run` per markdown chunk is the expected rhythm, not a mistake.
  A `///` block is already short, so code rarely does this.
* **`prose-apply` is all-or-nothing** across every file in a plan, and it rejects an edit that changed code or wrote a line the rules then fire on.
  Markdown has no lexer, so the code-unchanged check cannot see a span that landed on the wrong lines there.
  `dev.py check crossrefs` is what catches that, and only if the damage touched a link or a heading.

## Related

- [coding-guidelines.md](../coding-guidelines.md#prose-style--one-semantic-point-per-line) — the rule itself.
- [tools/shaped-linter/readme.md](../../tools/shaped-linter/readme.md) — the tool, its rules, and the plan grammar.
- [building-and-testing.md](building-and-testing.md) — `dev.py lint`'s place in the build/check surface.
