# shaped-core

Shaped Core is a collection of foundational C++ libraries developed by Shaped Code. They power
Solidean, internal tools, customer projects, and experimental research efforts.

The libraries are C++23, built with CMake, and share a single build & test driver
([dev.py](dev.py)). The set is **growing** — the table below is current, not exhaustive.

## What's inside

Libraries live under `libs/<category>/<lib>`:

| Library                       | What it is                                                                                  |
|-------------------------------|---------------------------------------------------------------------------------------------|
| [clean-core](libs/base/clean-core/) | Foundational C++ data structures, memory utilities, assertions, and low-level primitives (`span`, `vector`, `string`, `optional`, `result`, …). Namespace `cc`. |
| [nexus](libs/base/nexus/)      | Lightweight C++23 test framework, Catch2 v3 CLI–compatible (discovery, filtering, sections, JUnit XML). Namespace `nx`. |

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

[![Windows Clang](https://github.com/solidean/shaped-core/actions/workflows/ci-windows-clang.yml/badge.svg)](https://github.com/solidean/shaped-core/actions/workflows/ci-windows-clang.yml)
[![Linux Clang](https://github.com/solidean/shaped-core/actions/workflows/ci-linux-clang.yml/badge.svg)](https://github.com/solidean/shaped-core/actions/workflows/ci-linux-clang.yml)

All targets are **64-bit**; no 32-bit support is planned. (WebAssembly's `wasm32` has a 32-bit
*address space* but is a 64-bit *register* target — it counts as part of the 64-bit family.)

Support tiers:

* **Tier 1** — actively tested, every change. The bar for "it works".
* **Tier 2** — explicitly supported, occasionally tested. Expected to work; breakage is a bug.
* **Tier 3** — planned. Not wired up yet and may not build.

| Platform                        | Arch        | Compiler / toolchain         | Tier | Notes                                              |
|---------------------------------|-------------|------------------------------|------|----------------------------------------------------|
| Windows                         | x64         | Clang (`clang-cl`), MSVC     | 1    | Default `relwithdebinfo-clang`; `*-msvc-*` for MSVC |
| Linux                           | x64         | Clang, GCC 13+               | 1    | Default `relwithdebinfo-linux-clang`               |
| macOS                           | arm64       | Homebrew LLVM                | 2    | `macos-arm-llvm-*` presets                         |
| iOS                             | arm64       | Apple Clang                  | 2    | First-party; no preset/CI yet                      |
| Android                         | arm64       | NDK r27c (Clang)             | 2    | `android-ndk-arm64-*` presets                      |
| WebAssembly — Emscripten        | wasm32      | Emscripten (Clang)           | 2    | Single-threaded, no WebGPU. `emscripten-*` presets; runs under Node |
| WebAssembly — Emscripten + threads | wasm32   | Emscripten (Clang)           | 3    | `-pthread`; planned                                |
| WebAssembly — Emscripten + WebGPU | wasm32    | Emscripten (Clang)           | 3    | emdawnwebgpu; planned                              |
| WebAssembly — WASI              | wasm32      | wasi-sdk (Clang)             | 3    | Planned                                            |
| Consoles                        | —           | vendor toolchains            | 3    | Planned                                            |

All tiers build the standard **Debug / RelWithDebInfo / Release** types (RelWithDebInfo and Debug
have `CC_ASSERT` on; Release off). Clang platforms additionally have sanitizer and coverage presets.

## Build presets

Presets exist per platform × compiler × build type. A sensible default is chosen for your platform;
override with `--preset` (after the subcommand). List them with:

```bash
uv run dev.py list-presets
```

### WebAssembly (Emscripten)

WASM builds need the [emsdk](https://github.com/emscripten-core/emsdk) (it bundles `emcc`, the CMake
toolchain file, and its own Node.js). Point `dev.py` at a checkout — no permanent activation needed:

```bash
uv run dev.py test --preset emscripten-relwithdebinfo --emsdk-path /path/to/emsdk
```

`dev.py` applies the emsdk environment itself and runs the test binaries under Node. It also accepts
the `SC_EMSDK_PATH` env var, or an already-activated `EMSDK`. `uv run dev.py doctor` validates the
toolchain. See [docs/requirements.md](docs/requirements.md) for details.

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
