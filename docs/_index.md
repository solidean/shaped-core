# shaped-core docs

The entry point for repo-wide documentation. (Library-local docs live next to their library under
`libs/<category>/<lib>/docs/`.)

## Top-level

- [coding-guidelines.md](coding-guidelines.md) — coding standards and design principles for the
  shaped-core libraries. `.clang-format` / `.clang-tidy` / `.clangd` are authoritative for
  formatting.

## Guides

See [guides/_index.md](guides/_index.md) for the full list. Most useful:

- [guides/building-and-testing.md](guides/building-and-testing.md) — how to drive `dev.py` and the
  `repo_tools` diagnostics.
- [guides/postmortem.md](guides/postmortem.md) — the session friction review (`/postmortem`).

## Per-library docs

- [clean-core](../libs/base/clean-core/docs/) — clean-core's own notes.
- [nexus catch2-runner-compat](../libs/base/nexus/docs/catch2-runner-compat.md) — how nexus test
  binaries are discovered and filtered.

> Place new repo-wide docs here (kebab-case names), in the matching subfolder. See
> [CLAUDE.md](../CLAUDE.md) for the repo's working conventions.
