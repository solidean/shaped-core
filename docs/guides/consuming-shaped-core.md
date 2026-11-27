# Consuming shaped-core from another project

How a project uses shaped-core as a git submodule: the libraries through `add_subdirectory`, and the dev.py driver imported from the submodule.
SOLIDEAN is the first such project, and the reference for everything here.
[starting-a-project.md](starting-a-project.md) is the walkthrough for a fresh repository, over a template that writes what this page describes.

## CMake

```cmake
add_subdirectory(extern/shaped-core EXCLUDE_FROM_ALL)
```

- **Add it before defining any target of your own.**
  Its options and helper functions (`sc_nexus_binary`, `sc_add_example`, …) exist from then on.
- `EXCLUDE_FROM_ALL` keeps `all` to what your targets link; every library is still configured.
- `SC_BUILD_TESTS`, `SC_BUILD_TOOLS` and `SC_BUILD_EXAMPLES` default to OFF here.
  Set `SC_BUILD_LINTER` ON before the call to build shaped-linter into your tree for `dev.py lint shaped`, a `SC_LIBRARIES` subset included.
  `SC_BUILD_TOOLS` configures every shaped-core tool, which an SGL shader package needs for `sgl`.
  Under `EXCLUDE_FROM_ALL` a tool is built only when something depends on it, so the tests one carries (`lod-probe`'s) stay out of your `dev.py test`.
- **The repo-wide build flags reach you through clean-core.**
  The x86-64 level's flag, floating-point contraction, sanitizers, coverage, PGO and the wasm flags are recorded on `shaped-core::build-config`, which clean-core links `PUBLIC`.
  [tools/cmake/BuildConfig.cmake](../../tools/cmake/BuildConfig.cmake) is the mechanism.
- **What is ABI is clean-core's generated `clean-core/build_config.hh`**, not a compile definition: the configuration, `CC_X64_LEVEL`, threading, source sites.
  `macros.hh` includes it, so a target using shaped-core headers still links clean-core, directly or not, for its include directory.
- `SC_LIBRARIES` names the libraries you need; only they and their shaped-core dependencies are added.
- The fetched dependencies (DXC, SDL3, SQLite, …) are fetched into the submodule's `extern/`, where its CMake reads them, by your dev.py as by ours.

## An installed shaped-core

`-DSC_INSTALL=ON` adds install rules for every built library, and `cmake --install` lays out a CMake package:

```cmake
find_package(shaped-core REQUIRED)
target_link_libraries(app PRIVATE shaped-core::shaped-graphics-language)
```

- **The consumer defines nothing and passes no flag**: the exported targets carry no compile definition and no code-generation option.
  `clean-core/build_config.hh` holds the configuration they were built in.
  A consumer must still be compiled for at least the build's `SC_X64_LEVEL`, since inline functions of the headers are compiled in its TUs.
  The one option the targets carry is MSVC's `/Zc:preprocessor`, since clean-core's headers use `__VA_OPT__`.
- **Externals come from your package manager when `SC_USE_VENDORED_<X>=OFF`.**
  clean-core's six are then found by `find_package` (`mimalloc`, `xxHash`, `BLAKE3`, `zstd`, `lz4`, `ZLIB`), and `shaped-coreConfig.cmake` finds them again.
  Vendored ones are installed as archives beside the libraries, under `shaped-core::` names, their licenses in `share/shaped-core/licenses/`.
- A library whose externals cannot be installed yet, the graphics stack's fetched SDKs among them, fails configure by name.
- `SC_PACKAGE_VERSION` (default `0.0.0`) is the version the package reports, for a recipe to set.
- `uv run tools/install-test/install-test.py` is the test: it installs `SC_LIBRARIES=shaped-graphics-language` and builds a consumer against it.
- **The end-of-configure registries run at the end of your top-level CMakeLists**, not of shaped-core's.
  The nexus binary manifest, shader packages and the wasm test pages therefore see your targets too, and you call none of the finalizers yourself.
  Register your test binaries with `sc_nexus_binary` and `dev.py test` finds them.

## dev.py

The driver is mechanism under `tools/dev/lib/` plus commands under `tools/dev/cmd/`, and both are reused from the submodule, not copied.
[dev-py-driver.md](../dev-py-driver.md) is the design; what a consuming project writes is:

- **A `dev.py`** that puts `extern/shaped-core` on `sys.path` (and its own root off it), then builds a `cmd.Policy`.
  The preset tables name presets from the project's own `CMakePresets.json`.
- **A `Layout`** in that Policy, naming the project's first-party directories: what clang-format owns, what shaped-linter and the crossref check walk, where libraries live.
  The driver's own files — the pinned clang-format, the emsdk pin — are found in the submodule without any configuration.
- **Its `check` registry**, `Policy.checks`, since which gates run is project policy.
  `check.standard_registry` runs the standard gates, `STANDARD_CHECKS` in [check.py](../../tools/dev/cmd/check.py): shaped-lint, house-style, format, crossrefs, issues, abspaths and test.
  A project with gates of its own passes a function of the same signature instead, placing them among the standard ones; `check.shaped_core_registry` is the worked case.
  A gate build passes `server=ctx.build_server(gate=True)`, so the gate goes through the build server whenever the project's builds do.
- **The house-style gate** holds the project's copies of `.clang-format` and `.editorconfig` to the submodule's, so a bump that changes the style fails until `check --fix` copies them again.
- A **`BuildServer`** in that Policy, which `dev.build_server_from_dev_toml(ROOT)` makes: on wherever ccs is installed, unless the checkout's `dev.toml` says otherwise.
  `speculative_tests=True` or `ship_proof=True` passed to it turns that switch on for every checkout, and a checkout's `dev.toml` can still turn it off.
  [Building and testing](building-and-testing.md#the-build-server) is what it changes.
- **Its own prerequisites**, `Policy.prerequisites`: a `dev.Prerequisite` each, for a dependency the project installs before configuring, as shaped-core fetches DXC and SDL3.
  `ensure(root, preset_name)` runs before every stale preset configures, after shaped-core's own and never concurrently, and must report a failure rather than raise.
  `fingerprint(root)` is a short string over what the install is made from, mixed into the configure fingerprint, so a change to it reconfigures and so ensures again.
  It runs on every fingerprint check, so a stat rather than a hash; a path read from the checkout's `dev.toml` is the usual input.
- **A `CMakePresets.json` that includes ours**, `"include": ["extern/shaped-core/CMakePresets.json"]`, and adds only the presets of its own.
  The launcher paths in ours are `${fileDir}`-relative, so they find the submodule's tools wherever it is included from, and the build directories stay the project's.
  dev.py follows the include to resolve preset names.

## Keeping the submodules current

`uv run dev.py submodules sync` fast-forwards each submodule to the commit the superproject records for it, and no further.
So the working tree matches what HEAD records: `git status` stays clean, and `check` tests what a push would carry.
**It never touches local work**, so it is safe to run at any time, after every pull above all:

| The submodule is | sync |
|---|---|
| declared but not checked out, as after a pull that adds it | initializes it at the recorded commit |
| behind the recorded commit, on its tracked branch and clean | fast-forwards the branch to it |
| behind the recorded commit, detached, as `git submodule update` leaves it | checks the tracked branch out and fast-forwards it, or detaches at the recorded commit when the branch is past it |
| at the recorded commit | leaves it |
| ahead of the recorded commit: local work, or a `--tip` | leaves it and says so; only a bump moves the pointer |
| diverged from the recorded commit | refuses |
| carrying uncommitted changes to tracked files | refuses |
| recording a commit its remote lacks, as when a bump went out before the submodule's push | refuses |
| on another branch | skips it |
| a submodule of a submodule, not checked out | initializes it at the commit its parent records, at any depth |
| a submodule of a submodule, checked out | leaves it alone |

Untracked files do not count: a fast-forward never touches one, and git refuses rather than overwrite it.
Nothing is ever reset, rebased or merged, and a refusal exits non-zero, naming what is in the way.

**`--tip` goes to the head of the tracked branch instead**, the `branch =` the `.gitmodules` entry names.
That is for starting work in a submodule, or for taking in what others pushed there before a bump.
It refuses unpushed commits and skips a submodule without a `branch =`; otherwise it moves as the table says.
A submodule past its pointer shows as modified in the superproject until a bump commits it.
`--bump` implies `--tip` and commits each moved pointer in a commit of its own; a project's own rules on when to bump still apply.
`--dry-run` reports what would move, and `sync <path>` limits it to some submodules.

Set `update = merge` on each `.gitmodules` entry as well.
Then a bare `git submodule update --remote` merges into the checked-out branch instead of detaching HEAD, so even the raw git command strands no commit.

## What does not carry over yet

- `lint clang-tidy` runs `tools/lint/clang-tidy.py`, which roots itself at shaped-core and reads shaped-core's gate list.
- `deps`, `install` and `test-web` expect shaped-core's own files at the project root.
- `.shaped-lint.yml` configs merge along a file's own ancestors, so a consumer copies the root policy rather than inheriting it.
