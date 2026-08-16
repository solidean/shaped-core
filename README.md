# shaped-core

Shaped Core is a collection of foundational C++ libraries developed by Shaped Code.
They power Solidean, internal tools, customer projects, and experimental research efforts.

The libraries are C++23, built with CMake, and share a single build & test driver ([dev.py](dev.py)).
Both the **set of libraries and each library's scope are still growing**, so the descriptions below state what each library is *for* — its intended scope — and therefore what belongs where.
Not all of it is implemented yet.

## What's inside

Libraries live under `libs/<category>/<lib>`:

| Category | Library | Namespace | What it is |
|----------|---------|-----------|------------|
| base | [clean-core](libs/base/clean-core/) | `cc` | Foundational C++ data structures, memory utilities, assertions, and low-level primitives (`span`, `vector`, `string`, `optional`, `result`, …). No dependencies. |
| base | [nexus](libs/base/nexus/) | `nx` | Lightweight C++23 test framework, Catch2 v3 CLI–compatible (discovery, filtering, sections, JUnit XML), so IDE test integration works out of the box. Every `<lib>-test` binary is built on it. |
| base | [typed-geometry](libs/base/typed-geometry/) | `tg` | The repo's strongly-typed math & geometry vocabulary, where the type system encodes the geometry. Intended home for linear algebra (`vec`/`pos`/`comp`/`mat`/`quat`), transforms, geometric primitives & queries (distance, intersection, containment), curves, color, sampling, spatial acceleration structures, symbolic/exact math, and meshes. |
| io | [babel-serializer](libs/io/babel-serializer/) | `babel` | Serialization & deserialization of various formats. Each format parses into an unopinionated native structure, with opinionated aggregators ("load an image", "load a mesh") on top. Base64, JSON, markdown, Wavefront OBJ, glTF 2.0/GLB, SQLite, and PNG/JPEG images today. |
| data | [versioned-document](libs/data/versioned-document/) | `vdoc` | Structured documents that are versioned, mergeable and verifiable: entities → components → properties, materialized from an immutable content-addressed DAG of ops. Ships zero components — the component set belongs to the application. Design stage. |
| data | [versioned-document-file](libs/data/versioned-document-file/) | `vdoc::file` | The `.vdoc` save format: one SQLite file holding a document's op DAG, its refs and snapshots, its embedded assets over deduplicated blobs, and a disposable workspace. Design stage. |
| data | [blob-cache](libs/data/blob-cache/) | `bcache` | A persistent, multi-process cache for expensive derived bytes: `(namespace, key, version)` → content-addressed blobs in one shared SQLite file, with in-process singleflight, TTLs and cost-aware eviction. Deleting it can never affect correctness. |
| graphics | [shaped-graphics](libs/graphics/shaped-graphics/) | `sg` | The graphics-API wrapper: a backend-agnostic `context`, `command_list`, and GPU resource types over per-backend static libraries (dx12/vulkan tier 1, metal/webgpu tier 2, opengl/webgl legacy). Also home to the render-routine framework. |
| graphics | [shaped-shader-compiler-dxc](libs/graphics/shaped-shader-compiler-dxc/) | `ssc::dxc` | A lean wrapper over the DirectX Shader Compiler: HLSL → `sg::compiled_shader` (bytecode + reflection), plus an async content-keyed cache. Windows-only, built only once DXC has been fetched. |
| graphics | [shaped-shader-library](libs/graphics/shaped-shader-library/) | `slib` | Shader packages + hot reloading. Any target declares its shaders via `sc_add_shader_package` and gets typed C++ symbols; `acquire(ctx)` returns bytecode in a format that context accepts. |
| graphics | [shaped-rendering](libs/graphics/shaped-rendering/) | `sr` | Concrete render routines on top of sg (mipmap generation, tonemapping, texture compression, …), the Dear ImGui renderer, and the SDL3-backed window abstraction (`sr::window_system` / `sr::window`). |
| graphics | [shaped-viewer](libs/graphics/shaped-viewer/) | `sv` | The professional visualization library: a modern, RTX-enabled renderer with a dev-friendly API. The top of the graphics stack. |

The graphics stack is early-stage, and the libraries in it are at very different depths.
[docs/libraries.md](docs/libraries.md) is the full catalog, including what each library is *for* beyond what exists today.

## Quick start

Prerequisites: a C++23 toolchain (Clang or MSVC first-class, GCC supported), CMake >= 3.28, and [`uv`](https://docs.astral.sh/uv/), which runs the Python driver with no venv setup needed.

```bash
uv run dev.py doctor   # sanity-check the toolchain
uv run dev.py build    # configure + build (default preset for your platform)
uv run dev.py test     # build + run the full test suite
```

Run a subset while iterating:

```bash
uv run dev.py test "<pattern>"   # auto-build + run just the matching test(s)
```

`dev.py` auto-configures and auto-builds as needed, and is quiet by default — it captures logs under `build/<preset>/` and prints a terse summary.
[docs/guides/building-and-testing.md](docs/guides/building-and-testing.md) is the full reference.

## Platform support

shaped-core targets **64-bit** platforms only.
Every platform we build **and test in CI** is **Tier 1**, with one CI workflow and badge per platform below.
iOS and Android additionally have **build-only** CI — compiled every push and PR, not test-run — and their own badges, listed separately.
[docs/platforms.md](docs/platforms.md) has the full support model, including the tier definitions and the Tier-3 platforms; [docs/guides/ci.md](docs/guides/ci.md) has what each job actually runs.

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

**Debug / RelWithDebInfo / Release** should all work everywhere, but only Linux clang exercises the full matrix in CI.
The other Tier-1 platforms are built and tested at RelWithDebInfo only.

**Build-only CI** (Tier 2): iOS and Android are cross-compiled on every push and PR but not test-run, since the runners cannot execute the produced binaries.
So those two badges report a clean *compile*, not a passing test suite.

| CI workflow | Arch | Compiler / toolchain | Config |
|-------------|------|----------------------|--------|
| [![iOS Clang (build-only)](https://github.com/solidean/shaped-core/actions/workflows/ci-ios-clang.yml/badge.svg)](https://github.com/solidean/shaped-core/actions/workflows/ci-ios-clang.yml) | arm64 | Apple Clang | RelWithDebInfo |
| [![Android NDK (build-only)](https://github.com/solidean/shaped-core/actions/workflows/ci-android-ndk.yml/badge.svg)](https://github.com/solidean/shaped-core/actions/workflows/ci-android-ndk.yml) | arm64 | NDK (Clang) | RelWithDebInfo |

## Build presets

Presets exist per platform × compiler × build type, and a sensible default is chosen for your platform.
Override it with `--preset`, which goes *after* the subcommand, and list them with `uv run dev.py list-presets`.
[docs/guides/building-and-testing.md](docs/guides/building-and-testing.md#presets) covers the rest, including toolset pinning and the emsdk path for WebAssembly builds.

## Layout

```
libs/<category>/<lib>   # the libraries (src/<lib>/, tests/, optional docs/)
tools/                  # dev/ (build & test modules), cmake/, bin/ (checked-in binaries),
                        # lint/ (clang-tidy gates), shaped-linter/, instruction-tracer/
docs/                   # repo-wide docs — start at docs/_index.md
dev.py                  # build & test driver
CMakeLists.txt          # top-level build
CMakePresets.json       # platform/compiler presets
```

[docs/_index.md](docs/_index.md) is the documentation hub, and [CLAUDE.md](CLAUDE.md) carries the working conventions of this repo.

## Contributing

* `main` is the integration branch.
  Feature branches are **mandatory** and namespaced per contributor by initials: `u/<your-initials>/<feature>`.
* Code style is enforced by [.clang-format](.clang-format); design conventions live in [docs/coding-guidelines.md](docs/coding-guidelines.md).

## License

MIT — see [LICENSE](LICENSE).
