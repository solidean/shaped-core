---
name: reviewing-a-pr
description: Review a pull request together with the maintainer in chat — fetch it locally, investigate the code around it, and report numbered findings that double as a todo list for the author's next agent session. Use whenever you're asked to review a PR, look at a PR number or URL, or give a second opinion on a branch.
when_to_use: "review this PR", "look at PR #<n>", "what do you think of <pr url>", "review my branch", "second opinion on this change"
allowed-tools: Bash Read Write Edit mcp__repo_tools__repo_search mcp__repo_tools__repo_view mcp__repo_tools__repo_structure mcp__repo_tools__file_structure mcp__repo_tools__build_diag mcp__repo_tools__test_diag
---

This skill is **scaffolding only** — how to get a PR in front of you and what shape the report takes.
What we actually look for, and how we weigh the tradeoffs, lives in [docs/guides/reviewing-prs.md](../../../docs/guides/reviewing-prs.md).
Read that guide every time; it is the living record of the reviewer's taste and it changes as we review more.

## The flow

1. **Fetch the PR and pin the base against `origin/main`.**
   ```bash
   git fetch origin main --quiet
   git fetch origin refs/pull/<n>/head:pr-<n>
   git merge-base origin/main pr-<n>
   git diff --stat origin/main...pr-<n>   # three dots — the branch's own work, not main's
   gh pr view <n> --json title,body,author,state,additions,deletions,changedFiles
   ```
   **Never diff against local `main`.**
   It only moves when someone pulls, so it is usually stale and silently reports the wrong base — the same trap the `opening-a-pr` skill calls out.
   Always three dots: a two-dot diff drags in everything `main` gained since the branch forked, which buries the change under unrelated noise.

2. **Check the branch out and read the code, not just the diff.**
   `git checkout pr-<n>`, then read whole files around every hunk.
   A diff shows what moved; only the file shows whether the result still hangs together.

3. **Verify every claim against the final tree.**
   The PR body, each `///` the change touches, each cheat-sheet line, each `[done]` / `[planned]` row.
   Look the symbol up — never confirm a doc sentence by reading the sentence.
   This is where the highest-value findings are, because it is exactly what the GitHub diff view hides.

4. **Investigate.**
   Follow every symbol the change leans on to its definition, and check the callers it does not touch.
   `repo_search` over the branch is the tool; `git diff main...pr-<n> -- <paths>` narrows to one area.

5. **Build only to settle a specific question** — see the `building-and-testing` skill.
   "It compiles" and "the tests pass" are not review output; CI and `dev.py check` cover that.
   Build when you need to prove a claim, and report what you learned rather than that it ran.

6. **Write the report in chat first.** Post to GitHub only on an explicit go-ahead, as **one comment**.

## Two different artifacts

The chat report and the posted comment are **not the same document**, and conflating them is the easy mistake.
The chat report is a conversation with the reviewer; the comment is a work order for someone who was not in it.

### The chat report

Four parts, in this order.

**1. What this PR is.**
Two or three sentences in your own words.
This is how the reviewer checks you understood it before weighing anything you say about it.

**2. The API it adds or changes.**
Real symbols — signatures, type names, the enum, the handle — plus a few lines of call-site code showing it in action.
API shape is the highest-stakes thing in most PRs and the hardest to judge from prose, so show the surface, do not describe it.
Note what was deleted here too.

**3. Numbered findings.**
The reviewer answers by number, so the numbering is the interface.

- **One point per number.** Never bundle two findings — the reviewer cannot half-agree with a number.
- **Order by what it costs to get wrong**, not by file order.
- **Label the kind and the confidence.** A bug, a design call, a doc defect and a typo get answered differently.
- **Cite `file.hh:line`** as a clickable link for every point.
- Collect nits into a single trailing point.

**4. Questions.**
Only the genuinely open ones, never findings in disguise.
Each names where you saw it, gives both sides of the tradeoff, and says what you would do — so a one-word answer works.

### The GitHub comment

One comment, and it is **a task list for a fresh agent session** — not a summary of the review and not a record of the conversation.

- **Drop parts 1 and 2 entirely.** Restating what the PR is and what API it adds tells the author nothing they do not already know, and it is not actionable.
  Those two exist so the *reviewer* can check your understanding; they have no reader on GitHub.
- **Drop the questions**, and everything still open.
  Only decided instructions are posted.
- **Every point is an instruction with its reasoning under it**, executable by someone who has not read the diff and cannot ask a follow-up.
  Name the files, the call sites and the replacement.
- **It has to stand alone.** No "as we discussed", no reference to chat, no numbering that implies a conversation.
- Plain backticked `path/to/file.cc:63` beats markdown links here — relative links do not resolve reliably in a comment.

## Feed the guide

The point of reviewing in chat is that the reviewer's answers are data.
When they correct a call, reject a finding as noise, or explain a tradeoff, **write it into [docs/guides/reviewing-prs.md](../../../docs/guides/reviewing-prs.md)** in the same session.
A rule under "Settled calls" if it generalizes, an example under an existing rule if it does not.
Ask before adding a rule you are inferring rather than being told.
