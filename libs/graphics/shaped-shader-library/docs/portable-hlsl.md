# Portable HLSL

The design for a blessed way to write one `.hlsl` that compiles correctly for every backend.

[shaders.md's "Writing HLSL for both backends"](../../shaped-graphics/docs/shaders.md) is the authoring guide; this is the design behind it.
The prelude and the compile flags have landed, and the phasing at the end says what has not.
Every claim below marked "pinned" is asserted by [portable-hlsl-spike-test.cc](../../shaped-shader-compiler-dxc/tests/portable-hlsl-spike-test.cc).
A DXC upgrade that changes one of them fails a test rather than a shader.

Today's targets are dx12 (DXIL) and vulkan (SPIR-V), both through DXC.
Metal and WebGPU are intended, and reuse the same two arms — metal-shaderconverter consumes DXIL, Tint consumes SPIR-V — so a third arm is not automatically a third spelling.

## What DXC actually does

Five questions decided the shape, and the answers were not all what we expected.

**An unguarded `[[vk::…]]` attribute is a hard error on the DXIL target.**
`[[vk::binding]]`, `[[vk::push_constant]]` and `[[vk::location]]` each produce `'<attr>' attribute ignored [-Werror,-Wignored-attributes]`, and ssc compiles with `-WX` by default.
So there is no "write it once, DXC ignores it where it does not apply" — every annotation forks on `__spirv__`.
That is the entire justification for a prelude rather than a style guide: the fork is mechanical and easy to forget.
Its failure mode is a shader that builds on the backend you tested and not on the other.
[pinned]

**`__COUNTER__` works, and survives token pasting through the usual two-level indirection.**
So a binding's index does not have to be written by hand.
[pinned]

**A duplicate file-scope `static const` is a redefinition error, and an unused one is quiet under `-WX`.**
So a marker symbol whose *name* encodes the slot turns a double-booked slot into a compile error.
[pinned]

**DXC assigns DXIL registers per class from zero when the source names none, and a SPIR-V set accepts sparse indices.**
Neither target needs its numbers spelled out.
[pinned]

**`[[vk::binding]]` takes a constant expression, not only an integer literal — and it is still not enough for a `BEGIN`/`END` macro pair.**
A macro cannot emit a preprocessor directive, so a group can only be carried by an HLSL constant that the annotation reads.
That constant needs one fixed name for the binding macro to find, an `END` cannot undeclare it, and a second `BEGIN` in the same file redeclares it.
A macro pair could therefore only ever serve one group per file, which is why the group is an argument on each binding instead.
[pinned]

One consequence worth stating plainly, because it constrains the validation below: compiled to both targets, the same declaration keeps its **name and type** and changes its **index**.
SPIR-V takes the counter's number, DXIL takes DXC's per-class assignment.
That is fine — a binding group resolves a name — but it means an equivalence check compares names and kinds, never addresses.
[pinned]

## Space is not set

A DXIL register space is a namespace for register numbers.
A SPIR-V set is a hardware-visible descriptor set that the bind slot must match.
sg already models them as two different fields for that reason — see [binding.hh](../../shaped-graphics/src/shaped-graphics/binding/binding.hh).

Keeping them separate is a deliberate choice, and it rules one design out.
DXC's `-fvk-*-shift` family would let a shader carry no annotations at all, by mapping HLSL space onto SPIR-V set automatically.
But that ties the two together: a space used purely as a namespace would silently mint another descriptor set.
So the annotation stays, and the prelude carries it.

What follows is the nice part.
Because the space is free and sg builds group layouts from reflection, the macro does not need to emit `register()` at all.
It therefore does not need to know SRV from UAV from CBV, since `[[vk::binding]]` is register-class-agnostic.
One prefix macro replaces the six a register-emitting design would need, and the surface stops being D3D-shaped.

## The authoring surface

```hlsl
// frame_bindings.hlsli
#include "sc/portable.hlsli"

SC_BINDING(0) Texture2D<float4> albedo;
SC_BINDING(0) SamplerState linear_sampler;

SC_BINDING(1) RaytracingAccelerationStructure scene;
SC_BINDING(1) RWTexture2D<float4> output;

SC_INLINE_CONSTANTS(frame_constants, frame);

struct vs_input
{
    SC_VERTEX_INPUT(0) float3 position : POSITION;
    SC_VERTEX_INPUT(1) float3 normal : NORMAL;
};
```

**The index never appears.**
It is the number that is easy to get wrong and impossible to check by eye, so `__COUNTER__` supplies it from declaration order.
The group stays on the line, where it reads with the resource it applies to.

`SC_BINDING_AT(group, index)` pins both, for a slot something outside the shader depends on.
[bindless_tables.cc](../../shaped-viewer/src/shaped-viewer/resources/bindless_tables.cc) is the case that needs it today.

The vocabulary is sg's, not either API's: **group**, not "set" and not "space"; **binding**, not "register"; **inline constants**, not "push constants" or "root constants".
The `SC_` prefix is the repo-wide neutral one, already carried by `sc_add_shader_package` and `SC_THREADS`.
HLSL macros share no namespace with C++ ones, so there is nothing to collide with.

A **group block** — bracketing a run of bindings so the group is stated once — was tried and dropped.
It cannot be a macro pair, for the reason pinned above, so it can only be a raw `#define SC_GROUP 0` / `#undef SC_GROUP` around the run.
That reads worse than the number it saves, especially around a single binding.
It also buys only the structural half of a guarantee the pipeline check below already enforces.

Each binding expands to a marker symbol plus, on SPIR-V only, its annotation:

```hlsl
#define SC_BINDING(group) SC_BINDING_AT(group, __COUNTER__)
#define SC_BINDING_AT(group, index) SC_BINDING_I(group, index)
#define SC_BINDING_I(group, index)                                                               static const uint SC_CAT(SC_CAT(SC_CAT(sc_slot_taken_, group), _), index) = uint(group);     SC_ANNOTATE_BINDING(group, index)
```

`SC_INLINE_CONSTANTS` is the one macro that is a **capability facade** rather than a spelling shortcut.
Today it is a push-constant block on SPIR-V and an auto-assigned `b` register on DXIL.
On a target without push constants — WebGPU — it has to become an ordinary uniform buffer that the backend feeds, and the shader must not have to know.

It supersedes sv's [`InlineConstantBuffer`](../../shaped-viewer/shaders/inline_constant.hlsli), which does the same job for one library and now carries the same `__spirv__` fork.
That fork was missing until the spike found it, and nothing used the macro, which is the only reason a DXIL build never hit it.

`SC_SHADER_RECORD`, for DXR local root signatures and Vulkan's `[[vk::shader_record_ext]]`, is a **TODO**: sg has no local root signatures yet, so there is nothing to be portable about.

## Numbering, and what it asks of you

Three rules come attached to `__COUNTER__`, and they are consequences rather than preferences.

- The counter is **per file**, so a group shared by shaders in **different files must be declared in one shared `.hlsli`**.
  Two files that each declare group 0 both start from zero, and the two shaders disagree about what lives at each slot.
  This is a discipline rather than a guarantee, which is what the pipeline-creation check below is for.
- Indices come out **unique across groups and sparse within one**.
  Legal in Vulkan, and sg uses a binding's declaration position as its slot index rather than its number, so nothing downstream cares.
- **Reordering declarations renumbers them.**
  Harmless while bindings are reflection-derived; it would matter the day precompiled bytecode ships separately from the layout describing it.

## Validation

Three layers, catching three different failures.
None subsumes another.

**Within one file, at shader-compile time: the marker symbols.**
Two bindings claiming one slot is a redefinition error naming the group and index.
This is what makes `SC_BINDING_AT` safe to mix with auto-numbered bindings, which is exactly where a hand-pinned number quietly steps on a counter-assigned one.
Free, and it fires on both targets.

**Across the stages of a pipeline, at pipeline creation: a name ↔ (group, index) bijection.**
This is the real cross-file check, and the data is already in the description.
[raster_pipeline.hh](../../shaped-graphics/src/shaped-graphics/raster/raster_pipeline.hh) holds every stage as a `compiled_shader` carrying its reflected bindings.
[raytracing_pipeline.hh](../../shaped-graphics/src/shaped-graphics/raytracing/raytracing_pipeline.hh) carries the same for ray tracing.
Both directions matter:

- the same `(group, index)` reached by two stages must carry the same name, type and count — the collision above, across files;
- the same *name* in two stages must sit at the same `(group, index)` — because binding groups match by name, so one name at two addresses cannot be satisfied by one group.

Worth having regardless of the prelude: it catches hand-written `register()` collisions just as well.
Ray tracing is where it earns its keep, since a pipeline's shaders naturally live in separate files.
sv is exposed to this today, hand-numbering across files with nothing checking the overlap.
`scene` sits at `t0` in [pathtrace.hlsl](../../shaped-viewer/shaders/pathtrace.hlsl).
`Vertices` and `Indices` take `t2` and `t3` in [mesh.hlsli](../../shaped-viewer/shaders/mesh.hlsli).

**Across targets, at build time: compile every package to every format the toolchain can produce, and compare the reflections.**
Not just the format the local context happens to want.
This is what turns "ships broken on the other backend" into a failing build.
Compare names and kinds only, per the pinned finding above.
Write it from the start to **tolerate a target-specific extra binding**: an emulated inline-constant block is exactly that.
Retrofitting that tolerance later costs more than allowing for it now.

A fourth layer — a package-wide "all reflections must agree" check — is deliberately **not** on the list.
An unrelated compute shader in the same package legitimately reuses group 0 index 0 for something else, so it would fire on correct code, and a check people learn to ignore is worse than no check.

## Compile flags

Four silent divergences that no macro can reach, and that a shader cannot annotate away.
All are `-fvk-*`, so `build_compile_args` adds them to the SPIR-V arm only.
Each makes SPIR-V behave the way the DXIL arm already does, so one source means one behaviour.

- **`-fvk-use-dx-layout`** — without it, cbuffer member offsets can differ between the targets, so one CPU-side struct cannot serve both.
- **`-fvk-support-nonzero-base-vertex`** and **`-fvk-support-nonzero-base-instance`** — Vulkan's `VertexIndex`/`InstanceIndex` include the base, D3D's `SV_VertexID`/`SV_InstanceID` do not.
  Only visible once a draw uses a non-zero base.
- **`-fvk-use-dx-position-w`** — Vulkan's `FragCoord.w` is the reciprocal of D3D's `SV_Position.w`, so any pixel shader reading `.w` is wrong on one of them.

And one flag that must **never** be added: **`-fvk-invert-y`**.
The vulkan backend already flips through a negative-height viewport, so adding the flag would flip twice.
See [vulkan_command_list.raster.cc](../../shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_command_list.raster.cc).

These change the emitted bytecode, so if any ever becomes a per-compile option rather than a constant it has to reach `compute_key` in the shader cache.

## Open

- **Where an emulated inline-constant block lands.**
  Reserving a group index for it now would keep it from renumbering anything an author wrote, but the right answer depends on how the emulation works, so no index is reserved.
- **A linter rule** banning raw `register(` and `[[vk::` outside the prelude.
  The equivalence check catches a *wrong* annotation; only a linter catches a correct-looking one that bypasses the facade and happens to work on the backend that was tested.

## Phasing

1. **[done]** The compile flags, and sv's unguarded `InlineConstantBuffer` — latent breakage that existed already and depended on nothing else here.
2. **[in progress]** The prelude and its macros, mounted at `sc`.
   shaped-rendering's four shaders and both cube examples are ported; sv's seventeen are not.
3. **[done]** The pipeline-creation bijection check, in `try_create_raster_pipeline` and its ray-tracing twin.
4. **[planned]** The all-targets compile and reflection comparison.
5. **[planned]** The linter rule, and whatever a third backend asks for.

Steps 1, 3 and 4 are worth doing even if the prelude never ships.
Step 1 was also the only one that fixed something already wrong rather than preventing something future.

## Why slib owns this

sg owns the vocabulary but not the content: what a project does with a binding model is not sg's business.

sr is above slib, so a prelude there could not serve slib's own test shaders, sg's backend test shaders, or a project that takes slib without sr.

slib is where `shader_language` and `shader_format` meet, and it owns the mount table — the only delivery mechanism available.
[dxc_compiler.cc](../src/shaped-shader-library/compiler/dxc_compiler.cc) is literally the `hlsl` → `dxil` or `spirv` pair the prelude adapts.
A package reaches `sc/portable.hlsli` through the mount-root fallback with no per-application wiring.
The prelude is the source-side half of what registering a compiler means, which makes it a widening of slib's charter rather than a new responsibility.
