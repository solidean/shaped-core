# Portable HLSL

The design for a blessed way to write one `.hlsl` that compiles correctly for every backend.

Today's targets are dx12 (DXIL) and vulkan (SPIR-V), both through DXC.
Metal and WebGPU are intended, and reuse the same two arms — metal-shaderconverter consumes DXIL, Tint consumes SPIR-V — so a third arm is not automatically a third spelling.

Three things diverge between the targets, and each has an answer of its own:

- **Bindings** — SPIR-V has none of HLSL's implicit addressing, and DXIL's implicit addressing is not stable across the stages of one pipeline.
  Answered by the [binding preprocessor](binding-preprocessor.md): the shader declares groups as annotated namespaces, and a rewriting pass writes every address.
- **Vertex input locations** — sg identifies an attribute by its HLSL semantic and SPIR-V has none.
  Answered by the same pass, through its [`vertex_input` attribute](binding-preprocessor.md#vertex_input); the shader writes a `__spirv__` fork by hand until it lands.
- **Silent behavioural divergences** — cbuffer layout, base vertex, `SV_Position.w`.
  Answered by the [compile flags](#compile-flags), which have landed.

Every claim below marked "pinned" is asserted by [portable-hlsl-spike-test.cc](../../shaped-shader-compiler-dxc/tests/portable-hlsl-spike-test.cc).
A DXC upgrade that changes one of them fails a test rather than a shader.

## What DXC actually does

The findings the design rests on, in the order they constrain it.

**Without an explicit `register()`, DXIL addresses are not stable across a pipeline.**
DXC assigns a register only to what an entry point actually references, so two stages sharing one declaration disagree about its address when one of them reads a resource the other does not.
Neither stage wrote either number, and nothing downstream can notice.
This is the finding that decided everything else: the address has to be written into the source.
[pinned]

**With an explicit `register()`, the two stages agree and the DXIL index equals the SPIR-V one.**
So a group's slot, its declaration order and `sg::binding::index` can be one number, and a cross-target check may compare addresses rather than only names.
[pinned]

**A resource declaration may live in an HLSL namespace on both targets, and reflection reports the bare name.**
`frame::albedo` reaches sg as `albedo`, so a namespace can carry the group with no change to sg's name matching at all.
Two namespaces may declare the same symbol, so a namespace catches no duplicate slot — the pass detects collisions itself.
[pinned]

**An unguarded `[[vk::…]]` attribute is a hard error on the DXIL target.**
`[[vk::binding]]`, `[[vk::push_constant]]` and `[[vk::location]]` each produce `'<attr>' attribute ignored [-Werror,-Wignored-attributes]`, and ssc compiles with `-WX` by default.
So there is no "write it once, DXC ignores it where it does not apply" — every annotation is written for exactly one target.
Its failure mode is a shader that builds on the backend you tested and not on the other, and nothing catches it at build time, because shader compilation happens at runtime.
[pinned]

**A macro cannot emit a preprocessor directive, and `register()` is a suffix while the Vulkan annotation is a prefix.**
Together these rule out the macro prelude this document used to describe.
[binding-preprocessor.md](binding-preprocessor.md#why-not-macros) carries the argument in full, because it is the same argument a future session would have to re-derive.
[pinned]

## Space is not set

A DXIL register space is a namespace for register numbers.
A SPIR-V set is a hardware-visible descriptor set that the bind slot must match.
sg already models them as two different fields for that reason — see [binding.hh](../../shaped-graphics/src/shaped-graphics/binding/binding.hh).

Keeping them separate is a deliberate choice, and it rules one design out.
DXC's `-fvk-*-shift` family would let a shader carry no annotations at all, by mapping HLSL space onto SPIR-V set automatically.
But that ties the two together: a space used purely as a namespace would silently mint another descriptor set.

The binding preprocessor ties them instead, deliberately and one way: a group's number is both its SPIR-V set and its HLSL space, so group `n` occupies `space<n>` and nothing else does.
That is a rule about what a *group* is, not a global mapping DXC applies to every space — a binding declared outside an annotated namespace keeps whatever space it wrote by hand.

## Vertex input locations

sg identifies a vertex attribute by its HLSL semantic, SPIR-V has no semantics, and the vulkan backend therefore falls back to the attribute's position in the layout.
So a Vulkan-targeted shader spells its locations out today, in the order sg's vertex layout lists them, and gets them right by hand:

```hlsl
#ifdef __spirv__
#define VK_LOCATION(n) [[vk::location(n)]]
#else
#define VK_LOCATION(n)
#endif

struct vs_input
{
    VK_LOCATION(0) float3 position : POSITION;
    VK_LOCATION(1) float3 normal : NORMAL;
};
```

A mismatch is silent: the pipeline builds and the geometry is wrong.
It is Q8's class of failure with no assertion anywhere: the two orders are written twice, in two languages, and nothing compares them.

The [`vertex_input` attribute](binding-preprocessor.md#vertex_input) takes it over, numbering a struct's members by declaration order the way the pass numbers a group's bindings.
That numbering is safe here in a way it is not for a group: a vertex input struct is declared once, in one block, where a group has to survive being shared across files.
The same parse then emits the `sg::vertex_layout_of` specialization, so the C++ side stops restating what the shader already said.

## Inline constants

`pipeline_layout_description::inline_constants` is a push-constant range on SPIR-V and root constants on DXIL, and a plain `ConstantBuffer` is neither.
Under SPIR-V it becomes a descriptor in a set that the pipeline layout never binds, so a shader that does not say what it wants declares a resource nothing feeds.

Today the shader forks by hand, `[[vk::push_constant]]` against `register(b0)`.
The [`push_constants` attribute](binding-preprocessor.md#push_constants) replaces that fork.
It also adds the thing a fork cannot give: the space is stated, so an inline-constants block cannot collide with a group's `b` registers.

## Validation

Three layers, catching three different failures.
None subsumes another.

**Within one translation unit, in the pass: duplicate declarations and duplicate addresses.**
A namespace gives no protection of its own, per the pinned finding above, so the pass reports a collision itself and names the line.
[done once the pass lands]

**Across the stages of a pipeline, at pipeline creation: a name ↔ (group, index) bijection.**
This is the cross-file check, and the data is already in the description.
[raster_pipeline.hh](../../shaped-graphics/src/shaped-graphics/raster/raster_pipeline.hh) holds every stage as a `compiled_shader` carrying its reflected bindings.
[raytracing_pipeline.hh](../../shaped-graphics/src/shaped-graphics/raytracing/raytracing_pipeline.hh) carries the same for ray tracing.
Both directions matter:

- the same `(group, index)` reached by two stages must carry the same name, type and count;
- the same *name* in two stages must sit at the same `(group, index)`, because binding groups match by name, so one name at two addresses cannot be satisfied by one group.

It catches hand-written `register()` collisions just as well, which is why it was worth having before the pass existed.
Ray tracing is where it earns its keep, since a pipeline's shaders naturally live in separate files.
sv is exposed to this today, hand-numbering across files with nothing checking the overlap.
`scene` sits at `t0` in [pathtrace.hlsl](../../shaped-viewer/shaders/pathtrace.hlsl), and `Vertices` and `Indices` take `t2` and `t3` in [mesh.hlsli](../../shaped-viewer/shaders/mesh.hlsli).
[done]

**Across targets, at build time: compile every package to every format the toolchain can produce, and compare the reflections.**
Not just the format the local context happens to want.
This is what turns "ships broken on the other backend" into a failing build.
Because the pass writes the address, this compares addresses and not only names — which is the part that would have caught the DXIL drift above.
Write it from the start to **tolerate a target-specific extra binding**: an emulated inline-constant block is exactly that.
Retrofitting that tolerance later costs more than allowing for it now.
[planned]

A fourth layer — a package-wide "all reflections must agree" check — is deliberately **not** on the list.
An unrelated compute shader in the same package legitimately reuses group 0 index 0 for something else, so it would fire on correct code, and a check people learn to ignore is worse than no check.

## Compile flags

Four silent divergences that no annotation can reach.
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

- **Where an emulated inline-constant block lands** on a target with neither push nor root constants.
  Reserving a group index for it now would keep it from renumbering anything an author wrote, but the right answer depends on how the emulation works, so no index is reserved.
- **`SC_SHADER_RECORD`**, for DXR local root signatures and Vulkan's `[[vk::shader_record_ext]]`.
  sg has no local root signatures yet, so there is nothing to be portable about.
- **A linter rule** banning raw `register(` and `[[vk::` inside an annotated namespace.
  The cross-target check catches a *wrong* annotation; only a linter catches a correct-looking one that bypasses the pass and happens to work on the backend that was tested.

## Phasing

1. **[done]** The compile flags, and sv's unguarded `InlineConstantBuffer` — latent breakage that existed already and depended on nothing else here.
2. **[done]** The pipeline-creation bijection check, in `try_create_raster_pipeline` and its ray-tracing twin.
3. **[in progress]** The binding preprocessor — its own [phasing](binding-preprocessor.md#phasing) is the detailed one.
4. **[planned]** The all-targets compile and reflection comparison.
5. **[planned]** The linter rule, and whatever a third backend asks for.

Steps 1, 2 and 4 are worth doing even if no pass ever ships.
Step 1 was also the only one that fixed something already wrong rather than preventing something future.

## Why slib owns this

sg owns the vocabulary but not the content: what a project does with a binding model is not sg's business.

sr is above slib, so a pass there could not serve slib's own test shaders, sg's backend test shaders, or a project that takes slib without sr.

slib is where `shader_language` and `shader_format` meet, and it owns the compile path every shader travels — `_compile_text`, between the include flatten and the compile.
[dxc_compiler.cc](../src/shaped-shader-library/compiler/dxc_compiler.cc) is literally the `hlsl` → `dxil` or `spirv` pair the pass adapts.
It is also where the build-time generator already lives, since `sc_add_shader_package` is slib's.
That makes source-side portability a widening of slib's charter rather than a new responsibility.
