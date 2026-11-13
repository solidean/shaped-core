# SGL beside the alternatives: the full matrix

How SGL compares with the other ways to write one set of shaders for DX12, Vulkan, WebGPU and Metal.
Every row is judged for a renderer that targets all four, WebGPU in a browser included, and wants hardware ray tracing wherever a platform has it.

**As of 2026-11-02**; SGL's E3 and E4 as of 2026-11-03, its A7, B2, B3, B6, D3, E4, F7 and G5 as of 2026-11-05.
Its A5, A8, B4, B8, B9, B10, B11, B12, D5, D6, D7, E1, F2, F4 as of 2026-11-06, and its B6, D8, F3, F8 and G6 as of 2026-11-08.
Its B5 and B10 as of 2026-11-09.
Its E4 as of 2026-11-10, run on a Mac.
Its D6 as of 2026-11-11, its D6 and D9 as of 2026-11-12, and its D7 as of 2026-11-13.
Every cell's marker was judged again on 2026-11-05, when ✅ came to mean full support to the extent the targets allow.
Where a stack lets a program choose defined behaviour on every target, as SGL's robustness profiles do, its semantics rows are ✅ though the default leaves a case undefined.
The other columns move fast, Slang's Metal and WGSL support above all, so each cell carries a source and the date it was checked; see [Sources](#sources).

## Columns

| Column | What it means |
|---|---|
| **HLSL + chain** | HLSL through DXC to DXIL and SPIR-V; to Metal through Apple's Metal Shader Converter (MSC) from DXIL, or SPIRV-Cross from SPIR-V; to WGSL through Tint or Naga from SPIR-V. |
| **Slang** | Slang v2026.20 (Khronos), emitting HLSL for DXC, SPIR-V directly, MSL and WGSL. |
| **WGSL** | WGSL as the source language, translated by Naga (wgpu 30) or Tint (Dawn) to HLSL, SPIR-V and MSL; WESL where it changes an answer. |
| **GLSL + chain** | GLSL through glslang to SPIR-V, then SPIRV-Cross to HLSL and MSL, and Tint or Naga to WGSL. |
| **SGL** | SGL at shaped-core `main`, with slib and sg for the host side. |

## Markers

| Marker | Meaning |
|---|---|
| ✅ | Full support, to the extent the targets allow: every target that can deliver it gets it, and where one cannot, the stack says so before run time. |
| ⚠️ | Partly: a target that could deliver it does not get it from this stack, or a caveat the cell names. |
| ❌ | No. |
| — | In a per-target cell: the target itself cannot deliver it, so no stack owes it there. |
| ➕ | In a per-target cell: delivered beyond what the target has, as an emulation. |
| ❓ | Unsure: not verified, or documented but never shown to run. |
| 🗺️ | SGL only: on the roadmap, linked to the issue or incubator note that tracks it. |

A target's own limit is never held against a stack, so a row whose best possible answer leaves a target out can still be ✅.
A target can deliver a capability when its API or hardware has it, or when a lowering of modest cost reaches it, as a float atomic add through a compare-exchange loop.
A per-target cell reads `dx12 / vulkan / metal / webgpu`.
"No issue yet" in an SGL cell marks a gap nothing tracks so far.

## A. Targets and ray tracing

| Row | HLSL + chain | Slang | WGSL | GLSL + chain | SGL |
|---|---|---|---|---|---|
| **A1** DX12 path | ✅ DXC → DXIL, one hop | ✅ HLSL text → DXC, in process | ⚠️ HLSL text → DXC, two hops | ⚠️ SPIR-V → SPIRV-Cross HLSL → DXC, three hops | ✅ DXIL written directly, by default; an entry point holding a target's own text, and a context that asks for it, take HLSL text → DXC |
| **A2** Vulkan path | ✅ DXC → SPIR-V (a community-maintained backend) | ✅ SPIR-V directly | ✅ SPIR-V directly | ✅ glslang → SPIR-V | ✅ SPIR-V written directly, by default; an entry point holding a target's own text, and a context that asks for it, take HLSL text → DXC → SPIR-V |
| **A3** Metal path | ⚠️ two hops: DXIL → MSC, or SPIR-V → SPIRV-Cross → MSL | ✅ MSL text; metallib through Apple's tool | ✅ MSL text | ✅ SPIRV-Cross → MSL | ✅ MSL text; metallib, or the driver compiles the MSL |
| **A4** WebGPU path | ⚠️ SPIR-V → Tint or Naga; drops bindless, int64 and ray queries | ✅ WGSL text; ray tracing, mesh, geometry and tessellation, which WebGPU lacks, are capability errors | ✅ the browser takes it as is; wgpu-only extensions do not reach a browser | ⚠️ SPIR-V → Tint or Naga, a subset | ✅ WGSL text directly; what WebGPU lacks is refused by feature; ray queries emulated beyond it |
| **A5** Inline ray queries | ✅ / ✅ / ✅ / — | ✅ / ✅ / ⚠️ / — metal: one fixed query shape, first run on hardware 2026-10 | ⚠️ / ⚠️ / ⚠️ / — a wgpu-only experimental extension; a WGSL proposal was accepted 2026-10-27 | ✅ / ✅ / ✅ / — | ✅ / ✅ / ✅ / ➕ metal: every inline-trace GPU test passes on an M4; not yet run on an M-series GPU without hardware ray tracing 🗺️ [chore](../issues/chore-sgl-metal-raytracing-on-apple-hardware.md); webgpu emulated in software, as watertight as a target's float allows |
| **A6** Ray-tracing pipelines | ✅ / ✅ / ⚠️ / — metal through MSC alone, emulated as a compute kernel over function tables | ✅ / ✅ / ❌ / — metal at design stage | ❌ / ⚠️ / ❌ / — Naga to SPIR-V only, experimental | ❌ / ✅ / ❌ / — SPIRV-Cross has no ray-tracing stages | ✅ / ✅ / ⚠️ / — metal: four gaps to DXR 🗺️ [epic](../issues/epic-sgl-metal-raytracing-dxr-parity.md); webgpu: a polyfill in the 🗺️ [incubator](../spec/incubator/raytracing-futures.md) |
| **A7** Ray-tracing host side across DXR and Metal | ⚠️ MSC's runtime header helps; the engine still writes both host paths | ❌ the engine writes the shader table and Metal's function tables | ❌ | ❌ | ✅ pipeline description and shader tables generated for dx12, vulkan and metal; the metal side passes sg's, sr's and sv's pipeline tests on an M4 |
| **A8** Ray-tracing depth | ⚠️ full on DX12 and Vulkan; MSC fuses any hit into intersection, no payload qualifiers | ⚠️ full on DX12 and Vulkan; motion blur NVIDIA-only; Metal ray queries only | ⚠️ no callables, no intersection stage | ⚠️ full on Vulkan; elsewhere ray queries only | ⚠️ callables, intersection, any hit, ray flags; moving instances and vertices through the emulated trace, on webgpu and the interpreter, and on dx12, vulkan and metal where a package opts in, inline traces only; natively on metal, a pipeline's trace; no recursive trace graph 🗺️ [incubator](../spec/incubator/raytracing-futures.md) |

## B. Portability and semantics

| Row | HLSL + chain | Slang | WGSL | GLSL + chain | SGL |
|---|---|---|---|---|---|
| **B1** Portable by default | ❌ each hop accepts or rejects on its own, late | ⚠️ capabilities reject missing features statically; behaviour is implementation-defined | ⚠️ designed portable; Naga and Tint disagree on uniformity | ❌ | ✅ one check judges every target; a non-portable form needs a feature (floats: B3) |
| **B2** Integer semantics | ❌ per target | ⚠️ unsigned wrap defined; the rest unspecified | ✅ fully defined, polyfilled on every target | ❌ division overflow, ÷0 and wide shifts undefined | ✅ wrap, truncating `/`, signed `%`, masked shifts defined; ÷0 and `INT_MIN / -1` undefined by default, a constant one an error, and defined everywhere under the `define` profile, or reported or ended under the others (B10, B11) |
| **B3** Float semantics | ⚠️ per target; Metal fast math by default; DXC writes a float `!=` as an ordered compare in SPIR-V, so `NaN != NaN` is false on Vulkan | ⚠️ implementation-defined; `precise` has no effect on Metal | ⚠️ loose by spec; Metal relaxed by default in Dawn and wgpu | ❌ per target | ✅ the exact meaning is IEEE f32, which the interpreter computes; on a GPU an entry point's `@float` level licenses named departures, `fast` by default, up to `ieee` wherever the target keeps it (B12); `!=` is true for a NaN on every target |
| **B4** Out-of-bounds access | ⚠️ per target and robustness setting | ⚠️ undefined by spec | ⚠️ bounded outcomes, where the translator is configured for them | ❌ undefined unless robustness is on | ✅ undefined by default; the interpreter reports it as a program error, and a profile guards an array index, a binding array's, a buffer index and a `bytes` access by `values.length`, an element of a binding array's included, defining it under `define`, where memory that holds nothing reads zero and drops a store; on all four targets, metal's buffers by the lengths sg passes in (B10, B11) |
| **B5** Uniformity, derivatives, discard | ⚠️ no uniformity check; a failure surfaces at Tint or in the browser | ⚠️ no model of WGSL uniformity; open bugs | ⚠️ Tint enforces it; Naga skips derivative checks | ⚠️ no uniformity analysis | ✅ its own uniformity pass for every target; a discarded pixel stays a helper on every target, metal measured on an M4 |
| **B6** Float and 64-bit atomics | ⚠️ 64-bit on DX12 and Vulkan; reported by whichever hop fails | ⚠️ neither on Metal or WGSL; a static capability error | ⚠️ core has 32-bit only; native-only extensions | ⚠️ SPIRV-Cross refuses 64-bit atomics for MSL | ✅ `atomic[float]` on all four, native on vulkan and metal and a compare-exchange loop on dx12 and webgpu, which have no float add; `atomic[long]` and `atomic[ulong]` in buffers and workgroup memory on dx12 and vulkan, each where the device has them; metal's `ulong` min and max too, run on an M4 |
| **B7** Subgroup operations | ⚠️ MSC requires wave size 32; WGSL needs its extension | ⚠️ docs and code disagree for WGSL; wave size left to the user | ✅ `subgroups` extension, shipped in Chrome 134 | ✅ to Wave and SIMD-group ops | ✅ behind `subgroups` on all four; code must be correct at any size |
| **B8** Vendor features (SER, OMM) | ⚠️ DX12 only (SM 6.9); DXC has no SPIR-V lowering; none on Metal or WebGPU; not reported statically | ⚠️ SER on DX12 and Vulkan, checked statically; no OMM | ❌ | ⚠️ extensions reach Vulkan; other targets fail at SPIRV-Cross | ✅ opacity micromaps traced on dx12 and vulkan with no change to the shader, sg building them, and `ray_flags.force_omm_2_state` on both; SER on both, `maybe_reorder`, a reordering trace and a first-class hit object, checked statically and drawn on vulkan and on dx12 under the Agility SDK; vulkan's written as SPIR-V's own instructions, since DXC lowers none; metal and webgpu have neither |
| **B9** Inline target code | ⚠️ inline SPIR-V, Vulkan only | ✅ `__target_switch`, `__intrinsic_asm`, `spirv_asm` | ⚠️ whole-module passthrough in wgpu only | ❌ | ✅ a function body in HLSL, WGSL or MSL text under `const if` on target, feature and option, with placeholders, a file preamble per language, text types and a declared contract (`@pure`, `@barrier`, `@subgroup`, `@derivatives`); dx12's `maybe_reorder` is written this way; a body takes a buffer as it takes a texture, and DXC and Metal name the `.sgl` line of an error inside its text |
| **B10** Finding UB in development | ⚠️ the API validation layers catch out-of-bounds memory, per API and against its objects; nothing checks an integer division | ⚠️ the same API layers; none of its own | ❌ defined rather than reported: a division by zero has a value, an access is clamped, and nothing says so | ⚠️ the same API layers | ✅ the `report` profile logs a division by zero or an array index past its array at its `.sgl` line, with its operands, on dx12, vulkan, webgpu and metal, and so do `pow`, `asin`, `acos`, `atan2`, `smoothstep` and `clamp` with no value; so does a binding array's own index, an unbounded one's by the length sg writes, and a buffer index or a `bytes` access past the bound memory, an element of a binding array's included; so does a read of workgroup memory no invocation stored, in a compute, task or mesh stage, named with its indices |
| **B11** Containing UB in production | ⚠️ per API: vulkan's robust buffer access, dx12's zero for an out-of-range descriptor; integer division per target | ⚠️ the same | ✅ every access bounded and every integer operation defined, polyfilled on every target | ⚠️ vulkan's robustness alone | ✅ the `abort` profile ends the invocation, with no debug window; where the end waits for a barrier, the invocation's writes until then are skipped; the `define` profile gives each such operation WGSL's value instead and goes on, and zeroes a compute stage's workgroup memory as WGSL does, where memory too short for an access reads zero and drops a store; on dx12, vulkan, webgpu and metal, metal's buffers bounded by the lengths sg passes in |
| **B12** Float conformance and fast modes | ⚠️ DXC's `-Gis` per compile and `precise` per variable, `-denorm` from SM 6.2; the later hops carry neither, and nothing refuses a target that drops them | ⚠️ `-fp-mode precise` or `fast` per compile, and denormal modes for SPIR-V and DXIL; nothing documented for Metal or WGSL ❓ | ❌ one mode, whose implementations may assume no NaN or infinity | ⚠️ `precise` per variable, `NoContraction` in SPIR-V; nothing keeps NaNs or infinities | ✅ a ladder from `fast` to `ieee`, stated per function and kept per entry point; dx12 keeps every level, metal up to `precise`, since its safe math flushes denormals, vulkan every level where the device has its float controls; each target compiles an entry point at `fast` with its fast math and a stricter one strictly, metal by a math-mode pragma in the text; webgpu, which promises none, refuses above `fast` unless the host weakens it; `x != x` at `fast` warns |

## C. Host integration and reflection

| Row | HLSL + chain | Slang | WGSL | GLSL + chain | SGL |
|---|---|---|---|---|---|
| **C1** Binding reflection | ✅ three formats, one per target | ✅ rich API and JSON | ✅ Naga's module, Tint's inspector | ✅ SPIRV-Cross reflection | ✅ `sgl describe`, from the source, one format for every target |
| **C2** Usage per entry point: used, read, write | ⚠️ which bindings an entry uses; no read or write bit | ⚠️ a used bit per entry; read or write from the type alone | ✅ Naga: read, write and atomic use per entry (Tint: ❓) | ⚠️ active variables; read or write from decorations | ✅ the footprint: per slot, after inlining and dead-branch removal, pinnable in a test |
| **C3** Generated host code | ❌ | ❌ | ❌ community bindgens only | ❌ | ✅ typed C++ for groups, constants, vertex inputs, targets and pipelines |
| **C4** Binding model across APIs | ⚠️ the engine maps it; MSC lays out an argument buffer from a root signature | ⚠️ parameter blocks map to sets, argument buffers and bind groups; the engine builds the API objects | ⚠️ binding maps; the engine designs the layout | ⚠️ SPIRV-Cross remaps | ✅ each target's final addresses are written by the emitter, by sg's binding conventions |

## D. Language

| Row | HLSL + chain | Slang | WGSL | GLSL + chain | SGL |
|---|---|---|---|---|---|
| **D1** Generics | ✅ templates (HLSL 2021), no constraints | ✅ interfaces and checked generics | ❌ (WESL: draft) | ❌ | ✅ templates checked per instance: type and value parameters, generic structs; no interfaces, by design |
| **D2** Modules | ❌ `#include` | ✅ modules, precompiled modules, link-time specialization | ⚠️ WESL imports | ⚠️ `#include` extension | ✅ modules with dotted names, each a checked layer of its library's chain, checked once per process and precompiled into an artifact a host may keep and a later process load; an edit checks its module and moves the layers after it where its interface stands; a module's options specialize its layer and those after it alone |
| **D3** Bindless | ✅ / ✅ / ✅ (MSC) / — | ✅ / ✅ / ⚠️ / ⚠️ | ⚠️ / ⚠️ / ⚠️ / — native extension only | ✅ / ✅ / ⚠️ / — | ✅ / ✅ / ⚠️ / — fixed-size and unbounded binding arrays, checked `nonuniform`, an unbounded one sized per group, on metal through a table its group sizes |
| **D4** GPU pointers | ⚠️ Vulkan only | ⚠️ SPIR-V; Metal partly | ❌ | ⚠️ Vulkan, and Metal through SPIRV-Cross | ❌ whether to have them is an open design question 🗺️ [epic](../issues/epic-sgl-gpu-pointers.md) |
| **D5** half, f64, int64, int16 | ⚠️ f64 on DX12 and Vulkan, which alone have it, but DXC narrows a double handed to `sqrt`, `floor`, `lerp` or `sin` to a float without a word; 64- and 16-bit ints everywhere but WebGPU | ✅ the same shape | ⚠️ f16 extension; no f64 or int64 in a browser | ✅ the same shape as HLSL | ✅ half, 16- and 64-bit ints and double behind features, wherever the target has them: all of them on dx12 and vulkan, all but double on metal, half alone on webgpu; a 64-bit value crosses a stage link on every target with its type, as its words where DXIL and MSL carry none; a double's sqrt, rounding and vector geometry on dx12 and vulkan, written once in SGL's prelude since DXC narrows a double handed to them, and lowered from it by the text and both native routes alike; `fma` of halves, floats and doubles, rounded once on the interpreter and for a double on DXIL; a double's bits as a `uint2`; no transcendental of a double, which no target has |
| **D6** Mesh, geometry, tessellation | ⚠️ mesh on DX12, Vulkan and MSC | ⚠️ mesh on DX12, Vulkan and Metal; geometry and tessellation on DX12 and Vulkan | ⚠️ Naga experimental mesh; none in a browser | ⚠️ mesh to HLSL and MSL | ⚠️ geometry and tessellation on dx12 and vulkan; mesh and task stages with a payload, per-primitive attributes, triangles or lines and an indirect draw on dx12, vulkan and metal, and on webgpu polyfilled by default, a compute pass and a draw in the native primitive order, the mesh stage at +17% to +41% over the native one where both run; no geometry or tessellation on metal, which Metal 4 dropped, or on webgpu, until their polyfills land 🗺️ [epic](../issues/epic-sgl-every-stage-everywhere.md) |
| **D7** Autodiff | ❌ | ✅ forward and backward | ❌ | ❌ | ⚠️ a prototype outside the normative spec: `fwd_diff` and `bwd_diff` over floats, vectors, matrices and geometry types, loops of constant or stated bounds (`@max_iterations`) over a tape, `tangent[T]` for structs, derivatives written by hand, each mode held to central differences on the interpreter and to each other over random programs; a buffer of floats, vectors or structs differentiable by its `@gradient` and `@tangent`, its atomics shared through the subgroup where `subgroups` is declared, and a texture_2d's load and level-0 sample through a static sampler 🗺️ [epic](../issues/epic-sgl-autodiff.md) |
| **D8** Methods, operators, enums, inference | ⚠️ methods, operators, enums, `auto`; no constructors | ✅ | ⚠️ no methods, operators or enums | ❌ | ✅ methods, properties, `@operator`, enums with computed case values, type aliases, inference, arrow lambdas |
| **D9** Work lists: a shader picks each draw's or dispatch's pipeline | ❓ D3D12 work lists are announced and not shipped; a shader writes records as plain data, and the engine writes the program table | ❓ as HLSL | — / — / — / — WebGPU switches no pipeline from the GPU | ❓ vulkan's device-generated commands switch a pipeline per sequence; the records are plain data the engine lays out | ❌ not started: a program table the host code generator emits, typed records and a check of the pipelines a record may name, native on vulkan and metal and polyfilled elsewhere 🗺️ [feat](../issues/feat-sgl-work-lists.md) |

**D9 is apart from D6: work lists are new and niche, and mesh, task, geometry and tessellation are stages every renderer may meet.**
None of D9's cells was read from a primary source yet, which is why the others are ❓.

**Work graphs are not a row, and SGL will not add them: D3D12 has deprecated them.**
They are reported as supported in SM 6.8 and 6.9 and not in SM 6.10, for lack of adoption, and their only Vulkan counterpart is AMD's provisional `VK_AMDX_shader_enqueue`.
Work lists, which D3D12 named as the direction after them, are row D9.

## E. Testing and debugging

| Row | HLSL + chain | Slang | WGSL | GLSL + chain | SGL |
|---|---|---|---|---|---|
| **E1** Shader unit tests without a GPU | ❌ | ✅ CPU target and an interpreter, documented as incomplete | ❌ (WESL tests run on a GPU) | ❌ | ✅ `test` blocks run on the spec's interpreter: buffers, textures, images and samplers, whose texels and state a test states itself or a driver binds, and subgroup operations and derivatives over lanes, at every subgroup size a target has |
| **E2** Diagnostics | ⚠️ later hops report against IR, once per hop | ⚠️ once against the source; downstream errors per target | ✅ once against the source | ⚠️ SPIRV-Cross errors per target, against SPIR-V | ✅ once against the source, for every target |
| **E3** Debugging the output | ⚠️ PIX at source level on DXIL; mapping lost after a hop | ⚠️ debug info and line directives | ⚠️ readable output, no source stepping | ⚠️ readable output, mapping lost | ✅ readable text with SGL's names; `#line` in HLSL and MSL, so DXC, Metal and PIX name the `.sgl` line; webgpu logs Tint's messages at the `.sgl` line and its inlined calls; SGL's own SPIR-V and DXIL carry the line of each operation when asked, DXIL with its inlined calls |
| **E4** printf and assert on a GPU | ⚠️ printf on Vulkan only | ⚠️ printf except on WGSL; no assert | ⚠️ Naga printf on Metal and Vulkan | ⚠️ printf on Vulkan only | ✅ `print` and `assert` with interpolated values on dx12, vulkan and webgpu, logged once the epoch completes; a failed assert ends its invocation, after any barrier still ahead, and fails a test; a vertex stage's on vulkan where the device lets it store, and never on webgpu, which has no storage there; on metal in every raster and compute stage, and in a ray-tracing pipeline's stages through what its raygen hands them |
| **E5** Normative specification | ⚠️ an Ecma working draft; DXC is the de facto spec | ⚠️ a spec, self-described incomplete | ✅ W3C Candidate Recommendation | ✅ GLSL 4.60 | ✅ normative, with stable rule ids; every example in it is a test |

## F. Workflow and production

| Row | HLSL + chain | Slang | WGSL | GLSL + chain | SGL |
|---|---|---|---|---|---|
| **F1** Hot reload, native | ✅ libraries in process | ✅ in-process API | ✅ microseconds to translate | ✅ libraries in process | ✅ survives a broken edit |
| **F2** Compile in the browser | ❌ no official DXC for wasm (a third-party build is ~17 MB); no MSC | ✅ official slang-wasm, 25 MB (10 MB zipped) | ✅ the browser takes WGSL; Naga is 1.6 MB as wasm | ⚠️ glslang ~0.9 MB plus a 1.7 MB translator | ✅ the compiler runs in the page, and edits live there: sgl-cube's page rebuilds its pipeline as its shader is typed or saved on disk, a broken edit keeping the last that built and showing its error; the whole `sgl` tool, language server included, is 5.6 MB as wasm (1.7 MB gzip, 1.2 MB brotli) |
| **F3** Compile in a shipped product | ⚠️ DXC is tens of MB; MSC's redistribution terms are unclear | ⚠️ 26.5 MB, plus DXC for DXIL | ⚠️ Naga is small; DX12 still needs DXC | ⚠️ a heavy chain | ✅ the compiler depends on clean-core alone, and no target needs a toolchain for what SGL writes: SPIR-V and DXIL are SGL's own by default, every stage, ray tracing and `double` maths among them, and MSL and WGSL are text the driver compiles; a body or a type a program writes in a target's own text is that target's compiler's (B9) |
| **F4** Compile speed | ❓ | ❓ | ✅ Naga: 20–60 µs per small shader and backend | ❓ | ✅ a corpus of bgfx shaders compiles as fast as with Naga; a compile's fixed cost is ~10 µs and the corpus's smallest shader takes 16 µs, against Naga's ~18 µs; the prelude's ~20 ms once per process remain 🗺️ [epic](../issues/epic-sgl-separate-compilation.md) |
| **F5** Permutations | ⚠️ macros; specialization constants on SPIR-V only | ✅ macros, specialization constants, link-time specialization | ⚠️ `override` constants; WESL `@if` | ✅ macros and specialization constants | ✅ options; a false branch leaves the text and the footprint; no specialization constants |
| **F6** Shipping and caching | ✅ offline bundles | ⚠️ precompiled modules; determinism undocumented | ⚠️ no bundle format | ⚠️ SPIR-V as the cached form | ✅ frozen bundles, a persistent cache, deterministic text |
| **F7** Editor tooling | ⚠️ third-party language servers | ⚠️ `slangd`; no formatter | ⚠️ wgsl-analyzer | ⚠️ community language servers | ✅ an in-tree language server: diagnostics, tokens, inlay hints, hover with every overload, go-to-definition, completion, signature help, references, rename, formatting, an outline, folding, workspace symbols and test runs; a formatter, `sgl format`, that proves its output parses the same |
| **F8** Toolchain hosts | ⚠️ MSC on macOS and Windows only | ⚠️ text anywhere; metallib needs Apple's tool | ✅ | ✅ | ✅ every target's code is written in process on any host, wasm included: DXIL and SPIR-V by SGL itself, MSL and WGSL as text; a metallib ahead of time needs Apple's toolchain on macOS, and a body in HLSL text needs DXC |
| **F9** Licensing | ⚠️ MSC is proprietary; whether an app may ship it is unanswered | ✅ Apache-2.0 with LLVM exception | ✅ MIT, Apache-2.0, BSD-3 | ✅ permissive | ✅ MIT |

## G. Ecosystem and risk

| Row | HLSL + chain | Slang | WGSL | GLSL + chain | SGL |
|---|---|---|---|---|---|
| **G1** Governance | ⚠️ only as strong as its weakest link: DXC's SPIR-V backend is community-contributed, SPIRV-Cross mostly one maintainer, MSC a closed Apple binary | ✅ Khronos, driven by NVIDIA | ✅ W3C | ⚠️ Khronos; SPIRV-Cross mostly one maintainer | ⚠️ one company |
| **G2** Maturity | ✅ the industry default | ⚠️ mature on DX12 and Vulkan; young on Metal and WGSL | ⚠️ in production in browsers; ray tracing experimental | ✅ mature | ⚠️ young: first commit 2026-09; in use by an in-house path tracer and renderer |
| **G3** Exit cost | ✅ you keep HLSL | ✅ readable output; Slang-only features lost | ✅ a standard with two implementations | ✅ | ⚠️ the emitted HLSL, MSL and WGSL are readable and can be kept; they follow sg's binding model; MIT allows a fork |
| **G4** Migration from HLSL | ✅ none | ✅ accepts most HLSL | ❌ a rewrite | ❌ a rewrite | ⚠️ a rewrite; LLM-assisted ports of hundreds of shaders have gone through |
| **G5** Vendor libraries (FSR, NRD, RTXDI) | ✅ native; NVAPI on DX12 only | ⚠️ compile for DX12 and Vulkan | ❌ | ⚠️ | ⚠️ beside SGL as HLSL packages on dx12 and vulkan, or ported; reachable from SGL through a text body (B9) over a preamble, untried on a whole vendor library |
| **G6** Speed of the emitted code | ⚠️ near native on DXIL and SPIR-V; later hops unbenchmarked | ❓ | ⚠️ bounds checks and polyfills cost | ❓ | ⚠️ SGL's time over the same three kernels written by hand, so lower is faster, on an Arc B580: dx12 0.99–1.00×, vulkan 0.97–0.99×, webgpu 0.81–1.00× on Dawn and 0.52–1.00× on wgpu; its own SPIR-V and DXIL against its text through DXC, over 22 kernels and two path tracers on an Arc B580 and a Radeon iGPU, within 3% but for four named gaps, the largest 1.15 on the B580's dx12, and ahead on the BC encoders; metal unmeasured 🗺️ [chore](../issues/.closed/2026/11/chore-sgl-emitted-code-speed-on-every-target.md) |

## Sources

Short names:

- **DXC**: <https://github.com/microsoft/DirectXShaderCompiler> (releases, `docs/SPIR-V.rst`), v1.9.2609 (2026-09-29)
- **SM69**: <https://devblogs.microsoft.com/directx/shader-model-6-9-retail-and-more/>
- **HLSLSPEC**: <https://microsoft.github.io/hlsl-specs/specs/hlsl.html>, <https://devblogs.microsoft.com/directx/standardizing-hlsl/> (2026-02-10)
- **MSC**: <https://developer.apple.com/metal/shader-converter/>, MSC 4.0 beta, its manual and changelog
- **SXC**: <https://github.com/KhronosGroup/SPIRV-Cross> (`spirv_hlsl.cpp`, `spirv_msl.cpp`), main 2026-10-28
- **TINT**: <https://github.com/google/dawn> (`src/tint/lang/spirv/reader`, `src/dawn/native/metal/ShaderModuleMTL.mm`), main 2026-11
- **NAGA**: <https://github.com/gfx-rs/wgpu> (`naga/`, `CHANGELOG.md`, `wgpu/src/documentation/extensions/`), 30.0.1 (2026-08-21)
- **WGSL**: <https://www.w3.org/TR/WGSL/>, Candidate Recommendation Draft 2026-10-26; ray-query proposal <https://github.com/gpuweb/gpuweb/pull/11056>
- **WESL**: <https://wesl-lang.dev/>
- **GLSL**: <https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html>, 4.60.8
- **SLANG**: <https://github.com/shader-slang/slang> (releases, `source/slang/slang-capabilities.capdef`, `docs/`), v2026.20 (2026-10-17); <https://shader-slang.org/slang/user-guide/>
- **SLANGSPEC**: <https://github.com/shader-slang/spec>, commit of 2026-10-15
- **VK**: <https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_ray_tracing_invocation_reorder.html>
- **GBV**: <https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-d3d12-debug-layer-gpu-based-validation>
- **VVL**: <https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/main/docs/gpu_validation.md>
- **MTLSV**: <https://developer.apple.com/documentation/xcode/validating-your-apps-metal-shader-usage>
- SGL cells cite this repository; paths are relative to `libs/graphics/shaped-graphics-language/` unless they name another library.

Per row and column:

- **A1**
  - HLSL + chain: DXC releases
  - Slang: SLANG `slangc` reference (downstream compilers)
  - WGSL: NAGA `back/hlsl`, TINT `lang/hlsl/writer`
  - GLSL + chain: SXC `spirv_hlsl.cpp`
  - SGL: [architecture.md](../architecture.md) "The emitters"
  - SGL, the native default: [epic-sgl-native-ir-targets](../issues/epic-sgl-native-ir-targets.md), and the full sg, sr and sv suites on it (2026-11-07)
- **A2**
  - HLSL + chain: DXC `docs/SPIR-V.rst`
  - Slang: SLANG `slangc` reference
  - WGSL: NAGA `back/spv`
  - GLSL + chain: glslang
  - SGL: [bindings.md](../spec/bindings.md) "What the compiler carries today"
  - SGL, the native default: [epic-sgl-native-ir-targets](../issues/epic-sgl-native-ir-targets.md), and the full sg, sr and sv suites on it (2026-11-07)
- **A3**
  - HLSL + chain: MSC
  - Slang: SXC
  - WGSL: SLANG Metal target page
  - GLSL + chain: NAGA `back/msl`
  - SGL: slib `docs/structure.md` "The MSL arm"
- **A4**
  - HLSL + chain: TINT SPIR-V reader capability list, NAGA `front/spv`
  - Slang: SLANG WGSL target page
  - WGSL: WGSL, NAGA extension docs
  - GLSL + chain: SXC with TINT or NAGA
  - SGL: [emitting.md](../spec/semantics/emitting.md) EMIT-109, [bindings.md](../spec/bindings.md) "Features"
- **A5**
  - HLSL + chain: MSC manual, SXC `spirv_msl.cpp`
  - Slang: SLANG capdef `rayquery`, issues 13688, 13860, 13408
  - WGSL: NAGA `ray_tracing.rs`, WGSL proposal 11056
  - GLSL + chain: SXC
  - SGL: [raytracing.md](../spec/raytracing.md) "Native and emulated", [chore-sgl-metal-raytracing-on-apple-hardware](../issues/chore-sgl-metal-raytracing-on-apple-hardware.md) (2026-11-05)
  - SGL, the emulated triangle test: [raytracing-polyfill.md](../raytracing-polyfill.md) "Traversal" (2026-11-06)
- **A6**
  - HLSL + chain: MSC manual
  - Slang: SLANG capdef `raytracing`, issues 11296, 11516, 13067
  - WGSL: NAGA CHANGELOG v29 and v30
  - GLSL + chain: SXC (no ray-tracing execution models outside its GLSL backend)
  - SGL: [raytracing.md](../spec/raytracing.md) "Per target"
- **A7**
  - HLSL + chain: MSC manual (`IRShaderIdentifier`, `IRDispatchRaysArgument`)
  - Slang: SLANG issues 11296, 13067
  - WGSL: NAGA
  - GLSL + chain: SXC
  - SGL: slib `docs/raytracing-pipelines.md`, [chore-sgl-metal-raytracing-on-apple-hardware](../issues/chore-sgl-metal-raytracing-on-apple-hardware.md) (2026-11-05)
- **A8**
  - HLSL + chain: MSC manual feature table
  - Slang: SLANG capdef, issues 13408, 13688
  - WGSL: NAGA `ray_tracing.rs`
  - GLSL + chain: Khronos GLSL extension registry
  - SGL: [raytracing.md](../spec/raytracing.md) "Stages", CHK-332, EVAL-130, EVAL-132, EMIT-135, sg `tests/pipeline/software-trace-test.cc` (2026-11-06)
- **B1**
  - HLSL + chain: TINT and NAGA capability lists, DXC
  - Slang: SLANG capabilities guide, SLANGSPEC `basics-behavior.md`
  - WGSL: NAGA `valid/analyzer.rs`
  - GLSL + chain: SXC
  - SGL: [bindings.md](../spec/bindings.md) "Features", EMIT-13
- **B2**
  - HLSL + chain: WGSL §8, SPIR-V spec, HLSLSPEC
  - Slang: SLANGSPEC
  - WGSL: WGSL §8, NAGA `wgsl-operators.metal`
  - GLSL + chain: GLSL §4.1.3, §5.9
  - SGL: [evaluation.md](../spec/semantics/evaluation.md) EVAL-6, EVAL-83, EVAL-84, EVAL-86
- **B3**
  - HLSL + chain: MSC 4.0 changelog, DXC 1.9.2609's SPIR-V for a float `!=` (`OpFOrdNotEqual`), read 2026-11-05
  - Slang: SLANG issue 13689
  - WGSL: WGSL §15.7, TINT `ShaderModuleMTL.mm`
  - GLSL + chain: GLSL
  - SGL: EVAL-6, CHK-435, EMIT-186, sg's metal backend `metal_common.cc` (2026-11-05)
- **B4**
  - HLSL + chain: WGSL §6.5.7, Vulkan robustness
  - Slang: SLANGSPEC
  - WGSL: NAGA `proc/index.rs`, TINT robustness transform
  - GLSL + chain: GLSL §5.11
  - SGL: EVAL-90, EVAL-97, CHK-460, LEGAL-65, LEGAL-66, LEGAL-68, EMIT-195, `tests/legalize/buffer-guard-test.cc` (2026-11-06)
- **B5**
  - HLSL + chain: DXC 1.9.2609 notes, WGSL uniformity
  - Slang: SLANG uniformity guide, issues 6962, 13631, 13640
  - WGSL: NAGA `analyzer.rs`, TINT `demote_to_helper`
  - GLSL + chain: SXC
  - SGL: [checking.md](../spec/semantics/checking.md) CHK-277, CHK-282, CHK-283, sg `sg - a discarded pixel still takes part in its quad's derivatives` on every backend, metal on an M4 (2026-11-09)
- **B6**
  - HLSL + chain: TINT and NAGA capability lists
  - Slang: SLANG capdef `atomic64`, `atomicfloat`
  - WGSL: NAGA CHANGELOG v24
  - GLSL + chain: SXC `spirv_msl.cpp`
  - SGL: CHK-296, [incubator/atomics.md](../spec/incubator/atomics.md), sg `tests/compute/compute-sixty-four-bit-test.cc` (2026-11-05)
  - SGL, the `ulong` min and max on an M4: [feat-sgl-metal-ulong-min-max-atomics-on-hardware](../issues/.closed/2026/11/feat-sgl-metal-ulong-min-max-atomics-on-hardware.md) (2026-11-06)
- **B7**
  - HLSL + chain: MSC manual
  - Slang: SLANG capdef `subgroup_basic`, issue 14095
  - WGSL: WGSL, <https://developer.chrome.com/blog/new-in-webgpu-134>
  - GLSL + chain: SXC
  - SGL: CHK-371, CHK-376 to CHK-380
- **B8**
  - HLSL + chain: SM69, VK, DXC `SpirvEmitter.cpp`
  - Slang: <https://shader-slang.org/slang/shader-execution-reordering.html>, capdef
  - WGSL: WGSL
  - GLSL + chain: SXC
  - SGL: sg `tests/pipeline/ray-query-test.cc` over `tests/shaders/sgl/raytracing.sgl`, [incubator/specialization.md](../spec/incubator/specialization.md)
  - SGL, SER on vulkan: sg `tests/pipeline/raytracing-pipeline-test.cc`, EMIT-225 (2026-11-06)
  - SGL, SER drawn on dx12 under Agility 1.620.1 and on vulkan: sg `tests/pipeline/mixed-route-ray-test.cc`, EMIT-246 (2026-11-07)
- **B9**
  - HLSL + chain: <https://github.com/microsoft/DirectXShaderCompiler/wiki/GL_EXT_spirv_intrinsics-for-SPIR-V-code-gen>
  - Slang: SLANG capabilities guide
  - WGSL: NAGA CHANGELOG v28
  - SGL: CHK-427, CHK-428, CHK-431, CHK-432, `prelude/raytracing.sgl`, [incubator/specialization.md](../spec/incubator/specialization.md) (2026-11-05)
  - SGL, a buffer parameter and `#line` inside the text: CHK-366, EMIT-182 (2026-11-09)
- **B10**
  - HLSL + chain: GBV, VVL, MTLSV
  - Slang: SLANG (no runtime check of its own in the user guide)
  - WGSL: WGSL §8, §6.5.7
  - GLSL + chain: as HLSL + chain
  - SGL: [legalization.md](../spec/semantics/legalization.md) LEGAL-65, LEGAL-66, LEGAL-69, shaped-rendering `tests/debug-output-test.cc` (2026-11-06)
- **B11**
  - HLSL + chain: Vulkan `VK_EXT_robustness2`, D3D12 descriptor out-of-bounds behaviour
  - Slang: SLANGSPEC
  - WGSL: WGSL §8, §6.5.7, TINT robustness transform
  - GLSL + chain: Vulkan `VK_EXT_robustness2`
  - SGL: LEGAL-65, LEGAL-66, LEGAL-67, LEGAL-69, EMIT-229, [epic-sgl-robustness-profiles](../issues/epic-sgl-robustness-profiles.md) (2026-11-06)
- **B12**
  - HLSL + chain: DXC `-help` (`-Gis`, `-denorm`), SXC, TINT
  - Slang: SLANG `docs/command-line-slangc-reference.md` (`-fp-mode`, `-denorm-mode-fp16/32/64`), read 2026-11-05
  - WGSL: WGSL §15.7
  - GLSL + chain: GLSL §4.7 `precise`, glslang
  - SGL: CHK-435, CHK-436, EMIT-185, sg `tests/compute/compute-float-specials-test.cc` (2026-11-05)
- **C1**
  - HLSL + chain: D3D12 shader reflection docs
  - Slang: SLANG reflection guide
  - WGSL: NAGA, TINT inspector
  - GLSL + chain: SXC
  - SGL: `src/shaped-graphics-language/driver/describe.hh`
- **C2**
  - HLSL + chain: <https://learn.microsoft.com/en-us/windows/win32/api/d3dcommon/ne-d3dcommon-d3d_shader_input_flags>
  - Slang: SLANG reflection guide (`isParameterLocationUsed`)
  - WGSL: NAGA `analyzer.rs` (`GlobalUse`)
  - GLSL + chain: SXC
  - SGL: [bindings.md](../spec/bindings.md) "Footprint", CHK-267, CHK-356
- **C3**
  - HLSL + chain: DXC
  - Slang: SLANG
  - SGL: slib `docs/structure.md` "Compiler chains"
- **C4**
  - HLSL + chain: MSC manual, DXC `SPIR-V.rst`
  - Slang: SLANG Metal and WGSL target pages
  - WGSL: NAGA backend options
  - GLSL + chain: SXC
  - SGL: [bindings.md](../spec/bindings.md) "How a group reaches sg", EMIT-89, EMIT-133
- **D1**
  - HLSL + chain: DXC releases
  - Slang: SLANG interfaces and generics guide
  - WGSL: <https://wesl-lang.dev/spec/Generics.html>
  - GLSL + chain: GLSL
  - SGL: CHK-338 to CHK-340
- **D2**
  - HLSL + chain: HLSLSPEC
  - Slang: SLANG modules guide
  - WGSL: WESL
  - GLSL + chain: glslang
  - SGL: CHK-346, [epic-sgl-modules-beyond-a-tracer](../issues/epic-sgl-modules-beyond-a-tracer.md)
- **D3**
  - HLSL + chain: MSC shader-model table, TINT capability list
  - Slang: SLANG `DescriptorHandle`, issue 14041
  - WGSL: NAGA CHANGELOG, TINT `chromium_experimental_resource_table`
  - GLSL + chain: SXC
  - SGL: [bindings.md](../spec/bindings.md) "Binding arrays"
- **D4**
  - HLSL + chain: DXC 1.8.2505 notes
  - Slang: SLANG convenience features, issues 14035, 14141, 14142
  - WGSL: WGSL
  - GLSL + chain: SXC
  - SGL: [bindings.md](../spec/bindings.md) "Bytes"
- **D5**
  - HLSL + chain: TINT capability list, MSC table, DXC 1.9.2602's DXIL and SPIR-V for a double's `sqrt`, read 2026-11-06
  - Slang: SLANG target compatibility
  - WGSL: WGSL, NAGA
  - GLSL + chain: SXC
  - SGL: CHK-381, CHK-382, CHK-450 to CHK-454, CHK-458, EMIT-192, EMIT-196, EMIT-219, sg `tests/compute/compute-sixty-four-bit-test.cc`, `tests/compute/compute-double-maths-test.cc` (2026-11-06)
  - SGL, for `fma`, a double's bits and the native routes: EVAL-146, CHK-517, EMIT-254 to EMIT-256, the same double test (2026-11-07)
- **D6**
  - HLSL + chain: MSC table
  - Slang: SLANG capdef `_meshshading`
  - WGSL: NAGA CHANGELOG v28, v30
  - GLSL + chain: SXC
  - SGL: CHK-301 to CHK-307, sg `docs/issues/feat-sg-mesh-task-stages.md`
  - SGL's polyfill: CHK-524, sg's [mesh-polyfill.md](../../../shaped-graphics/docs/concepts/mesh-polyfill.md), its test and benchmark (2026-11-11)
- **D7**
  - HLSL + chain: DXC
  - Slang: SLANG autodiff guide
  - WGSL: WGSL
  - GLSL + chain: GLSL
  - SGL: [incubator/autodiff.md](../spec/incubator/autodiff.md) "The prototype", `tests/corpus/derivatives/`, [epic-sgl-autodiff](../issues/epic-sgl-autodiff.md) (2026-11-06)
- **D8**
  - HLSL + chain: DXC 1.9.2607 notes
  - Slang: SLANG convenience features
  - WGSL: WGSL
  - GLSL + chain: GLSL
  - SGL: CHK-14, CHK-36, CHK-68, CHK-142 to CHK-151, CHK-416
- **D9**
  - every column: secondary reports of D3D12 work lists and `VK_EXT_device_generated_commands`, not yet a primary source
  - SGL: [feat-sgl-work-lists](../issues/feat-sgl-work-lists.md), [gpu-driven-work.md](../spec/incubator/gpu-driven-work.md) (2026-11-12)
- **E1**
  - HLSL + chain: DXC
  - Slang: SLANG `slangc` targets (`slangvm`, `llvm`, `cpp`)
  - WGSL: WESL
  - SGL: [evaluation.md](../spec/semantics/evaluation.md) EVAL-127, EVAL-128, EVAL-140 to EVAL-145, `tests/corpus/tests/lanes.sgl`, [architecture.md](../architecture.md) "Tests" (2026-11-06)
- **E2**
  - HLSL + chain: TINT, NAGA, MSC error formats
  - Slang: SLANG
  - WGSL: NAGA
  - GLSL + chain: SXC
  - SGL: CHK-402
- **E3**
  - HLSL + chain: DXC 1.9.2602 notes, MSC changelog v2
  - Slang: SLANG `slangc` `-g`, `-line-directive-mode`
  - SGL: EMIT-47, EMIT-181, EMIT-182 (2026-11-03), EMIT-257 (2026-11-07), read back by DXC's own disassembly; no debugger was driven
- **E4**
  - HLSL + chain: DXC `SPIR-V.rst`
  - Slang: SLANG capdef `printf`
  - WGSL: NAGA `debug_printf.rs`
  - GLSL + chain: Khronos registry
  - SGL: CHK-424, EVAL-103, LEGAL-63, LEGAL-64, LEGAL-65, EMIT-183, sg's `vertex_pipeline_stores` (2026-11-05); metal's ray-tracing stages run on an M4 (2026-11-10)
- **E5**
  - HLSL + chain: HLSLSPEC, <https://ecma-international.org/technical-committees/tc57/>
  - Slang: SLANGSPEC
  - WGSL: WGSL
  - GLSL + chain: GLSL
  - SGL: [spec/_index.md](../spec/_index.md)
- **F1**
  - HLSL + chain: DXC, MSC
  - Slang: SLANG compiling guide
  - WGSL: NAGA
  - GLSL + chain: glslang, SXC
  - SGL: slib `docs/structure.md` "Reload"
- **F2**
  - HLSL + chain: <https://kaltinril.github.io/ShadowDusk/architecture/wasm-frontend.html>, MSC
  - Slang: SLANG release `slang-2026.20-wasm.zip`, measured
  - WGSL: NAGA 30.0.1 built for wasm32, measured
  - GLSL + chain: <https://unpkg.com/@webgpu/glslang@0.0.15/>, Babylon twgsl
  - SGL: `examples/graphics/sgl-cube` and its page's `?selftest` in headless Edge, slib `docs/structure.md` "Reload", the `sgl` tool built with `emscripten-release`, measured (2026-11-06)
- **F3**
  - HLSL + chain: MSC changelog 3.1, <https://developer.apple.com/forums/thread/829379>
  - Slang: SLANG Windows release, measured
  - WGSL: NAGA
  - SGL: [architecture.md](../architecture.md) "Dependencies", slib `docs/frozen-shaders.md`
  - SGL, the default route: slib's `cheat-sheet.md`, and what stays text in [feat-sgl-native-compute-gaps](../issues/feat-sgl-native-compute-gaps.md) (2026-11-08)
- **F4**
  - Slang: SLANG issue 13565
  - WGSL: NAGA measured on the bgfx shaders
  - SGL: shader-rosetta `tools/sgl-benchmark.py`, `sgl bench`, [epic-sgl-prelude-check-cost](../issues/epic-sgl-prelude-check-cost.md) (2026-11-06, Ryzen 9 9950X3D2)
- **F5**
  - HLSL + chain: DXC `SPIR-V.rst`, MSC changelog v3
  - Slang: SLANG link-time specialization guide
  - WGSL: WGSL, WESL
  - GLSL + chain: SXC
  - SGL: CHK-355, CHK-356
- **F6**
  - HLSL + chain: DXC, MSC
  - Slang: SLANG modules guide
  - SGL: slib `docs/frozen-shaders.md`, EMIT-8
- **F7**
  - Slang: SLANG release (`slangd`)
  - WGSL: WESL
  - SGL: [lsp.md](../lsp.md), `src/shaped-graphics-language/driver/format.hh` (2026-11-05)
- **F8**
  - HLSL + chain: MSC, <https://vulkan.lunarg.com/doc/view/latest/mac/getting_started.html>
  - Slang: SLANG `slangc` reference
  - SGL: root `CMakeLists.txt`, slib `docs/issues/.closed/2026/11/feat-slib-hlsl-to-metal-route.md`
  - SGL, the bypass digest SGL's DXIL loads under: [epic-sgl-native-ir-targets](../issues/epic-sgl-native-ir-targets.md) (2026-11-08)
- **F9**
  - HLSL + chain: DXC, SXC, TINT, NAGA licenses, MSC agreements
  - Slang: SLANG `LICENSES/`
  - WGSL: NAGA, TINT, WESL
  - GLSL + chain: glslang, SXC
  - SGL: repository `LICENSE`
- **G1**
  - HLSL + chain: DXC, HLSLSPEC, MSC
  - Slang: <https://www.khronos.org/news/press/khronos-group-launches-slang-initiative-hosting-open-source-compiler-contributed-by-nvidia>
  - WGSL: WGSL, WESL
  - GLSL + chain: SXC
  - SGL: repository `LICENSE`
- **G2**
  - HLSL + chain: DXC releases
  - Slang: SLANG releases and issue tracker
  - WGSL: NAGA CHANGELOG
  - GLSL + chain: SXC
  - SGL: the git history, shader-rosetta
- **G3**
  - Slang: SLANG `slangc` targets
  - WGSL: WGSL
  - SGL: [emitting.md](../spec/semantics/emitting.md), [bindings.md](../spec/bindings.md) "How a group reaches sg"
- **G4**
  - Slang: SLANG `agent-skills/slang-port-hlsl` in the release
  - WGSL: NAGA (no HLSL frontend)
  - SGL: shader-rosetta, [question-sgl-bgfx-wave3-friction](../issues/question-sgl-bgfx-wave3-friction.md)
- **G5**
  - HLSL + chain: vendor repositories
  - Slang: SLANG (inferred from G4, A5, A6)
  - SGL: shaped-rendering `shaders/fsr3/`, slib `docs/issues/.closed/2026/11/feat-slib-hlsl-to-metal-route.md`, CHK-427, CHK-428 (2026-11-05)
- **G6**
  - HLSL + chain: MSC manual performance notes
  - Slang: SLANG issues 11774, 13220, 13245, 14016
  - WGSL: NAGA `BoundsCheckPolicy`
  - SGL: [readme.md](../../readme.md) "Performance", shaped-rendering `tests/bench/sgl-against-hlsl-benchmark.cc`, `tests/bench/sgl-against-wgsl-benchmark.cc` (2026-11-06, Arc B580)
  - SGL, its own binaries against its text through DXC: [native-parity.md](../native-parity.md), shaped-rendering `tests/bench/sgl-routes-benchmark.cc` (2026-11-08, Arc B580 and Radeon iGPU)
  - SGL, what the text owes the target's compiler: EMIT-230, EMIT-231
  - SGL, the gap on an RTX: [perf-sgl-svgf-filter-two-percent](../issues/perf-sgl-svgf-filter-two-percent.md)
