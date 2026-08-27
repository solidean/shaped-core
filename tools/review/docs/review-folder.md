# The review folder

Everything a review is, on disk.
Default location is `<repo>/.tmp/reviews/<name>/`, which is scratch space; `--dir` puts it elsewhere.
Here `<repo>` is the repository the tool runs from, which is not necessarily the one under review — `review.toml` records that one, and every command reads it from there.
The tool warns when that path is not gitignored, because the first thing a fresh repo does wrong is commit a review.

```
review.toml           what the review is pinned to, and what it has reached
changes/
  ledger.jsonl        one record per change, append-only
  CHANGE-7Q2M.diff    that change's hunk body
entries/040-x.md      the entries — agent-owned
attachments/x.txt     captured output and screenshots — agent-owned
answers/040-x.json    the answers, and the maintainer's comments — server-owned
rounds/round-1.md     a finalized round's transcript
log.jsonl             every action, append-only
.signal               the one-shot mailbox `round --wait` polls
```

## Who writes what

This split, not locking, is what makes concurrency a non-problem.

| directory | sole writer |
|---|---|
| `entries/`, `attachments/` | the agent |
| `answers/`, `.signal` | the server |
| `changes/`, `review.toml`, `rounds/` | the CLI |

An entry can therefore gain a paragraph while an answer to it is being typed, with no merge algorithm anywhere.
The page joins the two when it renders, so the maintainer never sees the seam.

`ledger.jsonl` and `log.jsonl` are append-only with exactly one writer each, so a record is encoded whole and written in one call.
No lock is taken, because there is no second writer to race.

Every replacement is a temp file plus `os.replace`, retried a few times.
The retry is not defensive theatre: on Windows an open editor tab or a virus scanner holds the target and makes the replace fail.

## What survives

**Change ids survive.**
They are derived from the hunk's own bytes, so deleting `changes/` and re-ingesting hands back the ids that were there before, and a hunk the author did not touch keeps its id across a `sync`.

**Answers survive.**
A question that changed under an answer keeps the text against the new wording; a question that disappeared keeps the answer as an orphan, shown in the page and reported by `delta`.

**Entry formatting survives.**
The tool splices; it never re-serializes what someone wrote.

**Everything but answers and attachments is regenerable.**
The ledger, the diffs and the generated entries all come back from git and `review.toml`.
An attachment does not: the run that produced it is gone, and `review run` re-running it produces a capture of a different commit.

**A review folder is scratch, and there is no migration promise.**
It lives under `.tmp/`, the tool changes under it, and a format change may simply break one — starting a fresh review is the answer.
Making a review durable is a separate thing the tool does not do yet; `TODO.md` carries it.

## review.toml

Hand-editable on purpose: retargeting a review or widening its goals should not need the CLI.

`base` and `head` are shas.
`base_spec` and `head_spec` are what the user typed, kept for messages only.
`context` and `coalesce_gap` are recorded because change ids reproduce only under the parameters that produced them; changing either means a re-ingest.
`run_prefixes` is what `review run` may execute, empty unless `init` recognised the repository.
Running examples through `dev.py` is a fact about shaped-core, and this tool reviews any git repository.
It is a **lint rather than a boundary**: the check is a prefix test and the command then goes to a shell, so everything after the prefix is unconstrained.
What it says is that `dev.py example` is the blessed place for things meant to be run to show functionality — it guards against an unintended side effect, not against a command that means harm.
`title` is what the page and the tab show, set by `review title` once the range has actually been read.
`watermark` is the last finalized round, so the next one is `watermark + 1`.
`tool_version` guards a format the running tool is too old to understand.

## A design review

`--goal design` has no changeset, so `changes/` stays empty, `ingest` and `coverage` refuse with a message rather than a traceback, and the gate does not exist.
Everything else — entries, asks, answers, rounds, the page — is identical.
