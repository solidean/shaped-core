# issues — in-repo issue tracking

One markdown file per issue, in any `docs/issues/` folder of a git repository.
[docs/guides/issue-tracking.md](../../docs/guides/issue-tracking.md) is the system: the format, the lifecycle, and the CLI.

- [issues.py](issues.py) is the whole tool, one file needing only `uv`, so a repository without shaped-core can run it from a URL.
  Standalone it is `uv run tools/issues/issues.py <command>`; a `dev.py` registers it as `dev.py issues` through [tools/dev/cmd/issues.py](../dev/cmd/issues.py).
- [issues_serve.py](issues_serve.py) and [web/](web/) are `issues serve`, loaded only by that command, so `issues.py` stays one file for every other.
  The page is plain HTML, CSS and JavaScript with no build step; it reads the files fresh on every request and holds no state of its own.
- [issues-self-test.py](issues-self-test.py) is its suite, the `issues-selftest` gate of `dev.py check`.
  It claims against a real bare remote in throwaway repositories, so the one-commit push and its retry are tested as git does them.

**The tool never re-serializes YAML.**
It reads the frontmatter with PyYAML's `BaseLoader`, every scalar a string, and writes by replacing whole top-level lines, so a hand-edited file keeps its comments, order and style.

**A claim never pushes local work, and neither does the page's Commit & push.**
Both go through `_push_commit`: the commit is built on the fetched remote branch through a temporary index, `commit-tree` and a push of that one commit, and only then merged into the checkout.
