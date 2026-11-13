# Reserved bindings

A reserved binding is a resource no program declares and sg binds itself.
A program cannot name one, and the host never hears of one.
`sg_debug`, the window `print` and `assert` write into, is the first, and more are planned.

## The contract

sgl and sg each hold one side, and neither links the other.
This page is the agreement, and two tables state it:

| side | table | what it is for |
|---|---|---|
| sgl | `check::k_reserved_bindings` in [`check/reserved_bindings.hh`](../src/shaped-graphics-language/check/reserved_bindings.hh) | what a compile declares, and which stages reach it |
| sg | `sg::reserved_bindings` in [`binding/reserved_bindings.hh`](../../shaped-graphics/src/shaped-graphics/binding/reserved_bindings.hh) | what each backend binds, and what a reflection may report |

**Neither table is derived from the other, and the build holds them equal.**
slib is the one library that sees both.
[`reserved_contract.cc`](../../shaped-shader-library/src/shaped-shader-library/compiler/reserved_contract.cc) is a `static_assert` over every name and address, and over the debug window's header.
A table edited on one side alone stops the build.

| entry | dx12 | vulkan | WebGPU | metal | enabled by |
|---|---|---|---|---|---|
| `sg_debug` | `u0, space11`, a root UAV | set 3, binding 19 | group 3, binding 19 | `[[buffer(30)]]` | diagnostics |
| `sg_acceleration_pool` | `t0, space12`, a root SRV | set 3, binding 17 | group 3, binding 17 | none | a software trace |
| `sg_acceleration_roots` | `b0, space12`, root constants | set 3, binding 18 | group 3, binding 18 | none | a software trace |
| `sg_array_lengths` | `b1, space12`, root constants | set 3, binding 20 | none | none, the array's table counts it | an unbounded binding array |
| `sg_dispatch_grid` | `b2, space12`, root constants | set 3, binding 21 | group 3, binding 21 | none, metal dispatches the grid's threads | a bare `@compute` |
| `sg_acceleration_addresses` | none | set 3, binding 22 | none | none | a trace in SGL's own SPIR-V |
| `sg_mesh_output` | `u1, space11`, a root UAV | set 3, binding 23 | group 3, binding 23 | `[[buffer(20)]]`, for a geometry stage alone | a polyfilled mesh or geometry stage's compute half |
| `sg_mesh_input` | `t2, space11`, a root SRV | set 3, binding 24 | group 3, binding 24 | `[[buffer(21)]]` | a polyfilled mesh or geometry stage's vertex half |
| `sg_geometry_draw` | `t8, space11` | set 3, binding 25 | group 3, binding 25 | `[[buffer(22)]]` | a polyfilled geometry stage's compute half |
| `sg_geometry_indices` | `t3, space11` | set 3, binding 26 | group 3, binding 26 | `[[buffer(23)]]` | the same |
| `sg_geometry_vertices0` to `3` | `t4` to `t7, space11` | set 3, bindings 27 to 30 | group 3, bindings 27 to 30 | `[[buffer(24)]]` to `[[buffer(27)]]` | the same |
| `sg_tessellation_draw` | `t11, space11` | set 3, binding 33 | group 3, binding 33 | `[[buffer(10)]]` | polyfilled tessellation's compute passes |
| `sg_tessellation_patches` | `u12, space11` | set 3, binding 34 | group 3, binding 34 | `[[buffer(11)]]` | the same |
| `sg_tessellation_controls` | `u13, space11` | set 3, binding 35 | group 3, binding 35 | `[[buffer(12)]]` | the same |
| `sg_tessellation_points` | `u14, space11` | set 3, binding 36 | group 3, binding 36 | `[[buffer(13)]]` | the same |
| `sg_tessellation_indices` | `u15, space11` | set 3, binding 37 | group 3, binding 37 | `[[buffer(14)]]` | the same |
| `sg_tessellation_args` | `u16, space11` | set 3, binding 38 | group 3, binding 38 | `[[buffer(15)]]` | the same |
| `sg_task_output` | `u9, space11`, a root UAV | set 3, binding 31 | group 3, binding 31 | none, metal draws every task stage | a polyfilled task stage's compute half and its scan |
| `sg_task_input` | `t10, space11`, a root SRV | set 3, binding 32 | group 3, binding 32 | none | the compute half of a mesh stage a polyfilled task stage launches |

On metal a ray-tracing pipeline's raygen alone takes `sg_debug` at its slot, and hands it every stage it calls through `sgl_context` (EMIT-183).

`sg_array_lengths` is a block of 16 lengths, `uint4[4]`, one per unbounded binding array of the bound groups in the order of their groups.
sg writes it at each dispatch and draw from the groups bound, beside the roots, and an emit for dx12 or vulkan rewrites `xs.length` into a read of it (CHK-491).

`sg_dispatch_grid` is the threads `dispatch_threads` asked for, `uint4`, which a bare `@compute`'s threads past the grid return before (CHK-520).
A pipeline of such a shader is built over its layout's twin that holds it, `pipeline_layout_description::dispatch_grid`, so no other layout pays for it.

`sg_acceleration_addresses` is every acceleration member's device address, `uint4[8]`, two a vector and each low word first, by the member's position.
That position is the layout's acceleration members group by group, each group's by slot: the numbering the software roots use.
**Every vulkan trace of SGL's reads its structure by it**, through `OpConvertUToAccelerationStructureKHR`, rather than through its descriptor.
That is how vulkan traces a partitioned tlas, which `VK_EXT_descriptor_buffer` has no descriptor for.
- SGL's own SPIR-V declares it in `lowering::acceleration_by_address`, so it has no prelude declaration; an element of an array of structures is a native gap there.
- The HLSL a DXC route compiles declares it as `cbuffer sg_acceleration_addresses_block`, and the conversion as inline SPIR-V, `[[vk::ext_instruction(4447)]]`, which DXC has no builtin for.
  An element of an array of structures still names its descriptor there.
sg's vulkan layout holds it wherever the layout holds an acceleration member, since any package may compile to SGL's own SPIR-V.

`sg_mesh_output` and `sg_mesh_input` are one buffer of words, which a polyfilled mesh stage's compute half writes and its vertex half reads back (CHK-524).
The compute half also reads `sg_dispatch_grid`, the threads sg dispatched it in, to number its workgroups.
[mesh-polyfill.md](../../shaped-graphics/docs/concepts/mesh-polyfill.md) is the protocol: the words, and how sg sizes and draws them.
`sg_task_output` and `sg_task_input` are one more buffer of words, a polyfilled task stage's records, which the same page describes.

A polyfilled geometry stage writes the same words, a primitive's in place of a workgroup's, and reads its draw from the `sg_geometry_*` entries (CHK-525).
`sg_geometry_draw` is a buffer of three `uint4`s rather than a block, since metal has no reserved block, and the protocol page says what each lane holds.
`sg_geometry_indices` and `sg_geometry_vertices0` on are the draw's own index buffer and vertex buffers, bound as words; metal binds them past every vertex-input slot.

Polyfilled tessellation reads its draw from the `sg_geometry_*` entries as a geometry stage does, and keeps its patches and points in the `sg_tessellation_*` ones (CHK-527).
Each is a buffer of words; metal binds them at slots 10 to 15, which a kernel leaves free, having no vertex input.
The protocol page says what each holds, and the evaluation pass writes its points into `sg_mesh_output`, which the draw reads back through `sg_mesh_input`.

## The debug window

The window is 32-bit words, a header of three and then records.

| word | holds |
|---|---|
| 0 | the cursor: the next record's first word, counted from the end of the header |
| 1 | the capacity, in words |
| 2 | how many records did not fit |

A record is the site's id, its own length in words, then each value's lanes in order.
A lane is one word: a float's bits, an int's, a bool as 0 or 1, and a `half` widened to a float first.
A record reserves its words with one atomic add on the cursor, so it is written whole or counted as dropped.
sg decodes it through the site table, which reaches `sg::compiled_shader` as `debug_sites`.

## How a compile reaches one

**An entry with a declaration is an ordinary resource everywhere below the prelude.**

1. `registry` writes each entry's declaration into the prelude as an `@internal` binding, so its types exist and no program finds it (CHK-323).
2. Legalize writes statements over it (LEGAL-70), and lists the binding in `flat_entry_point::reserved`, not in `bindings`, so no group number moves.
3. `plan` appends it to `resources` at the table's address, with `is_reserved` set.
   Those are the only lines that treat it differently: it takes no group and reaches no host list, no layout and no footprint.
4. Each writer declares it as it declares any resource, and metal takes it as a function parameter at its slot.

So neither a text writer nor the IR route holds any logic of `print` or `assert`.
A stage the table says cannot reach an entry has its prints erased, and a failed assert still ends the invocation.

**The acceleration pool and roots are table entries by name and address alone, with no declaration.**
Their addresses, slib's WGSL reader and the reflection exemption all read the tables, but `write_acceleration_pool` still declares them, and a builtin's text still reads them.
What stops the rest of the move is the roots: `cbuffer` on HLSL, root constants on dx12 and a uniform on WebGPU, which `mut buffer[...]` in the prelude does not say.
Nothing below the text reads either yet, since a ray query does not lower natively.

## Adding one

1. Add an entry to `check::k_reserved_bindings`, with its declaration, its address on each target, its stages, and what enables it.
   Its one member's type says what kind of resource it is.
2. Add the same name and addresses to `sg::reserved_bindings`, at the same position.
   The build refuses until the two agree.
3. Regenerate the prelude: `uv run dev.py run sgl -- prelude --write libs/graphics/shaped-graphics-language/prelude/builtins.sgl`.
4. Bind it in each sg backend, at the table's address, in the pipeline layout.
5. Write statements over it in legalize, with `reserved_symbol_of`, `binding_member` and `buffer_element`.

No writer changes.
