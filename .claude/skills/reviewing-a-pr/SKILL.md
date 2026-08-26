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

### It is new, and it will misbehave

The tool landed recently and is under active development.
Assume it has bugs you will be the first to hit, and treat anything surprising as the tool's fault before assuming you drove it wrong.

**Surface problems in chat, immediately, and keep going.**
Do not quietly work around a broken command, and above all do not silently narrow the review because something did not work —
a review that skipped a file because `ingest` misbehaved is exactly the failure the tool was built to prevent, now wearing the tool's authority.

Say what you ran, what you expected, and what happened.
If a workaround is needed to finish the round, use it and name it as a workaround.

What is worth reporting, roughly in order:

- **A wrong number.** Coverage that does not add up, a claim that looks too big or too small, an id that changed when nothing did.
- **A refusal you think is wrong** — the immutability guard, the attribute whitelist, an out-of-range commit.
- **A crash or a traceback**, with the command.
- **Friction**: something that took four commands and should have taken one, or output you had to squint at.

### Leave feedback in the `tooling` group

Both goal skeletons carry a `tooling` group, for friction in the review tool rather than in what is being reviewed.
Use it for anything worth fixing later but not worth derailing the round over, and write it as a normal entry —
context, what happened, and an `ask` whose options are the fixes you would consider.

It discharges nothing, so it costs the coverage gate nothing.
One entry per round is plenty; if there is nothing to say, leave the group empty rather than inventing something.

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

   Reviewing a branch the current checkout cannot hold — the tool lives here and the change lives elsewhere — means a worktree, and `init --repo` pointing the review at it.
   **Put it under `.tmp/worktrees/<name>`**, or in your scratchpad, or wherever the user said; never in a sibling folder next to their projects.
   ```bash
   git worktree add .tmp/worktrees/pr-<n> pr-<n>
   uv run review.py init pr-<n> --repo .tmp/worktrees/pr-<n> --range origin/main..pr-<n> --goal <goal>
   ```
   `init` records that path in `review.toml`, so every later command runs bare — the review folder stays under the repo you are standing in either way.

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
   `uv run review.py generate pr-<n>` writes the generated `015-changes` and `990-coverage` first, and
   `uv run review.py changes pr-<n>` lists the ledger, which is what `discharges:` is written from.

   Read [tools/review/docs/entry-types/_index.md](../../../tools/review/docs/entry-types/_index.md) and open the types that apply.
   Four anchor a review with a changeset: **orientation** at `010`, **glossary** at `018`, the findings between, and **verdict** at `980`.
   The set is prose rather than code, so it grows by writing a file there — never by teaching the tool a new entry.

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
**Every entry outside `meta`, `finalize` and `framing` carries all three context tiers, and `validate` fails without them.**
The obligation lasts while an ask is still waiting: an entry answered in an earlier round is left alone rather than retrofitted.
`context/cold` is for a reader new to both the change and the codebase, `context/repo` for one who knows the codebase, `context/delta` for one who has read the entries above.
The first two collapse by default and have word budgets — 150 and 120 — so they cost a reader nothing until they are wanted.

**Scope each tier to that entry's own subject, not to the change as a whole.**
This is the rule that decides whether the tiers are worth having.
Cold context for an entry about a texture budget is what a mip chain and an LRU budget are, and why charging the wrong one matters — not what the branch does.
Written that way, twelve entries produce twelve distinct tiers, because they are about twelve different things.
Written as "here is what this PR adds", they produce the same paragraph twelve times, and a reader who opens two of those never opens a third.

The orientation and verdict entries are exempt because they *are* the review's cold context; repeating it there teaches exactly that skimming habit.

**Describe neutrally, then recommend separately.**
The `prose` block says what is true; the `recommendation` block says what you would do.
Mixing them is how a reviewer ends up agreeing with framing rather than with a finding.

**Anticipate the answers.**
An `ask` with well-chosen options is answered in a click; an `ask` with only a text box is a chat message wearing a costume.
Offer the two or three directions the maintainer would actually pick between, mark one `(recommended)`, and add a `check:` for the follow-ups worth doing either way.
The freeform box is always there anyway, so options cost nothing when they are wrong.

**An entry with no `ask` gets an acknowledge checkbox, automatically.**
You do not write it, and you should not add a hollow ask to get one.
If the entry is reference material — a glossary, a generated listing — declare `## auto-acknowledge` and it is left alone.

**Discharge deliberately.**
`discharges:` on an ask is what says "this question accounts for those hunks".
An LGTM entry with one ask discharging forty changes is correct and good; forty entries would not be.

**Show the code, and decide whether it opens.**
Use `changes` blocks so the hunks are there, and cite `file.hh:63` in backticks — the page turns it into a link that opens the file.
Report API shape in symbols, never in prose about symbols.

Every `changes` block must carry `show: visible` or `show: collapsed`, and the tool refuses one that does not.
Ask **can the maintainer decide this entry without the code?**
Where the prose plus a quoted excerpt or a verified repro already settles it, the full hunks are depth material — `show: collapsed`.
Only where the point genuinely cannot be judged without seeing the hunk is it `show: visible`.

`collapsed` is the common answer, and quoting the four lines that matter inside a `prose` block beats opening the whole diff.
A screen of diff costs scrolling on every visit to that entry, and the maintainer's attention is the scarcest thing a review spends.

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
