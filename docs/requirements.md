# Requirements

The toolchain shaped-core assumes. Many of these are *implicit* — encoded in
[CMakePresets.json](../CMakePresets.json), the per-library `CMakeLists.txt`, and
the CMake helper modules rather than stated anywhere — so they are collected here.
`uv run dev.py doctor` validates the core ones (see the last section).

For *how* to build and test, see
[guides/building-and-testing.md](guides/building-and-testing.md).

## Core toolchain

| Tool       | Minimum            | Why                                                                 |
|------------|--------------------|---------------------------------------------------------------------|
| **CMake**  | 3.28               | See below. The floor lives in the top-level [CMakeLists.txt](../CMakeLists.txt). |
| **Ninja**  | any recent         | The only generator configured by every preset.                      |
| **Python** | 3.10+              | [dev.py](../dev.py) and helper scripts (PEP 723 inline deps).        |
| **uv**     | any recent         | How every Python entry point is run (`uv run dev.py ...`).           |
| C++ compiler | C++23           | See [Compilers](#compilers). All targets are 64-bit.                |

### Why CMake 3.28

3.28 is the first release with **official C++ named-module support** (Ninja and
Visual Studio generators). Keeping it as the floor lets a future module-based
library land without bumping the minimum repo-wide.

Nothing in the *current* sources actually needs that much — the highest feature
in use is `target_sources(... FILE_SET TYPE HEADERS)`, which is CMake **3.23**;
everything else (the `cxx_std_23` compile feature, presets schema v3) is
satisfied by 3.21. So 3.28 is a deliberate forward-looking floor, not a
present-day necessity. `dev.py doctor` reads this minimum straight from the
top-level `CMakeLists.txt` and checks the installed `cmake` against it, so the
two never drift.

## C++ standard

* **C++23**, enforced repo-wide (`CMAKE_CXX_STANDARD 23`, extensions off) and
  required per-target via the `cxx_std_23` compile feature.
* **64-bit only** — every preset targets x64 / arm64.
* MSVC builds add `/Zc:preprocessor` (conforming preprocessor).

## Compilers

Configured per platform via presets. "Known-good" means a preset exists and
targets it; older versions may work but are untested.

| Platform | Compiler            | Notes                                                        |
|----------|---------------------|-------------------------------------------------------------|
| Windows  | `clang-cl`          | Default (`relwithdebinfo-clang`). LLVM 21 family — see below. |
| Windows  | `cl` (MSVC)         | VS 2022 toolset; `*-msvc-*` presets.                         |
| Linux    | `clang++` / `clang` | Default (`relwithdebinfo-linux-clang`).                      |
| Linux    | `g++` / `gcc`       | `*-gcc-*` presets. GCC **13+** for `std::stacktrace`.        |
| macOS    | Homebrew LLVM       | Expects `/opt/homebrew/opt/llvm/bin/clang++` (arm64).        |
| Android  | NDK r27c            | Expects `C:/Android/android-ndk-r27c` (see preset).          |

### `std::stacktrace`

clean-core uses `std::stacktrace`, and which link library provides it is
detected at configure time ([DetectStacktraceLib.cmake](../libs/base/clean-core/cmake/DetectStacktraceLib.cmake)):

* MSVC / libc++ / newer toolchains — no extra library.
* GCC 14+ libstdc++ — `-lstdc++exp`.
* GCC 13 libstdc++ — `-lstdc++_libbacktrace`.

If none links, configure fails with a clear `FATAL_ERROR`. This makes **GCC 13**
the practical floor on the GCC path.

### Linkers

On non-MSVC compilers the fastest available linker is auto-selected
([DetectLinker.cmake](../libs/base/clean-core/cmake/DetectLinker.cmake)):
**mold > lld > system default**. None are required; absence just falls back.
MSVC uses its own linker.

## Developer / IDE tooling

| Tool             | Minimum | Role                                                                   |
|------------------|---------|------------------------------------------------------------------------|
| **clang-format** | **21**  | Authoritative formatter; the [.clang-format](../.clang-format) config uses v21-only option spellings. |
| **clangd**       | 21 fam. | IDE code intelligence; `dev.py doctor` and `dev.py diagnose clangd` use it. |
| clang-tidy       | —       | Advisory only; still being calibrated. Not gating.                     |

The repo's LLVM-based tooling tracks the **21** family — pair `clang-format`,
`clangd`, and (on the clang path) the compiler from the same major version to
avoid format churn and stale diagnostics.

### diag-launcher

Builds wrap the compiler and linker with
[tools/bin/diag-launcher.exe](../tools/bin/diag-launcher.exe) (set as
`CMAKE_<LANG>_COMPILER_LAUNCHER` / `..._LINKER_LAUNCHER` in the presets). It
captures per-invocation diagnostics into `.diag.json` sidecars that the
`repo_tools` `build_diag` MCP tool reads. It is checked into the repo, so no
install step — but it is a Windows binary, so the launcher wiring is currently
Windows-specific.

## What `dev.py doctor` validates

```bash
uv run dev.py doctor
```

Checks, in order: **cmake** (present *and* >= the declared minimum), **ninja**,
a usable **compiler** (MSVC env / `clang-cl` on Windows, `clang++`/`g++`
elsewhere), that **presets parse** and the platform **default preset** exists,
and **clangd** (found, a published `compile_commands.json` exists, and it parses
a real file cleanly). A red line names the fix.
