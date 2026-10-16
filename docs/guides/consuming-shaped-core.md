# Consuming shaped-core from another project

How a project uses shaped-core as a git submodule: the libraries through `add_subdirectory`, and the dev.py driver imported from the submodule.
SOLIDEAN is the first such project, and the reference for everything here.

## CMake

```cmake
add_subdirectory(extern/shaped-core EXCLUDE_FROM_ALL)
```

- **Add it before defining any target of your own.**
  Its options, PCH tiers and helper functions (`sc_nexus_binary`, `sc_target_pch`, `sc_add_example`, …) exist from then on.
- `EXCLUDE_FROM_ALL` keeps `all` to what your targets link; every library is still configured.
- `SC_BUILD_TESTS`, `SC_BUILD_TOOLS` and `SC_BUILD_EXAMPLES` default to OFF here.
  Set `SC_BUILD_TOOLS` ON before the call to build shaped-linter into your tree for `dev.py lint shaped`.
- **The repo-wide build flags reach you through clean-core.**
  The x86-64 level (`CC_X64_LEVEL`), floating-point contraction, sanitizers, coverage, PGO and the wasm flags are recorded on `shaped-core::build-config`, which clean-core links `PUBLIC`.
  Some of them are ABI, so a target that uses shaped-core headers must link clean-core, directly or not — never only include its headers.
  [tools/cmake/BuildConfig.cmake](../../tools/cmake/BuildConfig.cmake) is the mechanism.
- **The end-of-configure registries run at the end of your top-level CMakeLists**, not of shaped-core's.
  The nexus binary manifest, the PCH tiers, shader packages and the wasm test pages therefore see your targets too, and you call none of the finalizers yourself.
  Register your test binaries with `sc_nexus_binary` and `dev.py test` finds them.

## dev.py

The driver is mechanism under `tools/dev/lib/` plus commands under `tools/dev/cmd/`, and both are reused from the submodule, not copied.
[dev-py-driver.md](../dev-py-driver.md) is the design; what a consuming project writes is:

- **A `dev.py`** that puts `extern/shaped-core` on `sys.path` (and its own root off it), then builds a `cmd.Policy`.
  The preset tables name presets from the project's own `CMakePresets.json`.
- **A `Layout`** in that Policy, naming the project's first-party directories: what clang-format owns, what shaped-linter and the crossref check walk, where libraries live.
  The driver's own files — the pinned clang-format, the emsdk pin — are found in the submodule without any configuration.
- **Its own `check`**, since the gate registry is project policy.
  The `Check` type, the runner and the individual gates' machinery are all reusable.
- Its own `CMakePresets.json`: the diag-launcher paths in ours are `${sourceDir}`-relative, so a consumer points them into the submodule.

## Keeping the submodules current

`uv run dev.py submodules sync` fetches each submodule's tracked branch, the `branch =` its `.gitmodules` entry names, and fast-forwards to it.
**It never touches local work**, so it is safe to run at any time:

| The submodule is | sync |
|---|---|
| on its tracked branch, clean, nothing unpushed | fast-forwards it |
| detached at a commit the branch contains, as `git submodule update` leaves it | checks the branch out, then fast-forwards it |
| carrying uncommitted changes to tracked files | refuses |
| carrying commits its remote branch lacks, on the branch or detached | refuses |
| on another branch | skips it |
| without a `branch =` | skips it |

Untracked files do not count: a fast-forward never touches one, and git refuses rather than overwrite it.
Nothing is ever reset, rebased or merged, and a refusal exits non-zero, naming what is in the way.

Moving a submodule changes the commit the superproject records.
`--bump` commits each moved pointer in a commit of its own; a project's own bump rules still apply, and its `check` enforces them.
`--dry-run` reports what would move, and `sync <path>` limits it to some submodules.

Set `update = merge` on each `.gitmodules` entry as well.
Then a bare `git submodule update --remote` merges into the checked-out branch instead of detaching HEAD, so even the raw git command strands no commit.

## What does not carry over yet

- `lint clang-tidy` runs `tools/lint/clang-tidy.py`, which roots itself at shaped-core and reads shaped-core's gate list.
- `deps`, `install` and `test-web` expect shaped-core's own files at the project root.
- The prerequisite fetchers (DXC, SDL3, SQLite, …) look under the project's `extern/`, so a consumer gets those optional dependencies only by vendoring or fetching them itself.
- `.shaped-lint.yml` configs merge along a file's own ancestors, so a consumer copies the root policy rather than inheriting it.
