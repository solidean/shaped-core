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
answers/040-x.json    the answers — server-owned
rounds/round-1.md     a finalized round's transcript
log.jsonl             every action, append-only
.signal               the one-shot mailbox `round --wait` polls
```

## Who writes what

This split, not locking, is what makes concurrency a non-problem.

| directory | sole writer |
|---|---|
| `entries/` | the agent |
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

**Everything but answers is regenerable.**
The ledger, the diffs and the generated entries all come back from git and `review.toml`.

## review.toml

Hand-editable on purpose: retargeting a review or widening its goals should not need the CLI.

`base` and `head` are shas.
`base_spec` and `head_spec` are what the user typed, kept for messages only.
`context` and `coalesce_gap` are recorded because change ids reproduce only under the parameters that produced them; changing either means a re-ingest.
`watermark` is the last finalized round, so the next one is `watermark + 1`.
`tool_version` guards a format the running tool is too old to understand.

## A design review

`--goal design` has no changeset, so `changes/` stays empty, `ingest` and `coverage` refuse with a message rather than a traceback, and the gate does not exist.
Everything else — entries, asks, answers, rounds, the page — is identical.
