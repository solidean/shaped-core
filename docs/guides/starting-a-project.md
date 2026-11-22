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
git submodule add <shaped-core url> extern/shaped-core
git submodule update --init --recursive
```

- **Step 2 sets the `.gitmodules` entry** to follow `main` and to merge on update, where it says nothing else already.
  The branch is what `dev.py submodules sync --tip` and `--bump` follow.
  `update = merge` keeps a bare `git submodule update --remote` from detaching HEAD, as [Keeping the submodules current](consuming-shaped-core.md#keeping-the-submodules-current) says.
- The repository's own branch is `main`; rename a fresh repository's default branch to it if it is something else.

## 2. Lay out the project

```bash
uv run extern/shaped-core/tools/project-template/new-project.py --name <project> [--lib <first-lib>] [--namespace <ns>]
```

It writes the deterministic part, and refuses before writing anything if a file it would write exists already.
**A repository created on GitHub starts with a README**, and `--skip-existing` keeps it and writes the rest; adapt the kept files by hand.

- `CMakeLists.txt`, which adds shaped-core with `SC_LIBRARIES` and `SC_BUILD_LINTER`, then the sample library.
- `CMakePresets.json`, which includes shaped-core's presets rather than copying them, so the preset names are shaped-core's.
- `dev.py`, shaped-core's driver imported from the submodule, with this project's policy.
- `.clang-format`, `.editorconfig` and `.clangd` copied from shaped-core, and `.shaped-lint.yml` with its doc links pointed into the submodule.
- `CLAUDE.md`, shaped-core's house sections with `Specialize:` lines where the project's own go (step 7).
- `.gitignore`, `.gitattributes`, `.issues.toml`, `README.md`, `docs/_index.md`, `docs/issues/` and `apps/`.
- One sample library, `libs/base/<first-lib>`, with its `fwd.hh` and a test, so the first build has something to build and test.
  A type the library defines is declared in its `fwd.hh` and defined qualified, which shaped-linter's `qualified-type-definition` holds it to.

`--name` is kebab-case and is the CMake `project()`.
`--lib` defaults to the project's name and `--namespace` to the library's name in snake_case; pick a short namespace when the request names none, as ours are (`cc`, `tg`, `sg`).
[tools/project-template/](../../tools/project-template/readme.md) is the template, and its self-test is the `project-template-selftest` gate.

Then the parts that need judgement, in this order: the layout, dev.py's policy, the build server, the issue tracker, specializing CLAUDE.md, the skills.

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
- **`SC_LIBRARIES`** in `CMakeLists.txt` names the shaped-core libraries the project links: add one with the first `#include` that needs it.
  It is a plain variable rather than a cache entry, so the edit reaches a build folder that is configured already.
  The cheat sheets under `extern/shaped-core/libs/` say what each library offers.
- **`SC_BUILD_LINTER`** builds shaped-linter and none of shaped-core's other tools.
  `SC_BUILD_TOOLS` would add those too, and the ones that carry tests would join this project's `dev.py test`.
- **The fetched dependencies land in the submodule**: dev.py runs DXC's, SDL3's and SQLite's fetch scripts into `extern/shaped-core/extern/`, where shaped-core's CMake looks.
- **Not registered**: `deps`, `install` and `test-web`, which still expect shaped-core's own files at the root, and `lint clang-tidy` does not carry over yet either.
  [What does not carry over yet](consuming-shaped-core.md#what-does-not-carry-over-yet) is the list.

### A project that draws

The graphics stack builds in a project as in shaped-core, with a few lines of its own:

- **`SC_LIBRARIES` names the top it uses**, `shaped-rendering` or `shaped-viewer`, which bring shaped-graphics, the shader library and SGL with them.
- **A backend is a target to link**, `shaped-graphics-dx12`, `-vulkan`, `-metal` or `-webgpu`, each present only where it builds: test for it with `if(TARGET ...)`.
  `SC_HAS_DXC_COMPILER` and `SC_HAS_METAL_COMPILER` say whether the compiler behind an SGL package exists.
- **Shaders are a package on the executable**: `sc_add_shader_package(TARGET <exe> NAME <pkg> NAMESPACE shaders SOURCE_DIR shaders LANGUAGE sgl SHADERS <file>.sgl:*)`.
  The package stages DXC's DLLs beside its target itself.
- **On Windows the executable stages its other DLLs**, SDL3's among them: `sc_stage_runtime_dlls(<exe> DLLS "$<TARGET_RUNTIME_DLLS:<exe>>")`.
- `examples/graphics/sgl-cube/` in the submodule is the smallest whole case, and `libs/graphics/shaped-rendering/tools/lod-probe/` one linking two backends at once.

## 5. The build server

**Nothing to do where ccs is installed**: the project builds and tests through it by default, as shaped-core does.
ccs is the build server the driver's protocol is written against, and its client is found at `%LOCALAPPDATA%\ccs\bin\ccs.exe` or on PATH.
Where it is not installed, everything builds the plain way.

- The project's `dev.py` decides through `dev.build_server_from_dev_toml(ROOT)`, which the template already passes.
- **A `dev.toml` at the repository root** changes it for one checkout: `[build_server]` with `enabled = false` turns it off, and `client = "<path>"` names a client installed elsewhere.
  It is per checkout and gitignored, and the template's `.gitignore` already excludes it.
- `--no-server` and `SC_BUILD_SERVER=0` still win, and a server that cannot serve is one warning and a plain build.
  [The build server](building-and-testing.md#the-build-server) is the rest: what moves, the test cache, `ship --proof`.

## 6. Issue tracking

- **The tracker is already wired**: `dev.py issues` is registered, `docs/issues/` exists, and `check`'s `issues` gate validates every issue file and marker.
- **`.issues.toml` lists `extern/shaped-core` as a submodule**, so `dev.py issues known <query>` finds a bug filed in shaped-core too, as `shaped-core:<id>`.
- A library's own issues go in its `docs/issues/`; the repository's in the root one.
- [issue-tracking.md](issue-tracking.md) is the format and the lifecycle, and the `issue-tracking` skill (step 8) is how an agent drives it.

## 7. CLAUDE.md

**The project's CLAUDE.md is a copy, specialized at once**: step 2 wrote one, and it is the project's to change, never an import of shaped-core's.
The house sections are already shaped-core's, their links pointed into the submodule:
the hard rules, the living libraries, build & test, the strong guidelines, the style and prose rules, docs, cheat sheets and the git workflow.
What is left, unless the request says otherwise:

- **Replace every `Specialize:` line**: what the project is, and the one-liner per library with its namespace and dependencies.
  The setup is not done while one is left.
- **Write what the request said** into the sections it concerns: a library planned, a platform it targets, a rule of its own.
- **Grow the layout list as libraries and apps land**, one line each, as shaped-core's CLAUDE.md does.
- **Add a section for a skill copied in step 8**, and the `repo_tools` MCP server's section from shaped-core's CLAUDE.md where it is set up.
- **Keep the compatibility rule's beta stance** until the user says the project has shipped.

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
