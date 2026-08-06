---
name: opening-a-pr
description: How to commit work and open a pull request in shaped-core — branch naming, commit attribution, and the PR description style guide. Use whenever you're asked to open/create a PR, push a branch for review, or write a PR description.
when_to_use: "open a PR", "create a pull request", "push this for review", "raise a PR", "write the PR description", "make a PR"
allowed-tools: Bash Read
---

## The flow

1. **Branch first — never commit to `main`.** Feature branches are mandatory and
   namespaced by contributor initials: `u/<initials>/<feature>` (Philip Trettner
   is `pt`, so `u/pt/<feature>`; use the actual author's initials).
   ```bash
   git switch -c u/pt/<feature>
   ```
   If you already committed on `main` by mistake, move the commits to a branch
   and reset `main` — don't push to `main`.

2. **Review scope before committing.**
   `git status --short` and `git diff --stat HEAD` — confirm only the intended work is staged and nothing unrelated rode along.

   **Always `git fetch origin main` first, then diff against `origin/main`, not local `main`.**
   Local `main` is usually stale, since it only moves when you pull, so `main..HEAD` silently reports the wrong commits and diff.
   Use `git fetch origin main --quiet && git log --oneline origin/main..HEAD`, and `git diff --stat origin/main..HEAD`, to see what the PR actually contains.

3. **Commit in logical units.**
   Group related changes, and split genuinely separate threads into separate commits; prefer a new commit over amending an existing one.
   Multi-line messages go through a heredoc with the **Bash** tool — never PowerShell here-string syntax (`@'...'@`), which Bash takes literally:
   ```bash
   git commit -F - <<'EOF'
   Short imperative subject (<= ~72 chars)

   Body explaining the why, wrapped at ~72 columns.

   Assisted-By: Claude Code <claude-opus-4-8>
   EOF
   ```
   Add `Assisted-By: Claude Code <model-id>` for largely Claude-generated commits, using the exact model id.
   Not `Co-Authored-By`, and skip the trailer entirely for human-written or trivial agent edits.

4. **Push and open the PR.**
   ```bash
   git push -u origin u/pt/<feature>
   gh pr create --base main --head u/pt/<feature> --title "<title>" --body "$(cat <<'EOF'
   <body — see style guide below>
   EOF
   )"
   ```
   Stop and surface the problem if push or `gh` needs auth you don't have — don't retry blindly or force anything.
   **Never force-push to `main`.**

## PR description style guide

Optimize for reviewer efficiency.
Assume reviewers can read the code and the commit diff, so the description conveys **intent and high-level design**, not the investigation.
Aim for **~5–15 lines total**, structured as:

```
## Summary

One or two sentences: the problem being solved and the overall fix.

## Changes

- Concrete change per file/area, one bullet each.
- What it does, not how you discovered it.
```

**Avoid:**
- Long narratives, timelines, or "Problem / Root Cause / Verifying" sections
  unless genuinely necessary.
- Recounting every intermediate discovery made while debugging.
- Reviewer/setup instructions unless they're actually required.
- Marketing language, emojis, or "Generated with Claude Code" footers.

**Validation section:** include one *only* when testing is non-obvious, needs manual steps, or CI cannot cover it.
Otherwise omit it.

## Reference

This skill owns the PR flow.
[CLAUDE.md](../../../CLAUDE.md)'s "Git workflow" section is the summary an agent has loaded before it gets here, and [docs/guides/ci.md](../../../docs/guides/ci.md) is what the PR checks are running.
