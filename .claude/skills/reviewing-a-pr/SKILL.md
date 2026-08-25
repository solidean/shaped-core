---
name: reviewing-a-pr
description: Review a pull request, a branch or a design together with the maintainer, driving the review tool — pin the range, account for every change, write standalone entries with anticipated answers, and iterate in rounds. Use whenever you're asked to review a PR, look at a PR number or branch, give a second opinion, or think through a design before it is built.
when_to_use: "review this PR", "look at PR #<n>", "what do you think of <pr url>", "review my branch", "second opinion on this change", "let's design X before building it"
allowed-tools: Bash Read Write Edit mcp__repo_tools__repo_search mcp__repo_tools__repo_view mcp__repo_tools__repo_structure mcp__repo_tools__file_structure mcp__repo_tools__build_diag mcp__repo_tools__test_diag
---

This skill is **scaffolding only** — how to get a change in front of the maintainer and what shape the review takes.
What we actually look for, and how we weigh the tradeoffs, lives in [docs/guides/reviewing-prs.md](../../../docs/guides/reviewing-prs.md).
Read that guide every time; it is the living record of the reviewer's taste and it changes as we review more.

The tool is [tools/review/readme.md](../../../tools/review/readme.md), and its design is [tools/review/docs/_index.md](../../../tools/review/docs/_index.md).

## Use the tool

Drive `review.py` for any review with more than a handful of hunks, which is nearly all of them.
Chat is fine for a one-line fix; the moment you would be tempted to write "and a few other small things", you needed the ledger.

The tool exists because a chat review is a narrative, and a narrative silently skips a file.
It does not carry any taste — that is still yours and the guide's.

## Ask for the goal before anything else

`init` refuses without one, and you should refuse too rather than guess.

- **`pr-comment`** — the artifact is one standalone comment for an author who was not in the conversation.
- **`land-changes`** — the rounds are work orders you carry out in this session, and `sync` verifies each fix landed.
- **`design`** — no changeset at all; the artifact is agreement on something that does not exist yet.

They combine.
Reviewing someone's branch and landing the fixes yourself is `--goal pr-comment --goal land-changes`.

## The flow

1. **Fetch and pin.**
   ```bash
   git fetch origin main --quiet
   git fetch origin refs/pull/<n>/head:pr-<n>
   uv run review.py init pr-<n> --range origin/main..pr-<n> --goal <goal>
   ```
   `init` resolves the merge base and pins it as a sha, so a moving `main` cannot change what the review is accountable for.
   **Never diff against local `main`** — it only moves when someone pulls.

2. **Account for every change.**
   ```bash
   uv run review.py ingest pr-<n> --stats     # the shape, before committing to reading it
   uv run review.py ingest pr-<n>             # ids for everything
   uv run review.py coverage pr-<n>           # gate 1
   ```
   Where a vendored drop or a mechanical sweep would eat your context for nothing, bulk it *with a real reason*, then `--rest`:
   ```bash
   uv run review.py ingest pr-<n> --bulk extern/zstd/ --reason "vendored 1.5.6 drop-in, matches the upstream tag"
   uv run review.py ingest pr-<n> --bulk-commits <sha> --reason "clang-format sweep, no semantic change"
   uv run review.py ingest pr-<n> --rest
   ```
   Scope a bulk by path when the change is confined to a subtree, and by commit when it is confined to an author's intent — a sweep, a vendored drop, a merged branch already reviewed elsewhere.
   The reason is the honest part.
   It is what makes "I did not read this" a decision the maintainer can see and overrule.

3. **Check the branch out and read the code, not just the diff.**
   `git checkout pr-<n>`, then read whole files around the hunks.
   A diff shows what moved; only the file shows whether the result still hangs together.

4. **Verify every claim against the final tree.**
   The PR body, each `///` the change touches, each cheat-sheet line, each `[done]` / `[planned]` row.
   Look the symbol up — never confirm a doc sentence by reading the sentence.
   This is where the highest-value findings are, because it is exactly what a diff view hides.

5. **Write the entries**, as below.
   `uv run review.py generate pr-<n>` writes the overview and coverage entries first.

6. **Hand it over and wait.**
   ```bash
   uv run review.py serve pr-<n> --no-open &     # background: the shell caps below how long a review takes
   uv run review.py round pr-<n> --wait          # blocks, then prints the round
   ```
   Exit 0 means sent, 2 means paused, 3 means timed out.
   Do not treat a pause as an answer.

7. **Next round.** Act on what came back, append new blocks and entries, and serve again.
   A round with three answers out of twelve is a normal round.

8. **Finalize.** `uv run review.py finalize pr-<n>` drafts the artifact for the goal.
   It is a draft: the tool gathered what was decided, it did not decide which points are worth an afternoon.

## Writing entries that are worth answering

This is the part the tool cannot do for you, and the part the previous chat workflow got wrong.

**Every entry stands alone.**
The maintainer does not carry the changeset in their head, and assuming they do defeats the point of asking.
Write the three context tiers so a reader with any amount of background can start from this entry:
`context/cold` for someone new to both the change and the codebase, `context/repo` for someone who knows the codebase, `context/delta` for someone who has read the entries above.
The first two collapse by default and have word budgets — respect them, since a tier nobody can skim is a tier nobody opens.
`context/delta` is the one you always write.

**Describe neutrally, then recommend separately.**
The `prose` block says what is true; the `recommendation` block says what you would do.
Mixing them is how a reviewer ends up agreeing with framing rather than with a finding.

**Anticipate the answers.**
An `ask` with well-chosen options is answered in a click; an `ask` with only a text box is a chat message wearing a costume.
Offer the two or three directions the maintainer would actually pick between, mark one `(recommended)`, and add a `check:` for the follow-ups worth doing either way.
The freeform box is always there anyway, so options cost nothing when they are wrong.

**Discharge deliberately.**
`discharges:` on an ask is what says "this question accounts for those hunks".
An LGTM entry with one ask discharging forty changes is correct and good; forty entries would not be.

**Show the code.**
Use `changes` blocks so the hunks render inline, and cite `file.hh:63` in backticks — the page turns it into a link that opens the file.
Report API shape in symbols, never in prose about symbols.

**One point per ask.** The maintainer cannot half-agree with a bundled question.

**A finalized ask is immutable.** For a follow-up, add a new ask with `follows: <name>`; never reword an answered one.

## What is not a review finding

Cut these before writing, because they cost attention and return nothing.
The guide has the full list; the ones that bite most often:

- **"Does it build" and "do the tests pass."** CI and `dev.py check` cover that.
  Build only to settle a specific claim, then report what you learned.
- **PR size and splitting.** Only "can this be reviewed" matters, and the bar is high with the tool.
- **Compatibility and migration.** The repo is beta; breaking things is cheap and the wrong shape is expensive.
- **Restating the PR body.** The author wrote it.

## The GitHub comment

When the goal is `pr-comment`, the artifact is **a task list for a fresh agent session** — not a summary of the review and not a record of the conversation.

- **Drop the context tiers and the overview entirely.** They exist so the maintainer could judge a point; they tell the author nothing.
- **Drop everything still open.** Only decided instructions are posted.
- **Every point is an instruction with its reasoning under it**, executable by someone who has not read the diff and cannot ask a follow-up.
- **It has to stand alone.** No "as we discussed", no numbering that implies a conversation.
- Plain backticked `path/to/file.cc:63` beats markdown links there — relative links do not resolve in a comment.

Post only on an explicit go-ahead, as one comment.

## Feed the guide

The point of reviewing with the maintainer is that their answers are data.
When they correct a call, reject a finding as noise, or explain a tradeoff, **write it into [docs/guides/reviewing-prs.md](../../../docs/guides/reviewing-prs.md)** in the same session.
A rule under "Settled calls" if it generalizes, an example under an existing rule if it does not.
Ask before adding a rule you are inferring rather than being told.
