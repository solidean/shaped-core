# shaped-core

Shaped Core is a collection of foundational C++ libraries developed by Shaped Code. They power
Solidean, internal tools, customer projects, and experimental research efforts.

The libraries are C++23, built with CMake, and share a single build & test driver
([dev.py](dev.py)). Both the **set of libraries and each library's scope are still growing** — the
descriptions below state what each library is *for* (its intended scope), so it's clear what belongs
where; not all of it is implemented yet.

## What's inside

Libraries live under `libs/<category>/<lib>`:

| Library                       | What it is                                                                                  |
|-------------------------------|---------------------------------------------------------------------------------------------|
| [clean-core](libs/base/clean-core/) | Foundational C++ data structures, memory utilities, assertions, and low-level primitives (`span`, `vector`, `string`, `optional`, `result`, …). Namespace `cc`. |
| [nexus](libs/base/nexus/)      | Lightweight C++23 test framework, Catch2 v3 CLI–compatible (discovery, filtering, sections, JUnit XML). Namespace `nx`. |
| [typed-geometry](libs/base/typed-geometry/) | The repo's strongly-typed math & geometry vocabulary, where the type system encodes the geometry. Intended home for linear algebra (`vec`/`pos`/`comp`/`mat`/`quat`), transforms, geometric primitives & queries (distance, intersection, containment), curves, color, sampling, spatial acceleration structures, symbolic/exact math, and meshes. Namespace `tg`. |

See [docs/libraries.md](docs/libraries.md) for the full catalog.

## Quick start

Prerequisites: a C++23 toolchain (Clang or MSVC; GCC second-class), CMake >= 3.28, and
[`uv`](https://docs.astral.sh/uv/) (it runs the Python driver — no venv setup needed).

```bash
uv run dev.py doctor   # sanity-check the toolchain
uv run dev.py build    # configure + build (default preset for your platform)
uv run dev.py test     # build + run the full test suite
```

Run a subset while iterating:

```bash
uv run dev.py test "<pattern>"   # auto-build + run just the matching test(s)
```

`dev.py` auto-configures and auto-builds as needed, and is quiet by default (it captures logs
under `build/<preset>/` and prints a terse summary). See
[docs/guides/building-and-testing.md](docs/guides/building-and-testing.md) for the full reference.

## Platform support

shaped-core targets **64-bit** platforms only. Every platform we build **and test in CI** is
**Tier 1** — one CI workflow (and badge) per platform below. iOS and Android additionally have
**build-only** CI (compiled every push/PR, not test-run) and their own badges, listed separately.
The full support model (tier definitions plus the Tier-3 platforms: the other WebAssembly flavors
and consoles) is in [docs/platforms.md](docs/platforms.md); what each job actually runs is in
[docs/guides/ci.md](docs/guides/ci.md).

| CI workflow | Arch | Compiler / toolchain | Config |
|-------------|------|----------------------|--------|
| [![Windows Clang](https://github.com/solidean/shaped-core/actions/workflows/ci-windows-clang.yml/badge.svg)](https://github.com/solidean/shaped-core/actions/workflows/ci-windows-clang.yml) | x64 | Clang (`clang-cl`) | RelWithDebInfo |
| [![Windows MSVC (VS2022)](https://github.com/solidean/shaped-core/actions/workflows/ci-windows-msvc.yml/badge.svg)](https://github.com/solidean/shaped-core/actions/workflows/ci-windows-msvc.yml) | x64 | MSVC `cl` (toolset 14.44) | RelWithDebInfo |
| [![Windows MSVC (VS2026)](https://github.com/solidean/shaped-core/actions/workflows/ci-windows-msvc-vs2026.yml/badge.svg)](https://github.com/solidean/shaped-core/actions/workflows/ci-windows-msvc-vs2026.yml) | x64 | MSVC `cl` (toolset 14.51) | RelWithDebInfo |
| [![Windows ARM MSVC (VS2022)](https://github.com/solidean/shaped-core/actions/workflows/ci-windows-arm-msvc.yml/badge.svg)](https://github.com/solidean/shaped-core/actions/workflows/ci-windows-arm-msvc.yml) | arm64 | MSVC `cl` (toolset 14.44) | RelWithDebInfo |
| [![Linux Clang](https://github.com/solidean/shaped-core/actions/workflows/ci-linux-clang.yml/badge.svg)](https://github.com/solidean/shaped-core/actions/workflows/ci-linux-clang.yml) | x64 | Clang | Debug / RelWithDebInfo / Release |
| [![Linux GCC](https://github.com/solidean/shaped-core/actions/workflows/ci-linux-gcc.yml/badge.svg)](https://github.com/solidean/shaped-core/actions/workflows/ci-linux-gcc.yml) | x64 | GCC 14 (13+) | RelWithDebInfo |
| [![Linux ARM Clang](https://github.com/solidean/shaped-core/actions/workflows/ci-linux-arm-clang.yml/badge.svg)](https://github.com/solidean/shaped-core/actions/workflows/ci-linux-arm-clang.yml) | arm64 | Clang | RelWithDebInfo |
| [![macOS Clang](https://github.com/solidean/shaped-core/actions/workflows/ci-macos-clang.yml/badge.svg)](https://github.com/solidean/shaped-core/actions/workflows/ci-macos-clang.yml) | arm64 | Homebrew LLVM | RelWithDebInfo |
| [![WASM (Emscripten)](https://github.com/solidean/shaped-core/actions/workflows/ci-wasm-emscripten.yml/badge.svg)](https://github.com/solidean/shaped-core/actions/workflows/ci-wasm-emscripten.yml) | wasm32 | Emscripten (Clang) | RelWithDebInfo |

**Debug / RelWithDebInfo / Release** should all work everywhere, but only Linux clang exercises the
full matrix in CI; the other Tier-1 platforms are built and tested at RelWithDebInfo only. Details in
[docs/platforms.md](docs/platforms.md).

**Build-only CI** (Tier 2): iOS and Android are cross-compiled on every push/PR but not test-run
(the runners can't execute the produced binaries), so these badges report a clean *compile*, not a
passing test suite.

| CI workflow | Arch | Compiler / toolchain | Config |
|-------------|------|----------------------|--------|
| [![iOS Clang (build-only)](https://github.com/solidean/shaped-core/actions/workflows/ci-ios-clang.yml/badge.svg)](https://github.com/solidean/shaped-core/actions/workflows/ci-ios-clang.yml) | arm64 | Apple Clang | RelWithDebInfo |
| [![Android NDK (build-only)](https://github.com/solidean/shaped-core/actions/workflows/ci-android-ndk.yml/badge.svg)](https://github.com/solidean/shaped-core/actions/workflows/ci-android-ndk.yml) | arm64 | NDK (Clang) | RelWithDebInfo |

## Build presets

Presets exist per platform × compiler × build type. A sensible default is chosen for your platform;
override with `--preset` (after the subcommand). List them with:

```bash
uv run dev.py list-presets
```

WebAssembly builds need the [emsdk](https://github.com/emscripten-core/emsdk); point `dev.py` at a
checkout with `--emsdk-path` (no permanent activation needed). See
[docs/requirements.md](docs/requirements.md#emscripten--wasm) for the details.

## Layout

```
libs/<category>/<lib>   # the libraries (src/<lib>/, tests/, optional docs/)
tools/                  # dev/ (build & test modules) and bin/ (checked-in binaries)
docs/                   # repo-wide docs — start at docs/_index.md
dev.py                  # build & test driver
CMakeLists.txt          # top-level build
CMakePresets.json       # platform/compiler presets
```

See [docs/_index.md](docs/_index.md) for documentation and [CLAUDE.md](CLAUDE.md) for the
working conventions of this repo.

## Contributing

* `main` is the integration branch. Feature branches are **mandatory** and namespaced per
  contributor by initials: `u/<your-initials>/<feature>`.
* Code style is enforced by [.clang-format](.clang-format); design conventions live in
  [docs/coding-guidelines.md](docs/coding-guidelines.md).

## License

MIT — see [LICENSE](LICENSE).
