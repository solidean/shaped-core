# Starting a project on shaped-core

How a fresh repository becomes a project built on shaped-core, in our house style, with nothing but this page and a sentence of intent.
It is written to be followed by an agent as much as by a person, so every step says what to do by default.
**Custom instructions win**: whatever the request says about the project overrides a default here, and the rest of this page still applies.

The prompt it answers is the README's quick start:

```text
Add <shaped-core url> to extern/shaped-core and follow the guide at extern/shaped-core/docs/guides/starting-a-project.md to set up the project.
<optional: what the project is, what it is called, what it needs>
```

[consuming-shaped-core.md](consuming-shaped-core.md) is the mechanism behind every step: the CMake side, the driver's seams, the submodule workflow.
This page is the order to do things in and the defaults to choose.

## 1. Add shaped-core

```bash
git submodule add -b main <shaped-core url> extern/shaped-core
git config -f .gitmodules submodule.extern/shaped-core.update merge
git submodule update --init --recursive
```

- **`-b main` records the branch** that `dev.py submodules sync --tip` and `--bump` follow.
- **`update = merge`** keeps a bare `git submodule update --remote` from detaching HEAD, as [Keeping the submodules current](consuming-shaped-core.md#keeping-the-submodules-current) says.
- The repository's own branch is `main`; rename a fresh repository's default branch to it if it is something else.

## 2. Lay out the project

```bash
uv run extern/shaped-core/tools/project-template/new-project.py --name <project> [--lib <first-lib>] [--namespace <ns>]
```

It writes the deterministic part and refuses before writing anything if a file it would write exists already:

- `CMakeLists.txt`, which adds shaped-core with `SC_LIBRARIES` and `SC_BUILD_TOOLS`, then the sample library.
- `CMakePresets.json`, which includes shaped-core's presets rather than copying them, so the preset names are shaped-core's.
- `dev.py`, shaped-core's driver imported from the submodule, with this project's policy.
- `.clang-format`, `.editorconfig` and `.clangd` copied from shaped-core, and `.shaped-lint.yml` with its doc links pointed into the submodule.
- `.gitignore`, `.gitattributes`, `.issues.toml`, `README.md`, `docs/_index.md`, `docs/issues/` and `apps/`.
- One sample library, `libs/base/<first-lib>`, with a test, so the first build has something to build and test.

`--name` is kebab-case and is the CMake `project()`.
`--lib` defaults to the project's name and `--namespace` to the library's name in snake_case; pick a short namespace when the request names none, as ours are (`cc`, `tg`, `sg`).
[tools/project-template/](../../tools/project-template/readme.md) is the template, and its self-test is the `project-template-selftest` gate.

Then the parts that need judgement, in this order: the layout, dev.py's policy, the build server, the issue tracker, CLAUDE.md, the skills.

## 3. The folder layout

```text
libs/<category>/<lib>/      a library: src/<lib>/ (.hh and .cc side by side), tests/ (<lib>-test), optional examples/ and docs/
apps/<app>/                 an executable, thin: main and wiring, what it does lives in a library
docs/                       repo-wide docs, docs/_index.md the hub; docs/issues/ the repo-wide issues
tools/                      the project's own scripts and tools, each with a readme
extern/shaped-core          the submodule; other dependencies beside it under extern/
.claude/skills/             the agent skills
```

- **Libraries are layered**: a library depends only on lower ones, and on shaped-core's.
  A category is a layer or a domain (`base`, `data`, `graphics` in shaped-core), so the directory says what may depend on what.
  The sample library sits in `base/`; rename it into the project's first real library rather than keeping a placeholder.
- **A category is required even with one library**: coverage and the shared lint policy key on `libs/<category>/<lib>/`.
- **Each library's `CMakeLists.txt` is added from the root**, in dependency order, after `add_subdirectory(extern/shaped-core ...)`.
- **A test binary is `<lib>-test`, registered with `sc_nexus_binary(<target> KINDS tests)`**, which is how `dev.py test` finds it.
- The size limits are the house ones: files of about 200–800 lines, directories of about 5–15 direct entries, split by responsibility past that.

## 4. dev.py

The generated `dev.py` is policy only; every command is shaped-core's.

- **Presets**: the tables name presets from shaped-core's `CMakePresets.json`, which the project's includes.
  A preset of the project's own goes into its `CMakePresets.json` beside the include, inheriting shaped-core's hidden bases.
- **`LAYOUT`** names the first-party roots: `libs` and `apps` for clang-format, those plus `docs`, `tools` and `.claude/skills` for shaped-linter and crossrefs.
  A new top-level directory of the project's own is added there, or its files are silently unlinted.
- **`checks`** is the `check` registry: `check.standard_registry` runs shaped-core's standard gates, `STANDARD_CHECKS` in [check.py](../../tools/dev/cmd/check.py).
  A gate of the project's own is a function with the same signature, returning the standard gates with its own placed among them; `shaped_core_registry` is the worked case.
- **`SC_LIBRARIES`** in `CMakeLists.txt` names the shaped-core libraries the project links, and grows with the first `#include` that needs one.
  The cheat sheets under `extern/shaped-core/libs/` say what each library offers.
- **Not registered**: `deps`, `install` and `test-web`, which still expect shaped-core's own files at the root, and `lint clang-tidy` does not carry over yet either.
  [What does not carry over yet](consuming-shaped-core.md#what-does-not-carry-over-yet) is the list.

## 5. The build server

**When a build-server client is installed, opt this checkout in**; when none is, skip the step, since everything builds without one.
ccs is the build server the driver's protocol is written against, and on Windows its client installs to `%LOCALAPPDATA%\ccs\bin\ccs.exe`.
Look for it there, or ask, and write a `dev.toml` at the repository root:

```toml
[build_server]
enabled = true
client = "C:/Users/<user>/AppData/Local/ccs/bin/ccs.exe"
```

- **`dev.toml` is per checkout and gitignored**: it names a path on one machine, so it is never committed, and the template's `.gitignore` already excludes it.
- The project's `dev.py` reads it through `dev.build_server_from_dev_toml(ROOT)`, which the template already passes.
- `--no-server` and `SC_BUILD_SERVER=0` still win, and a server that cannot serve is one warning and a plain build.
  [An optional build server](building-and-testing.md#an-optional-build-server) is the rest: what moves, the test cache, `ship --proof`.

## 6. Issue tracking

- **The tracker is already wired**: `dev.py issues` is registered, `docs/issues/` exists, and `check`'s `issues` gate validates every issue file and marker.
- **`.issues.toml` lists `extern/shaped-core` as a submodule**, so `dev.py issues known <query>` finds a bug filed in shaped-core too, as `shaped-core:<id>`.
- A library's own issues go in its `docs/issues/`; the repository's in the root one.
- [issue-tracking.md](issue-tracking.md) is the format and the lifecycle, and the `issue-tracking` skill (step 8) is how an agent drives it.

## 7. CLAUDE.md

**The project's CLAUDE.md is a copy, specialized at once**: start from `extern/shaped-core/CLAUDE.md`, then make every section about this project.
It never imports shaped-core's, since the copy is the project's to change.
Section by section, unless the request says otherwise:

| shaped-core's section | In the project's CLAUDE.md |
|---|---|
| the opening "quick orientation, not for knowledge" paragraph | copy verbatim |
| What this repo is | write anew: what the project is, its language and build, `dev.py`, that shaped-core is at `extern/shaped-core` |
| Project layout | write anew in the same form: a one-liner per library and app, its namespace and its dependencies; the supporting directories |
| Hard rules | copy dependency direction, `.clang-format` authoritative, the `building-and-testing` skill, the disk-space escalation, never running a nexus binary directly, `*-example` naming, absolute paths, straight to `main`, never waiting for CI, no force-push |
| | the compatibility rule becomes the project's stance: copy shaped-core's beta wording unless told the project has shipped; drop the `.vdoc` line |
| The libraries are living | copy, adding that a missing capability belonging in shaped-core is surfaced as a shaped-core extension, not worked around locally |
| Build & test (essentials) | copy, with the commands the project's `dev.py` registers |
| Build flags / presets | keep C++23, the default presets, `--preset` after the subcommand and `CC_ASSERT` on in `relwithdebinfo-*` only; add the project's `SC_LIBRARIES`; drop the rest |
| Strong guidelines | copy verbatim: file size, directory branching, fast tests with no deliberate waiting, codifying edge cases |
| Style preferences, Prose, The style is evolving | copy verbatim, pointing the links at `extern/shaped-core/docs/` |
| Docs | copy, with `docs/_index.md` the project's hub |
| Agent skills | list the skills step 8 copies |
| Exploring the codebase | copy where the `repo_tools` MCP server is set up, else drop |
| Cheat sheets | copy, pointing at shaped-core's sheets under `extern/shaped-core/libs/`, and asking for one per project library too |
| Git workflow | copy: `main`, merging pulls, `check --fix` before every push, no push without the user's intent, the attribution rule, the heredoc for commit messages |
| Quick reference | copy the rows whose commands the project has |

Links in the copied text resolve from the project's root, so `docs/coding-guidelines.md` becomes `extern/shaped-core/docs/coding-guidelines.md`; the `crossrefs` gate finds any left over.

## 8. Skills

Copy these from `extern/shaped-core/.claude/skills/` into `.claude/skills/`, then make each about this project:

- **`building-and-testing`**, **`issue-tracking`**, **`reworking-prose`** and **`postmortem`** always.
- **`disassembly`** and **`analyzing-recordings`** where the project measures performance; **`debugging-ci`** once it has CI.
- Their links point into the project, so each `../../../docs/...` becomes `../../../extern/shaped-core/docs/...`, and the `crossrefs` gate checks the result.
- Leave out the skills that are shaped-core's own work: reviewing its PRs, modernizing its old code.

## 9. Verify

```bash
uv run dev.py test
git add -A && git commit -m "Lay out the project on shaped-core"
uv run dev.py check --all
```

- **`check --all`, not plain `check`**: a fresh repository has no `origin/main` for `check` to scope itself against.
- Lint the first doc and the CLAUDE.md as soon as they exist, `uv run dev.py lint shaped --dirty-only`, rather than after the last.
- **The setup is done when `check --all` is green**, CLAUDE.md included, since shaped-lint and crossrefs read it.
- Pushing is the user's call, as anywhere: commit, then ask.
