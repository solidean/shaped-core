# review — a folder-based review, answered in a browser

A review is a directory of plain files: a ledger of every change in a commit range, entries written as typed blocks, and the maintainer's answers beside them.
Nothing here is a database, and every file is readable and editable by hand.

Two things it buys over reviewing in chat.
**Every change in the range is accounted for**, because the ledger says which ones are not.
And **answering is clicking**, in a local page, instead of composing prose keyed by point number.

It ships from shaped-core and is dogfooded here, but it reviews any git repository — `--repo <path>`.
Git only; no forge concepts beyond an optional PR-body fetch.

## Quick start

```bash
uv run review.py init my-review --range origin/main..HEAD --goal pr-comment   # pin the range, name the goal
uv run review.py ingest my-review                                             # give every change an id
uv run review.py coverage my-review                                           # gate 1: is anything unaccounted for
uv run review.py generate my-review                                           # write the overview and coverage entries
#   ... the agent writes entries under .tmp/reviews/my-review/entries/ ...
uv run review.py serve my-review                                              # answer them in a browser
uv run review.py delta my-review --finalize                                   # freeze the round, print what changed
uv run review.py finalize my-review                                           # assemble the end artifact
```

`uv run review.py --help` is the CLI reference.
The design behind each piece is in [docs/](docs/_index.md).

## The three goals

A goal is mandatory, and `init` refuses without one, because it decides what the review is *for*.

| goal | the artifact | what changes |
|---|---|---|
| `pr-comment` | one standalone comment for the author | entries become instructions; context and open questions are dropped from the artifact |
| `land-changes` | a work order for this session | entries carry `resolved-by:`, and `sync` marking their changes superseded is the evidence the fix landed |
| `design` | the decisions, as input to a plan | no changeset at all — no ledger, no coverage gate, entries only |

Goals combine: `--goal pr-comment --goal land-changes` is a review of someone else's branch whose fixes you also land.

## Commands

| command | what it does |
|---|---|
| `init <name> --range A..B --goal G` | create the folder; pins the **merge base as a sha** so a moving branch cannot change the obligation |
| `ingest <name>` | give ids to every change; the default sweep closes gate 1 on its own |
| `ingest --commits A..B` | take hunks from individual commits instead, for a net hunk that blends two concerns |
| `ingest --bulk SEL --reason WHY` | one id over a whole subtree, no hunk bodies written; the reason is mandatory |
| `ingest --rest` | ids for whatever nothing claims yet |
| `ingest --dry-run` / `--stats` | what a sweep would create, and the shape of the change set, before committing to it |
| `coverage <name>` | gate 1 and the discharge progress, with the uncovered runs listed |
| `generate <name>` | write or refresh the overview and coverage entries |
| `show <name> [entry]` | an entry with its answers folded in, as plain text — the agent's view |
| `serve <name>` | the page the review is answered in; non-blocking |
| `delta <name> [--finalize]` | the answers since the watermark; `--finalize` freezes the round |
| `round <name> --wait` | block until the page signals, then print and freeze the round |
| `sync <name>` | re-point the review at a moved head |
| `finalize <name>` | assemble the end artifact for the goal |
| `self-test` | the tool's own suite, which `dev.py check` also gates on |

## The loop, from an agent's side

`serve` is non-blocking and `round --wait` is the blocking half, because an agent's shell caps well below how long a review takes.

```bash
uv run review.py serve my-review &        # background
uv run review.py round my-review --wait   # blocks; prints the round when the page signals
```

`round` exits **0** when the round was sent, **2** when the maintainer paused, **3** on a timeout.
Those are different situations: an agent that could not tell them apart would either stop early or wait on someone who has left.

The asynchronous path is the same thing without the block.
The maintainer answers whenever, says so, and the agent runs `delta <name> --finalize`.

## What is not obvious

- **Answers save themselves as they are typed**, as tentative.
  Nothing reaches the agent until a round is finalized.
- **Typed text is never discarded.** A question that changed under an answer keeps the text against the new wording; a question that disappeared keeps the answer as an orphan.
- **A finalized question is immutable.** Reword it and the tool refuses, naming the follow-up ask to write instead.
- **The tool never re-serializes an entry you wrote.** Every write is a splice, so your formatting survives exactly.
- **`entries/` belongs to the agent and `answers/` to the server**, which is why an entry can gain a paragraph while an answer to it is being typed.
- The first run pays a `uv` resolve for `pygments` and `markdown-it-py`. That is the dependency cost, and it is cached afterwards.

## Layout

```
review.py                 the entry point, at the repo root
tools/review/
  cmd/                    one module per command
  lib/core/               folder layout, review.toml, durable writes, the action log
  lib/git/                git plumbing and the only unified-diff parser
  lib/space/              net-diff line space, interval algebra, commit line maps
  lib/changeset/          change identity, the ledger, the three ingest modes
  lib/entry/              the block grammar, its parser, the splice writer, answers
  lib/render/             markdown, highlighting, the entry view, the plain-text view
  lib/serve/              the local server and its live updates
  lib/goals/              what each goal implies, and its end artifact
  assets/                 the page: plain HTML, CSS and JS, no framework
  review-self-test.py     the suite
```

## Elsewhere

- [docs/_index.md](docs/_index.md) — the design: the coverage model, the block grammar, the folder format.
- [docs/guides/reviewing-prs.md](../../docs/guides/reviewing-prs.md) — what we look for in a review, which is the taste this tool carries rather than replaces.
- [the `reviewing-a-pr` skill](../../.claude/skills/reviewing-a-pr/SKILL.md) — how a session drives all of this.
