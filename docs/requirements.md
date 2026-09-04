# Requirements

The toolchain shaped-core assumes.
Many of these are *implicit*, encoded in [CMakePresets.json](../CMakePresets.json), the per-library `CMakeLists.txt` and the CMake helper modules rather than stated anywhere.
So they are collected here.
`uv run dev.py doctor` validates the core ones; see the last section.

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

3.28 is the first release with **official C++ named-module support**, on the Ninja and Visual Studio generators.
Keeping it as the floor lets a future module-based library land without bumping the minimum repo-wide.

Nothing in the *current* sources actually needs that much.
The highest feature in use is `target_sources(... FILE_SET TYPE HEADERS)`, which is CMake **3.23**; everything else — the `cxx_std_23` compile feature, presets schema v3 — is satisfied by 3.21.
So 3.28 is a deliberate forward-looking floor, not a present-day necessity.
`dev.py doctor` reads this minimum straight from the top-level `CMakeLists.txt` and checks the installed `cmake` against it, so the two never drift.

## C++ standard

* **C++23**, enforced repo-wide (`CMAKE_CXX_STANDARD 23`, extensions off) and required per-target via the `cxx_std_23` compile feature.
* **64-bit only** — every preset targets x64 / arm64.
* MSVC builds add `/Zc:preprocessor` (conforming preprocessor).

## Compilers

Configured per platform via presets.
"Known-good" means a preset exists and targets it; older versions may work but are untested.

| Platform | Compiler            | Notes                                                        |
|----------|---------------------|-------------------------------------------------------------|
| Windows  | `clang-cl`          | Default (`relwithdebinfo-clang`). The version CI pins is in [guides/ci.md](guides/ci.md#toolchains). |
| Windows  | `cl` (MSVC)         | VS 2022 (toolset 14.44) and VS 2026 (14.51); `*-msvc-*` presets. |
| Linux    | `clang++` / `clang` | Default (`relwithdebinfo-linux-clang`).                      |
| Linux    | `g++` / `gcc`       | `relwithdebinfo-linux-gcc`, the only GCC preset. GCC **13+** for `std::stacktrace`. |
| macOS    | Homebrew LLVM       | Expects `/opt/homebrew/opt/llvm/bin/clang++` (arm64).        |
| Android  | NDK (r27+)          | `android-ndk-arm64-*` presets; NDK located via `$ANDROID_NDK_ROOT`. |
| WASM     | Emscripten (emsdk)  | `emscripten-*` presets (single-threaded). See below.        |

See [platforms.md](platforms.md) for the full tier matrix — which platforms are actively tested, which are supported, and which are planned.

### Emscripten / WASM

WebAssembly builds use the [emsdk](https://github.com/emscripten-core/emsdk), which bundles `emcc`, the CMake toolchain file, and its own Node.js — so no separate Node install is needed.
The `wasm-emscripten-*` configure presets reference the toolchain file via `$env{EMSDK}`.

You do **not** need to activate emsdk permanently or with `--system`: `dev.py` locates it and applies its environment to each configure, build and test subprocess.
Point it at a checkout with `--emsdk-path`:

```bash
uv run dev.py test --preset emscripten-relwithdebinfo --emsdk-path /path/to/emsdk
```

Resolution order is `--emsdk-path` → the `SC_EMSDK_PATH` env var → an already-activated `EMSDK` → `emcc` on `PATH`.
Tests run under Node: `-s NODERAWFS=1` gives the binaries real-filesystem access so the JUnit report is written, and `-s EXIT_RUNTIME=1` propagates the pass/fail exit code.
Only the single-threaded, no-WebGPU, `-fexceptions` combination is wired today.
The `SC_THREADS=ON` / `SC_WASM_WEBGPU` / `SC_WASM_EXCEPTIONS=wasm-exceptions` knobs exist but fail configure with a clear "not yet supported" message — Tier 3 in [platforms.md](platforms.md).

Threads are the repo-wide `SC_THREADS` option rather than a wasm-local knob, and the `wasm-emscripten-*` presets set it `OFF`.
A hand-rolled wasm configure must too: leaving the `ON` default fails rather than silently building single-threaded.
To develop that mode natively, use a `singlethreaded-*` preset instead of a wasm build — the knob itself is [platforms.md](platforms.md#threading-sc_threads)'s.

### WebGPU test runtimes

None of this is needed for the wasm tier as it stands, which has no WebGPU.
These are the runtimes the Tier-3 `SC_WASM_WEBGPU` work will test against, written down here so a machine can be prepared before that lands.

What they buy is that **WebGPU is reachable from the CLI**, so a wasm graphics build need not be driven through a browser to be tested.
They are also two different implementations of the same spec.
Node's binding is Google's Dawn, which is what Chrome ships; Deno's is wgpu, which is what Firefox ships.
Running both from the CLI therefore covers the quirks of both browser engines, which is the class of bug that otherwise appears only after deploying.

**Deno** has WebGPU built in, with no native module to compile.

```bash
winget install DenoLand.Deno                    # Windows
curl -fsSL https://deno.land/install.sh | sh    # Linux / macOS
deno --version
```

WebGPU is on by default as of Deno 2.9.6, which is what this was verified against; `--unstable-webgpu` is still accepted but no longer required.
It needs no permission flag either, which is worth knowing because Deno gates most of its capabilities by default: a plain `deno run` on a WebGPU script works.
Deno can also present to a real OS window through `Deno.UnsafeWindowSurface`, which binds a surface to a window handle an FFI windowing library supplies rather than opening one itself.

**Node** gets WebGPU from the `webgpu` package, which ships prebuilt Dawn bindings and needs no build step.

```bash
npm install webgpu
```

Node is the runtime `dev.py test` already drives for wasm, so it is the shorter path of the two.
A wasm module reaches the binding through `globalThis.navigator.gpu`, which the harness must install before the module loads.

### `std::stacktrace`

clean-core uses `std::stacktrace`, and whether it is usable at all is settled by a **link probe** at configure time.
[DetectStacktraceLib.cmake](../libs/base/clean-core/cmake/DetectStacktraceLib.cmake) is that probe.
It links a probe program rather than asking whether a header exists, which is the only test that distinguishes a declared `std::stacktrace` from an implemented one.
Its verdict picks the link library:

* MSVC / libc++ / newer toolchains — no extra library.
* GCC 14+ libstdc++ — `-lstdc++exp`.
* GCC 13 libstdc++ — `-lstdc++_libbacktrace`.
* A libstdc++ shipped **without** `libstdc++exp` — nothing links, so the stub is configured instead.
  It has `<stacktrace>` and none of its symbols, and SteamOS is where we first hit it; [platforms.md](platforms.md#steamos) has that case.

On the GCC path this makes **GCC 13** the practical floor.
Where `<stacktrace>` is unavailable altogether — notably Emscripten and WASI libc++ — the build does **not** fail either.

The probe also **publishes** its verdict, as clean-core's `PUBLIC` `CC_HAS_STACKTRACE` define, so no consumer can reach a different answer.
[stacktrace.hh](../libs/base/clean-core/src/clean-core/platform/stacktrace.hh) falls back to deciding for itself with `__has_include`, but only when nothing defined it.
That is a build which does not go through our CMake.
Either way an unavailable `<stacktrace>` means an empty `cc::stacktrace` stub (`CC_HAS_STACKTRACE 0`) rather than a compile error.
The configure-time detection downgrades to a status message instead of an error.

### Linkers

On non-MSVC compilers the fastest available linker is auto-selected by [DetectLinker.cmake](../libs/base/clean-core/cmake/DetectLinker.cmake): **mold > lld > system default**.
None are required, and absence just falls back.
MSVC uses its own linker.

## Graphics & windowing

Everything in this section is **optional**, and every piece degrades rather than breaking the build.
That is what makes it worth writing down: a checkout missing all of it configures, builds and passes the whole suite, and the gap only shows up as an example that draws nothing.
`uv run dev.py doctor` reports each line below.

| Need | For | Absent |
|------|-----|--------|
| **Vulkan headers** (`vulkan/vulkan.h`, via `$VULKAN_SDK` or a distro package) | building the vulkan backend at all — `find_package(Vulkan)` gates it | the backend is skipped at configure, and on non-Windows that leaves sg with no backend |
| **Vulkan loader + an ICD** | creating a device at run time | `create_vulkan_context` fails with no graphics device |
| **X11 / XCB / wayland development headers** (Linux) | a *windowed* swapchain, and SDL3 configuring at all | `sr::window_system::try_create` fails and a swapchain reports the platform unsupported; headless present and `--capture` are unaffected |
| **DXC** (`uv run extern/dxc/download-dxc.py`, run by dev.py) | `ssc::dxc`, so a shader package's HLSL becomes DXIL or SPIR-V | every `slib` `acquire` fails, and the examples that draw are gated out of the build |
| **Windows SDK** (`d3d12shader.h`) | DXIL *reflection*, and the dx12 backend | SPIR-V reflection still works, through the vendored SPIRV-Reflect |

The Linux windowing entry is headers-only on purpose: SDL loads `libX11` and `libwayland-client` themselves at run time.
So a machine with the runtime and no headers builds a shaped-rendering with `SR_HAS_WINDOW=0`, and never says why at run time.
[platforms.md](platforms.md#steamos) has the one platform where that combination is the default rather than an oversight.

## Developer / IDE tooling

| Tool             | Minimum | Role                                                                   |
|------------------|---------|------------------------------------------------------------------------|
| **clang-format** | **22**  | Authoritative formatter; the [.clang-format](../.clang-format) config uses v22-only option spellings. |
| **clangd**       | 22 fam. | IDE code intelligence; `dev.py doctor` and `dev.py diagnose clangd` use it. clangd 21 crashes on this codebase. |
| clang-tidy       | —       | Advisory only; still being calibrated. Not gating.                     |

The repo's LLVM-based tooling tracks the **22** family.
Pair `clang-format`, `clangd` and — on the clang path — the compiler from the same major version, to avoid format churn and stale diagnostics.

**clang-format is the one you do not have to install.**
`dev.py format` fetches the pinned build into `tools/bin/` when the one it finds is missing or the wrong major, so a toolchain on a different LLVM major is no longer a dead end.
The binary is gitignored rather than committed, and [tools/bin/fetch-clang-format.py](../tools/bin/fetch-clang-format.py) owns the pin.
Run that directly to install ahead of time, or set `SC_SKIP_CLANG_FORMAT_FETCH=1` to keep it off the network.

### diag-launcher

Builds wrap the compiler and linker with [tools/bin/diag-launcher.exe](../tools/bin/diag-launcher.exe), set as `CMAKE_<LANG>_COMPILER_LAUNCHER` / `..._LINKER_LAUNCHER` in the presets.
It captures per-invocation diagnostics into `.diag.json` sidecars that the `repo_tools` `build_diag` MCP tool reads.
It is checked into the repo, so there is no install step.
The `.exe` is Windows-only, so the POSIX presets wire `diag_launcher.sh` → `diag_launcher.py` instead — see [guides/ci.md](guides/ci.md) for which preset gets which.

## What `dev.py doctor` validates

```bash
uv run dev.py doctor
```

Checks, in order:

- **cmake** — present, *and* at least the declared minimum
- **ninja**
- a usable **compiler** — the MSVC environment or `clang-cl` on Windows, `clang++` / `g++` elsewhere
- that **presets parse**, and that the platform **default preset** exists
- the **coverage** tools, `llvm-cov` and `llvm-profdata`, which coverage and PGO both need
- the **emscripten** toolchain
- the **graphics environment** — Vulkan headers and runtime, windowed-surface support, `sr::window`'s SDL3 backend, and DXC
- **clangd** — found, a published `compile_commands.json` present, and a real file parsed cleanly

A red line names the fix.

The graphics checks are advisory too, and for a stronger reason: nothing there can fail a build.
Every graphics library degrades instead of disappearing, so a checkout missing all of it still configures, builds and passes the whole suite.
The first sign of a gap is an example that draws nothing.
The detail line names what a gap costs, since that is the part no other output states.

They run the header probes through the compiler **the selected preset configures**, not a bare `clang++`, so they see the same include path CMake will.
That is the whole answer on a machine whose headers live in a sysroot rather than in `/usr/include`.

The emscripten check is advisory.
With no emsdk signal — `--emsdk-path`, `SC_EMSDK_PATH`, `EMSDK`, or `emcc` on `PATH` — it reports a passing "not configured (optional)" line, so a native-only setup stays green.
Once any signal is present it validates strictly: emsdk located, `emcc` runnable, toolchain file present, and emsdk's `node` reachable.
