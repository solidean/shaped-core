# Platform support

What shaped-core targets: which platforms are actively tested, which are supported, and which are
planned. This doc is the **support model** — what *should* work. For the *toolchain requirements*
(compiler / CMake versions) see [requirements.md](requirements.md); for *what CI actually builds and
tests* see [guides/ci.md](guides/ci.md). (Back to [_index.md](_index.md).)

## 64-bit only

All targets are **64-bit**; no 32-bit support is planned. (WebAssembly's `wasm32` has a 32-bit
*address space* but is a 64-bit *register* target — it counts as part of the 64-bit family.)

That split has teeth: 64-bit *registers* are assumed everywhere, but 64-bit *pointers* are not, so
anything sized off a pointer is smaller on wasm32 (`cc::small_vector` is 48 B on x64/arm64, less on
wasm32). Code that pins a byte count branches on clean-core's **`CC_HAS_64BIT_POINTERS`** (0 or 1,
from [macros.hh](../libs/base/clean-core/src/clean-core/common/macros.hh)) — never on the arch, and
never on a hand-rolled `sizeof(void*) == 8`.

## Support tiers

- **Tier 1** — built **and tested in CI** on every push and pull request. The bar for "it works".
  Each Tier-1 platform is its own CI workflow; see [guides/ci.md](guides/ci.md).
- **Tier 2** — explicitly supported, expected to work; breakage is a bug. Some Tier-2 platforms
  have **build-only CI**: they are *cross-compiled* on every push/PR (catching portability
  breakage early), but the runner can't execute the produced binaries, so the test suite is not
  run there. The rest are occasionally tested by hand and not wired into CI.
- **Tier 3** — planned. Not wired up yet and may not build.

## Platforms

| Platform | Arch | Compiler / toolchain | Tier | Status / notes |
|----------|------|----------------------|------|----------------|
| Windows | x64 | Clang (`clang-cl`) | 1 | CI |
| Windows | x64 | MSVC `cl` — VS 2022 (toolset 14.44) | 1 | CI |
| Windows | x64 | MSVC `cl` — VS 2026 (toolset 14.51) | 1 | CI |
| Windows | arm64 | MSVC `cl` — VS 2022 (toolset 14.44) | 1 | CI — `windows-11-arm`, native arm64 |
| Linux | x64 | Clang | 1 | CI — the deep Debug / RelWithDebInfo / Release matrix |
| Linux | x64 | GCC 14 (13+) | 1 | CI |
| Linux | arm64 | Clang | 1 | CI — `ubuntu-26.04-arm`, native arm64 |
| macOS | arm64 | Homebrew LLVM (clang) | 1 | CI |
| WebAssembly | wasm32 | Emscripten (Clang) | 1 | CI — single-threaded, no WebGPU; runs under Node |
| iOS | arm64 | Apple Clang | 2 | CI — build-only (cross-compiled, not test-run) |
| Android | arm64 | NDK (Clang) | 2 | CI — build-only; `android-ndk-arm64-*` presets (NDK from `$ANDROID_NDK_ROOT`) |
| WebAssembly + threads | wasm32 | Emscripten (Clang) | 3 | `-pthread`; planned |
| WebAssembly + WebGPU | wasm32 | Emscripten (Clang) | 3 | emdawnwebgpu; planned |
| WebAssembly — WASI | wasm32 | wasi-sdk (Clang) | 3 | planned |
| Consoles | — | vendor toolchains | 3 | planned |

The non-WASM Tier-3 WebAssembly variants (`-pthread`, WebGPU, WASI) have configure knobs already —
`SC_THREADS` / `SC_WASM_WEBGPU` / `SC_WASM_EXCEPTIONS` — but they fail configure today with a
clear "not yet supported" message rather than building. See
[requirements.md](requirements.md#emscripten--wasm).

## Threading (`SC_THREADS`)

`SC_THREADS` (default `ON`) is the repo-wide threading knob; it reaches C++ as clean-core's
`CC_HAS_THREADS` (0 or 1). `OFF` builds without OS threads — what WASM is today, and what the
`singlethreaded-*` presets reproduce **on a native host**, so that mode is debuggable with the normal
toolchain instead of only under Node.

`ON` is an assertion rather than a preference: a platform that cannot honor it fails configure (wasm
today) rather than quietly demoting, so the flag never describes a build it didn't get.

No API appears or disappears with it. Threaded types fall back to running on the calling thread —
`cc::threaded_actor` runs on whoever pumps it, and sg drains its copy actors before any wait (see
[shaped-graphics threading](../libs/graphics/shaped-graphics/docs/concepts/threading.md)). It does change
struct layout (node_allocation's slab header), so it is a whole-build switch, never per-target.

`uv run dev.py check` runs a RelWithDebInfo single-threaded preset alongside the others, so both
threading modes stay exercised at precommit.

## Build types

The standard **Debug / RelWithDebInfo / Release** build types should all work on every supported
platform — RelWithDebInfo and Debug have `CC_ASSERT` **on**, Release **off**. In CI, only **Linux
clang** exercises Debug and Release (the full matrix); every other Tier-1 platform is built and
tested at **RelWithDebInfo** only. Clang platforms additionally carry sanitizer and coverage presets
(see [guides/building-and-testing.md](guides/building-and-testing.md#sanitizers) and
[guides/coverage.md](guides/coverage.md)).
