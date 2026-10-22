# Issue tracking

Issues live in the repository, one markdown file each, next to the code they are about.
**The files are the tracker.**
Every operation below can be done by hand in an editor; [tools/issues/issues.py](../../tools/issues/issues.py), `dev.py issues` in a repository that wires it, is comfort on top.

What it is for: what is broken, planned or open, written down so it survives the session that found it, and findable from the code it concerns.

## Where issues live

**Any `docs/issues/` folder is an issue folder, and the directory owning that `docs/` is its scope.**
`docs/issues/` is the repository's own, `libs/base/clean-core/docs/issues/` clean-core's, `tools/deploy/docs/issues/` a tool's.
Nothing is registered: creating the folder starts tracking there.

```text
<scope>/docs/issues/
  bug-test-pattern-unbuilt-binary.md      an open issue
  epic-axis-aligned-predicates.md
  closed/2026/10/feat-segment-predicates.md   closed in October 2026
```

- **The file name is the id: `<type>-<slug>.md`**, the slug lowercase kebab-case.
- **An id is unique in its repository, open and closed together, forever.**
  So `bug-<slug>` names one issue anywhere — in code, a commit message, a chat — with no path.
- **Closed issues move to `closed/<yyyy>/<mm>/`** of the same folder, the month two digits so the folders sort.
  The open folder stays short, and the month keeps a busy year's folder browsable.

## Types

The type is the file name's prefix and nothing else, so it cannot disagree with a second copy.

| Type | For |
|---|---|
| `bug` | Something behaves wrongly. |
| `feat` | Something wanted that does not exist yet, sized to be one piece of work. |
| `perf` | Something correct but slower or bigger than it should be. |
| `debt` | A workaround, a shortcut or a known wart, typically marked `TEMPORARY(...)` in code. |
| `chore` | Maintenance with no behavior change: a bump, a migration, a cleanup. |
| `question` | A decision someone has to make before work can go on. |
| `epic` | An umbrella over several issues, such as "booleans"; its children name it as `parent:`. |

A `feat` the size of "booleans" is an epic; a feature is something one person can claim and finish.

## An issue file

```markdown
---
created: 2026-10-16
priority: next
tags: [dev.py]
parent: epic-test-driver
blocked-by: [feat-other-thing]
refs:
  - test: app-viewer-example
  - file: tools/dev/cmd/test.py:120-140
  - symbol: dev.test
claimed-by: Philip Trettner
claimed-at: 2026-10-16T14:03Z
---

# `dev.py test <pattern>` crashes on an unbuilt matching binary

`test "kernels"` also matched `app-viewer-example`, which is not built without `APP_VIEWER` …
```

**The title is the body's first line, `# <title>`, and is the only thing an issue needs.**
A file holding just a title is a valid open issue; the frontmatter is optional.

The frontmatter is YAML, and every key is optional.
**Keys not listed here are allowed and kept**, so an issue collects whatever data it needs.

| Key | Value |
|---|---|
| `created` | `yyyy-mm-dd` |
| `priority` | `next`, `normal` (the default) or `someday` |
| `tags` | a list |
| `parent` | the id of the epic this belongs to; an epic has no parent |
| `blocked-by` | a list of ids |
| `refs` | a list of `- <kind>: <value>` items, below |
| `claimed-by`, `claimed-at` | who works on it, and since when: `yyyy-mm-ddThh:mmZ`, UTC; always both |
| `closed`, `resolution` | on a closed issue only: `yyyy-mm-dd`, and `fixed`, `wontfix`, `duplicate` or `obsolete` |

The body is free markdown under the repository's prose rules.
It says what is wrong or wanted and what is already known, and grows a plan or a log as work goes on.
What an issue teaches beyond its own life — a design decision, a measured invariant — belongs in the docs or a test when it lands, not only in the closed issue.

## Refs and markers: is this a known issue?

An issue points at what it concerns with `refs`:

| Kind | Value | Matches |
|---|---|---|
| `file` | a repository-relative path, optionally `:<line>` or `:<first>-<last>` | that file, or every file below a directory; with lines, only an overlapping line |
| `test` | a test or binary name, globs allowed | a test name the glob matches, or that contains it |
| `symbol` | a qualified name such as `sol::h256::intersect` | the name, or any `::`-suffix of it |
| `issue` | another id | — |

Line numbers drift as code changes; prefer a symbol or a whole file where either is precise enough.

Code points back at an issue with a **marker**:

- **`issue: <id>`** in a comment, at the code an issue concerns.
- **`TEMPORARY(<id>)`** on a workaround, naming the `debt` issue that replaces it.

These are the only spellings of open work in code: a bare `TODO`, `FIXME`, `HACK` or `XXX` in a comment is a shaped-linter finding (`no-bare-todo`).

`issues known <query>` answers "is this known?" for a path, a `path:line`, a test name, a symbol or plain text.
It searches refs, markers and the text, closed issues included and marked, since a fixed issue is what a regression looks for.
A marker counts for its whole file, since it sits above the code it marks rather than on the line a query names.

## Lifecycle

### Open

`issues new <type> <slug> [--in <dir>]` writes `<dir>/docs/issues/<type>-<slug>.md` and commits nothing.
By hand: create the file.

### Claimed — pushed at once

**Whoever starts on an issue claims it, and the claim reaches the remote immediately**, so two people never pick up the same work.
A claim is `claimed-by` and `claimed-at` in the frontmatter, in a commit of its own, `claim <id>`.

`issues claim <id>` does that without touching anything else of the local state:

1. It fetches the integration branch and reads the issue there, refusing if someone holds it.
2. It builds the claim commit directly on the remote branch, not on the local `HEAD`, from that one file.
3. It pushes exactly that commit.
   If someone pushed in between, the push is rejected, and it starts over from the new state.
4. It brings the checkout up to date: a fast-forward, or a merge when the local branch has unpushed commits of its own.

Unpushed local commits and uncommitted changes to other files stay where they are.
An issue file not yet committed anywhere is added by its claim commit, so filing and claiming is one step.

**A claim or release commit is exempt from the pre-push gate**: it changes one issue file, and pushing it alone is the point.
Everything else still goes through the repository's gate.

An agent claims on someone's behalf: `--as "Claude (for <name>)"`.
`issues release <id>` removes a claim the same way, and `--force` takes over or releases someone else's.
`issues list --stale <days>` finds claims nobody touched for a while.

**A leftover claim whose work has landed may be cleared by anyone, without asking.**
Nothing clears a claim automatically in every case, so a claim outlives its work: a closed issue still carrying `claimed-by`, or an open one whose resolving commit is already in the history.
When the commit history shows that closing, remove the claim: `issues release <id> --force` on an open issue, the two lines by hand on a closed one.

By hand: edit the two lines, commit only that file, push.

### Closed

**An issue closes in the commit that resolves it**, never on its own.
`issues close <id> [--resolution fixed]` adds `closed` and `resolution`, moves the file to `closed/<yyyy>/<mm>/` with `git mv`, and commits nothing.
It rewrites the moved file's own relative links, and every markdown link in the repository that pointed at it.

`issues reopen <id>` moves it back.
By hand: move the file and add, or remove, the two lines.

## Triage in the browser

`issues serve` opens a local web page over the issue files: a filtered list, each issue rendered in a side panel, and a graph of what blocks what and which epic holds which issue.
It also edits: priority, tags, epic, `blocked-by`, a claim, and a `## Notes` section in the body, plus renaming a tag on every issue and bulk tagging.

**An edit writes the file and nothing else.**
The page shows how many issue files differ from `HEAD`, edited there or anywhere else, and its **Commit & push** is the only thing that commits.
It commits the chosen issue files alone, on top of the fetched remote branch, the way a claim does, so other local work stays local.
It refuses a file the remote changed since `HEAD`, a file `issues check` faults, and an issue naming one that is not committed yet unless that one goes along.

A claim made there is an edit like any other, pushed with the rest.
`issues claim` pushes at once because an agent needs the claim visible before it starts; a person triaging does not.

**A triage commit is exempt from the pre-push gate**, as a claim is: it changes only issue files, and its own check runs before it pushes.

The page binds to `127.0.0.1` and answers only loopback host names, and a write needs the token the page was served with.
Rendering and the graph load marked, DOMPurify and cytoscape from cdn.jsdelivr.net.
It needs `tools/issues/` checked out, so unlike the other commands it does not run from a URL.

## Several repositories

Each git repository is its own tracker: its issues, its ids, its integration branch.
**The repository is the git checkout the command runs in**, so the tool works the same in every one.

`.issues.toml` at a repository's root configures it, every key optional:

```toml
name = "app"                                           # how other repositories qualify its ids (default: its directory's name)
branch = "main"                                        # the integration branch claims push to
remote = "origin"
submodules = ["extern/engine", "extern/api"]           # listed, searched and checked with this one by default
exclude = ["tools/issues/issues-self-test.py"]         # paths never read as issues or scanned for markers
```

A submodule's integration branch is its own `.issues.toml`'s, else the `branch =` its superproject's `.gitmodules` records, else its remote's default.

**From a superproject, a submodule's issue is `<name>:<id>`**: `engine:bug-<slug>`, and the same in a marker or a ref.
A bare id resolves in the issue's own repository first.
`--with-submodules` reaches every checked-out submodule, `--top-only` none.
Claiming a submodule's issue pushes to the submodule's remote.

## The CLI

| Command | Does |
|---|---|
| `new <type> <slug>` | create an issue: `--in`, `-t`, `--body`, `--priority`, `--tag`, `--parent`, `--ref kind:value` |
| `list` | open issues, epics' children indented: `--type`, `--tag`, `--in`, `--repo`, `--claimed`, `--unclaimed`, `--mine`, `--stale`, `--closed`, `--all`, `-g` |
| `show <id>` | the issue; an epic also counts its children |
| `known <query>` | the issues a file, line, test, symbol or text belongs to; exit code 1 when none |
| `claim <id>`, `release <id>` | one commit, pushed alone: `--as`, `--force` |
| `close <id>`, `reopen <id>` | move it: `--resolution`, `--date` |
| `check` | validate every issue and every marker |
| `serve` | the web page above: `--port`, `--no-open` |

An `<id>` may also be `repo:id`, a fragment unique among the ids, or the file's path.

`check` is a `dev.py check` gate.
It holds the file name, location and frontmatter to this page, ids unique, `parent` and `blocked-by` and `issue` refs resolving, and an open issue's `file` refs existing.
A `file` ref to a path git ignores passes even when absent: it names a fetched file, which this machine may not have.
**A marker naming an unknown or a closed issue fails it**: a fixed issue's workaround or comment has to go with it.

## Without shaped-core

The tool is one file needing only `uv`.
A repository without shaped-core runs it from GitHub, pinned to a commit:

```bash
uv run https://raw.githubusercontent.com/solidean/shaped-core/<commit>/tools/issues/issues.py list
```
