# Platform support

What shaped-core targets: which platforms are actively tested, which are supported, and which are planned.
This doc is the **support model** — what *should* work.
For the toolchain requirements see [requirements.md](requirements.md); for what CI actually builds and tests see [guides/ci.md](guides/ci.md).
Back to [_index.md](_index.md).

## 64-bit only

All targets are **64-bit**; no 32-bit support is planned.
WebAssembly's `wasm32` has a 32-bit *address space* but is a 64-bit *register* target, so it counts as part of the 64-bit family.

That split has teeth: 64-bit *registers* are assumed everywhere, but 64-bit *pointers* are not.
Anything sized off a pointer is smaller on wasm32 — `cc::small_vector` is 48 B on x64 and arm64, less on wasm32.
Code that pins a byte count branches on clean-core's **`CC_HAS_64BIT_POINTERS`**, 0 or 1, from [macros.hh](../libs/base/clean-core/src/clean-core/common/macros.hh).
Never on the arch, and never on a hand-rolled `sizeof(void*) == 8`.

## Support tiers

- **Tier 1** — built **and tested in CI** on every push and pull request; the bar for "it works".
  Each Tier-1 platform is its own CI workflow; see [guides/ci.md](guides/ci.md).
- **Tier 2** — explicitly supported and expected to work, so breakage is a bug.
  Some Tier-2 platforms have **build-only CI**: they are *cross-compiled* on every push and PR, which catches portability breakage early.
  The runner cannot execute the produced binaries, so the test suite is not run there.
  The rest are occasionally tested by hand and not wired into CI.
- **Tier 3** — planned, not wired up yet, and may not build.

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
| SteamOS | x64 | Clang | 2 | No CI — built and run by hand, semi-regularly; see [SteamOS](#steamos) below |
| WebAssembly + threads | wasm32 | Emscripten (Clang) | 3 | `-pthread`; planned |
| WebAssembly + WebGPU | wasm32 | Emscripten (Clang) | 3 | emdawnwebgpu; planned |
| WebAssembly — WASI | wasm32 | wasi-sdk (Clang) | 3 | planned |
| Consoles | — | vendor toolchains | 3 | planned |

The Tier-3 WebAssembly variants have configure knobs already: `SC_THREADS`, `SC_WASM_WEBGPU` and `SC_WASM_EXCEPTIONS`.
They fail configure today with a clear "not yet supported" message rather than building — [requirements.md](requirements.md#emscripten--wasm) owns those knobs.

### SteamOS

SteamOS is generic Linux x64 to the build, and Tier 2 rather than Tier 1 only because no CI runner runs it.
It is worth naming separately anyway, because its immutable base image ships a runtime where a plain Linux assumption does not hold.

- **`std::stacktrace` does not link**, so clean-core configures the empty stub and `CC_HAS_STACKTRACE` is 0.
  The image carries `libstdc++.so` but not the `libstdc++exp` that implements `<stacktrace>`, and the LLVM toolchain's libc++ ships no `<stacktrace>` header at all.
  An assert therefore reports "stacktrace unavailable on this platform" instead of frames.
- **No dx12, so vulkan is the only backend the cross-backend sg API tests run against here** — see shaped-graphics' `_sg_test_drivers`.
  Its driver is registered but disabled while the backend is built out, so a test runs against vulkan by being named exactly rather than by a sweep.
  A Vulkan SDK being present is what makes SteamOS the first platform to separate "a backend builds" from "a backend can be driven".
  It is also the first platform where the API suite runs against something other than dx12.
- **The base image ships the windowing runtime but not its development headers**, so `sr::window` and a windowed Vulkan swapchain both compile themselves out.
  `libX11.so` and `libwayland-client.so` are there; `X11/Xlib.h` and `wayland-client.h` are not.
  Both the SDL3 fetch in [extern/sdl3](../extern/sdl3/CMakeLists.txt) and the surface probe in the vulkan backend gate on the headers.
  What survives is everything headless: the suite passes, and `dev.py example <match> --capture` renders and writes its image.
  What is lost is a real window, which is why an example run without `--capture` reports no window backend and draws nothing.
  `uv run dev.py doctor` names it — its `vulkan surface` and `sr::window (SDL3)` lines.
  The fix is to put those headers where the compiler already looks, which on an immutable image usually means a sysroot rather than `/usr/include`.
- **The hardware-counter budget is smaller than the PMU's counter count**, because the NMI watchdog holds a PMC.
  `nx::bench` discovers the usable width rather than assuming it, so this costs extra measurement passes and nothing else.

None of this is SteamOS-specific in principle — any Linux without `libstdc++exp`, or with a watchdog on a PMC, behaves the same way.

## Frame pointers (`SC_FRAME_POINTERS`)

`SC_FRAME_POINTERS` (default `ON`) keeps a frame pointer in every frame, so `cc::capture_stack` can walk a stack by chasing the chain.
That is a few nanoseconds a frame, against roughly a microsecond for the table-driven unwind that is the only alternative.

It reaches **clang and GCC only**, as `-fno-omit-frame-pointer` (or `/Oy-` under clang-cl).
MSVC cannot maintain a frame pointer on x64 at all — `/Oy` is x86-only — which is why Windows captures with `RtlCaptureStackBackTrace` and does not care about this knob.
So the capture cost is genuinely asymmetric across tier 1, and stacktrace policy should account for it rather than assume one number.

**Turning it `OFF` costs clang and GCC the capture itself, not merely its speed.**
There is no table-driven fallback off Windows, so the walk chases a chain that is no longer maintained, fails its validation and reports `broken` after a frame or two.
Windows is unaffected either way, since it never used the chain.
So `OFF` means "this build does not capture stacks on clang or GCC" — which is a real choice for a shipping target that wants the register back, and never a free one.
Expect roughly 0.5-2% and one register on x86-64 for keeping them, and next to nothing on arm64, where macOS mandates the chain anyway.

## Threading (`SC_THREADS`)

`SC_THREADS` (default `ON`) is the repo-wide threading knob; it reaches C++ as clean-core's `CC_HAS_THREADS`, 0 or 1.
`OFF` builds without OS threads — what WASM is today, and what the `singlethreaded-*` presets reproduce **on a native host**.
That is what makes the mode debuggable with the normal toolchain instead of only under Node.

`ON` is an assertion rather than a preference: a platform that cannot honor it fails configure, as wasm does today, rather than quietly demoting.
So the flag never describes a build it did not get.

No API appears or disappears with it.
Threaded types fall back to running on the calling thread: `cc::threaded_actor` runs on whoever pumps it, and sg drains its copy actors before any wait.
See [shaped-graphics threading](../libs/graphics/shaped-graphics/docs/concepts/threading.md) for what that costs a caller.
It does change struct layout — node_allocation's slab header — so it is a whole-build switch, never per-target.

`uv run dev.py check` runs a RelWithDebInfo single-threaded preset alongside the others, so both threading modes stay exercised at precommit.

## Default allocator (`SC_MIMALLOC`)

`SC_MIMALLOC` (default `ON`) chooses what backs `cc::default_memory_resource`; it reaches C++ as clean-core's `CC_HAS_MIMALLOC`, 0 or 1.
`OFF` points the default at `cc::system_memory_resource` — the malloc/free resource that is always present as an explicit opt-out — and links no mimalloc at all.

No API and no struct layout changes with it, since both are `cc::memory_resource` implementations behind the same pointer.
What changes is whether a tool can see our allocations.
Sanitizers, Valgrind and heap profilers intercept malloc and operator new, and none of them intercepts `mi_malloc`.
So under mimalloc LeakSanitizer never scans memory the default resource handed out, and an object owned only by a `cc::` container is reported as a **direct leak** rather than as reachable.

That is why the `sanitize-*` presets set it `OFF`.
The two flags stay independent on purpose, and `SANITIZE` implies nothing about `SC_MIMALLOC`.
Sanitizing a mimalloc build stays available for the times the thing being chased is mimalloc's own behavior.

The one behavioral difference is in-place resize.
mimalloc reports its usable size and can grow a block into that slack, while the system resource always declines and the caller reallocates and copies.
So `try_resize_bytes_in_place` returning -1 is a normal outcome rather than a platform assumption.

## Build types

The standard **Debug / RelWithDebInfo / Release** build types should all work on every supported platform.
RelWithDebInfo and Debug have `CC_ASSERT` **on**, Release **off**.
In CI, only **Linux clang** exercises the full Debug / RelWithDebInfo / Release matrix; every other Tier-1 platform is built and tested at **RelWithDebInfo** only.
Clang platforms additionally carry sanitizer and coverage presets — see [sanitizers](guides/building-and-testing.md#sanitizers) and [coverage.md](guides/coverage.md).

**C++20 module scanning is off repo-wide** (`CMAKE_CXX_SCAN_FOR_MODULES`), because nothing here imports a module.
Left at the default CMP0155 turns on for C++20+, whether a build pays for a per-TU scan comes down to whether a `clang-scan-deps` happens to be installed.
CMake pairs whichever it finds with our compiler.
The Android CI runner had one for its Linux clang jobs, and a scanner cannot read a precompiled header the NDK's clang produced, so every scan failed there.
Turn it back on with the first `import`, and make sure the scanner matches the compiler.

Every build type builds against per-target precompiled headers.
The `nopch-*` and `debug-nopch-*` presets turn them off.
Linux clang's CI matrix carries `nopch-linux-clang`, so a source relying on a header only the PCH supplied cannot land — see [precompiled-headers.md](guides/precompiled-headers.md).
