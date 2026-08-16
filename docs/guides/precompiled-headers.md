# Precompiled headers

Every first-party target with more than a couple of translation units builds against a precompiled header, chosen per target.
This removes roughly half of the repo's compile CPU, and [notes/build-times.md](../notes/build-times.md) is the measurement behind that number.

Two CMake functions in [tools/cmake/PrecompiledHeaders.cmake](../../tools/cmake/PrecompiledHeaders.cmake) carry the whole mechanism.
**A library declares the tiers it owns, in its own CMakeLists**, and any target applies them:

```cmake
# libs/graphics/shaped-graphics/CMakeLists.txt
sc_declare_pch_tier(SG EXTENDS TG HEADERS <shaped-graphics/all.hh>)
sc_target_pch(shaped-graphics SG)

# libs/base/clean-core/CMakeLists.txt
sc_target_pch(clean-core-test CC_STD NEXUS)
```

That a tier's headers live next to the target that owns them is the point.
A list of `clean-core/container/*.hh` kept centrally would be a file nobody editing clean-core ever opens, and it would rot within a release.
The one exception is `STD`, which names standard headers and belongs to no library, so it stays in the module.

Applying several tiers composes them left to right, which is how an orthogonal tier joins a rung.
`NEXUS` is a tier rather than a special keyword for exactly that reason.

## The tiers today

| tier | declared in | adds |
|---|---|---|
| `STD` | the module | `<chrono> <memory> <mutex> <string> <string_view> <system_error> <ranges> <atomic> <type_traits> <utility>` |
| `CC` | clean-core | the clean-core surface a typical TU reaches for — containers, strings, `optional`/`result`, the function and memory wrappers |
| `CC_ALL` | clean-core | `CC` + the heavy tail: threading, streams, node allocation |
| `CC_STD` | clean-core | `CC_ALL` + `STD` |
| `NEXUS` | nexus | `<nexus/test.hh>`, orthogonal to the rungs |
| `TG` | typed-geometry | `CC_STD` + `<typed-geometry/all.hh>` |
| `SG` | shaped-graphics | `TG` + `<shaped-graphics/all.hh>` |
| `DX12` | the dx12 backend | `SG` + `<shaped-graphics/backends/dx12/dx12_common.hh>` |
| `DXC` | shaped-shader-compiler-dxc | `SG` + `<shaped-shader-compiler-dxc/impl/dxc_common.hh>` |

Two rules the mechanism enforces or relies on:

- **`EXTENDS` needs its parents already declared.** That makes a cycle impossible to express, and forces the tier graph to follow the library dependency order the root CMakeLists already imposes.
- **`sc_target_pch` is recorded, not applied**, until `sc_finalize_pch()` at the bottom of the root CMakeLists.
  So a target may name a tier from a library added after its own — `clean-core-test` wants `NEXUS`, and nexus has to come after clean-core because it depends on it.
  The repo does the same for shader packages and web runners.

## Picking a tier is a measurement, not a guess

**A bigger PCH is not automatically better, and the failure is quiet.**
`shaped-linter-core` measures 0.42 at `CC` and 0.50 at `CC_STD`; `nexus-test` measures 0.63 at `CC` and 0.84 with the STL block added.
Both are pure clean-core consumers, so the extra content is a PCH they pay to deserialize and never read.

Size itself is not the constraint.
The restore cost runs about 3 ms per MB up to ~28 MB and then flattens — a 48 MB PCH costs no more per TU than a 28 MB one, because clang deserializes lazily and a TU pays for what it references.
So the question is never "how big" but "does this target actually use it".

To answer that for a target:

```bash
uv run dev.py compile-time pch "libs/graphics/shaped-graphics/src/**/*.cc" --repeat 3
```

That compiles each TU with its configured PCH and again without, back to back, and prints the per-target ratio.
It builds the preset first, because the flags name a `.pch` the target's own build produces.
To compare two *tiers*, change the `sc_target_pch` call and measure again — the tiers live in CMake and are deliberately not duplicated in Python.

Growing a library is the other half of this.
When a library's public surface shifts, the tier it declares is part of that surface: a new vocabulary header most TUs reach for belongs in the tier, and one that stopped being used should leave it.
That is why the tier sits in the library's own CMakeLists rather than centrally.

Two traps worth knowing before you trust a number:

- **A ratio of 1.00 everywhere means the preset has PCH disabled**, not that the PCH is worthless.
  Check you are not on a `nopch-*` preset.
- **Runs vary by more than the ~8 % [notes/build-times.md](../notes/build-times.md#runs-vary-by-8) documents** when the machine is busy.
  One measurement pass here came out 3.2x inflated, and read as a plausible result rather than an outlier.
  The `baseline (empty TU)` line is the tell: ~0.03 s on a quiet machine, and much higher means re-measure.

## The SDK gate goes last

`DX12` and `DXC` name a *gate header* — `dx12_common.hh`, `dxc_common.hh` — rather than `<d3d12.h>` or `<dxcapi.h>`, and they sit at the end of the header list.

Both reasons are in [clean-core/platform/win32_sanitized.hh](../../libs/base/clean-core/src/clean-core/platform/win32_sanitized.hh).
`<rpcndr.h>` typedefs `byte` at global scope, which collides with `cc::byte` as an ambiguity at the first bare use.
Each gate brackets its SDK includes in `#define byte win_byte_override` and keeps its own clean-core headers above that bracket, so including a gate whole is safe and leaks no macro.
Naming the SDK headers directly instead skips the bracket and breaks the TUs — measured, 14 of 16 dx12 TUs failed that way.

A C++ header parsed *under* the macro would declare `std::win_byte_override` instead of `std::byte`, and since `cc::byte` **is** `std::byte`, that breaks everything downstream.
Both gates close their own bracket, so nothing after them is actually at risk.
Keep them terminal anyway: nothing `EXTENDS` a gate tier, and a call site applying one names it last (`sc_target_pch(x NEXUS DX12)`).
That is what stops the ordering from becoming load-bearing the day someone adds a header the gate does not close over.

There is deliberately no `VULKAN` tier: `<vulkan/vulkan.h>` reaches an unsanitized `windows.h` under `VK_USE_PLATFORM_WIN32_KHR`, and 6 TUs do not justify that risk.

## Turning it off — and why a no-PCH build is a gate

A PCH reaches every TU through `/FI`, so **a source that drops an include it still uses compiles anyway**.
Nothing else in the repo catches that: `shaped-linter`'s `blessed-includes` checks that an include is *allowed*, not that a use is *covered*.

`CMAKE_DISABLE_PRECOMPILE_HEADERS=ON` — CMake's own switch — makes every `sc_target_pch` a no-op.
There is no `SC_PCH` option, because it would implement nothing but a second name for that switch, and two levers can disagree.

Three places already use it, so the drift cannot land silently:

| where | preset | scope |
|---|---|---|
| `dev.py check`'s debug leg | `debug-nopch-clang` / `debug-nopch-linux-clang` / `macos-arm-llvm-debug-nopch` | every commit, Debug only |
| CI | `nopch-linux-clang`, in the Linux clang matrix | every PR, RelWithDebInfo |
| local repro | `nopch-clang`, `nopch-linux-clang` | on demand |

The debug leg is Debug-only on purpose: it is the cheapest of `check`'s four builds, and a config-specific ordering problem still meets CI's RelWithDebInfo canary first.

`dev.py lint clang-tidy` needs no preset of its own.
It strips the PCH flags into a filtered compilation database, so it parses each TU from source and would surface the missing include as a compile error.

## Costs to know about

- **Disk.** Roughly 28 MB per target per preset, so about 2.8 GB across `check`'s four build dirs.
  `dev.py clean` is the answer if it matters.
- **Rebuild churn is not one of them.** A PCH costs 0.9–2.5 s to build.
  Touching a clean-core header already rebuilds every dependent TU; the PCH rebuild rides along, in parallel, and every rebuilt TU is then ~40 % cheaper.
  Touching a `.cc` leaves the PCH alone entirely.
- **`REUSE_FROM` is not used.** It demands byte-identical flags between donor and reuser and forces one tier across the sharing group, to save 1–2 s of fully-parallel work.
- **Single-TU targets get no PCH** — `shaped-linter`, `instruction-tracer`, `instruction-tracer-fixture`.
  A 1–2 s PCH build to save ~0.4 s on one object is a loss.

## Adding a target, or a tier

Call `sc_target_pch` after that target's `target_link_libraries`, inside whatever `if()` created the target.
An unknown target name is a hard `FATAL_ERROR` rather than a silent skip, because a typo would otherwise cost that target its entire PCH win with no symptom.
An unknown *tier* is caught at `sc_finalize_pch()`, naming the target that asked.

Start at the tier its nearest measured sibling uses, then run `compile-time pch` and adjust.
Targets whose tier is inherited rather than measured say so in the comment on the call.

A new library declares its own tier next to its targets, extending the highest one it depends on:

```cmake
sc_declare_pch_tier(MYLIB EXTENDS SG HEADERS <mylib/all.hh>)
sc_target_pch(mylib MYLIB)
```

Redeclaring an existing tier name is an error, because letting the second declaration win would make the applied content depend on `add_subdirectory` order.
